/* libc: TLSF-inspired memory allocator with O(1) allocation/deallocation.
 *
 * Two-Level Segregated Fit (TLSF) allocator optimized for RTOS with:
 * - O(1) malloc() and free() via bitmap-based segregated free lists
 * - Bounded interrupt disable duration (no linear searches)
 * - Immediate coalescing on free() to minimize fragmentation
 * - Low memory overhead (~8 bytes per block)
 *
 * Performance characteristics:
 * - malloc(): O(1) - bitmap lookup + list pop
 * - free():   O(1) - coalesce + list insert
 * - realloc(): O(1) best case (in-place), O(n) worst case (copy)
 *
 * Reference:
 *   M. Masmano, I. Ripoll, A. Crespo, and J. Real.
 *   TLSF: a new dynamic memory allocator for real-time systems.
 *   In Proc. ECRTS (2004), IEEE Computer Society, pp. 79-86.
 */

#include <lib/libc.h>
#include <lib/malloc.h>
#include <sys/task.h>
#include <types.h>

#include "../config.h"
#include "private/error.h"
#include "private/utils.h"

/* TLSF Configuration
 *
 * Memory is organized into segregated free lists indexed by two levels:
 * - First Level (FL): log2(size) - power-of-2 size classes
 * - Second Level (SL): 4 subdivisions within each FL class
 *
 * This provides O(1) block lookup via bitmap operations.
 */

/* Alignment must be at least 4 bytes for RISC-V */
#define ALIGN_SIZE 4
#define ALIGN_SIZE_LOG2 2

/* Second level index: 4 subdivisions per first-level class */
#define SL_INDEX_COUNT 4
#define SL_INDEX_COUNT_LOG2 2

/* First level index shift: log2(SL_INDEX_COUNT * ALIGN_SIZE) = 4 */
#define FL_INDEX_SHIFT (SL_INDEX_COUNT_LOG2 + ALIGN_SIZE_LOG2)

/* First level index count: configurable via CONFIG_TLSF_FL_INDEX_MAX
 * See config.h for configuration options and memory overhead tradeoffs.
 */
#define FL_INDEX_MAX CONFIG_TLSF_FL_INDEX_MAX
#define FL_INDEX_COUNT (FL_INDEX_MAX - FL_INDEX_SHIFT + 1)

/* Small block threshold: blocks smaller than this use direct indexing */
#define SMALL_BLOCK_SIZE (1 << FL_INDEX_SHIFT)

/* Block header flags (stored in low bits of size field) */
#define BLOCK_BIT_FREE (1 << 0)
#define BLOCK_BIT_PREV_FREE (1 << 1)

/* Mask for extracting actual size from header */
#define BLOCK_SIZE_MASK (~(BLOCK_BIT_FREE | BLOCK_BIT_PREV_FREE))

/* Overhead: prev_phys + header fields (8 bytes on RV32) */
#define BLOCK_HEADER_OVERHEAD (sizeof(block_t *) + sizeof(size_t))

/* Minimum block size: space for free list pointers when block is freed */
#define BLOCK_SIZE_MIN (sizeof(block_t *) + sizeof(block_t *))

/* Block structure - 16 bytes when free, 8 bytes overhead when allocated */
typedef struct block {
    /* Previous physical block (for backward coalescing) */
    struct block *prev_phys;
    /* Size + flags: bit 0 = free, bit 1 = prev_free */
    size_t header;
    /* Free list links - only valid when block is free */
    struct block *next_free, *prev_free;
} block_t;

/* TLSF control structure */
typedef struct {
    /* Null block for list termination */
    block_t block_null;

    /* First-level bitmap: bit i set if fl_bitmap[i] has free blocks */
    uint32_t fl_bitmap;

    /* Second-level bitmaps: bit j set if blocks[i][j] has free blocks */
    uint32_t sl_bitmap[FL_INDEX_COUNT];

    /* Free block lists indexed by [fl][sl] */
    block_t *blocks[FL_INDEX_COUNT][SL_INDEX_COUNT];
} tlsf_t;

/* Global TLSF control structure */
static tlsf_t tlsf_control;
static void *heap_start, *heap_end;

/* Bit manipulation helpers - branchless binary search
 *
 * These use mask-based shifts to avoid conditional branches, compiling to
 * sltu/andi/srl sequences on RV32I. This provides predictable timing for
 * real-time systems without:
 * - Multiplication (heavy on RV32I without M extension)
 * - libgcc dependencies (__clzsi2, __ctzsi2)
 * - Branch misprediction penalties
 */

/* Find last set bit (1-indexed), 0 if no bits set */
static inline int fls(uint32_t x)
{
    if (!x)
        return 0;

    int r = 1;
    uint32_t m;

    m = !!(x & 0xFFFF0000u);
    r += (int) (m << 4);
    x >>= (m << 4);
    m = !!(x & 0x0000FF00u);
    r += (int) (m << 3);
    x >>= (m << 3);
    m = !!(x & 0x000000F0u);
    r += (int) (m << 2);
    x >>= (m << 2);
    m = !!(x & 0x0000000Cu);
    r += (int) (m << 1);
    x >>= (m << 1);
    r += (int) !!(x & 0x00000002u);

    return r;
}

/* Find first set bit (0-indexed), -1 if no bits set */
static inline int ffs_bit(uint32_t x)
{
    if (!x)
        return -1;

    int r = 0;
    uint32_t m;

    m = ((x & 0x0000FFFFu) == 0);
    r += (int) (m << 4);
    x >>= (m << 4);
    m = ((x & 0x000000FFu) == 0);
    r += (int) (m << 3);
    x >>= (m << 3);
    m = ((x & 0x0000000Fu) == 0);
    r += (int) (m << 2);
    x >>= (m << 2);
    m = ((x & 0x00000003u) == 0);
    r += (int) (m << 1);
    x >>= (m << 1);
    r += (int) ((x & 0x00000001u) == 0);

    return r;
}

/* Block accessor macros */
#define block_size(b) ((b)->header & BLOCK_SIZE_MASK)
#define block_is_free(b) ((b)->header & BLOCK_BIT_FREE)
#define block_is_prev_free(b) ((b)->header & BLOCK_BIT_PREV_FREE)
#define block_set_free(b) ((b)->header |= BLOCK_BIT_FREE)
#define block_set_used(b) ((b)->header &= ~BLOCK_BIT_FREE)
#define block_set_prev_free(b) ((b)->header |= BLOCK_BIT_PREV_FREE)
#define block_set_prev_used(b) ((b)->header &= ~BLOCK_BIT_PREV_FREE)

/* Get pointer to block from user pointer.
 * User pointer points to next_free field (offset = prev_phys + header = 8
 * bytes)
 */
static inline block_t *block_from_ptr(const void *ptr)
{
    return (block_t *) ((char *) ptr - sizeof(block_t *) - sizeof(size_t));
}

/* Get user pointer from block */
static inline void *block_to_ptr(const block_t *block)
{
    return (void *) &block->next_free;
}

/* Get next physical block */
static inline block_t *block_next(const block_t *block)
{
    return (block_t *) ((char *) block_to_ptr(block) + block_size(block));
}

/* Link next physical block's prev_phys pointer */
static inline void block_link_next(block_t *block)
{
    block_t *next = block_next(block);
    next->prev_phys = block;
}

/* Mark next physical block's prev_free flag */
static inline void block_mark_as_free(block_t *block)
{
    block_t *next = block_next(block);
    block_set_prev_free(next);
}

/* Clear next physical block's prev_free flag */
static inline void block_mark_as_used(block_t *block)
{
    block_t *next = block_next(block);
    block_set_prev_used(next);
}

/* TLSF mapping functions - O(1) size to index conversion
 *
 * Maps a size to first-level and second-level indices:
 * - fl = log2(size) - FL_INDEX_SHIFT
 * - sl = (size >> (fl + FL_INDEX_SHIFT - SL_INDEX_COUNT_LOG2)) & (SL_COUNT-1)
 */

static inline void mapping_insert(size_t size, int *fl, int *sl)
{
    if (size < SMALL_BLOCK_SIZE) {
        /* Small blocks: fl=0, sl=size/ALIGN_SIZE */
        *fl = 0;
        *sl = (int) size / ALIGN_SIZE;
    } else {
        int t = fls((uint32_t) size) - 1;
        *sl = (int) (size >> (t - SL_INDEX_COUNT_LOG2)) ^ SL_INDEX_COUNT;
        *fl = t - FL_INDEX_SHIFT + 1;

        /* Clamp to valid index range for very large sizes.
         * This handles sizes > 2^FL_INDEX_MAX which would overflow the
         * segregated list arrays. All such sizes map to the largest bucket.
         */
        if (*fl >= FL_INDEX_COUNT) {
            *fl = FL_INDEX_COUNT - 1;
            *sl = SL_INDEX_COUNT - 1;
        }
    }
}

/* Round up size for searching - ensures we find a block >= requested size */
static inline void mapping_search(size_t size, int *fl, int *sl)
{
    if (size >= SMALL_BLOCK_SIZE) {
        size_t round =
            (1 << (fls((uint32_t) size) - 1 - SL_INDEX_COUNT_LOG2)) - 1;
        size += round;
    }
    mapping_insert(size, fl, sl);
}

/* Free list management - O(1) insert and remove */

/* Remove block from its free list */
static inline void block_remove(tlsf_t *t, block_t *block)
{
    int fl, sl;
    mapping_insert(block_size(block), &fl, &sl);

    /* Bounds check: invalid indices indicate heap corruption or code bug.
     * Fail-fast with panic rather than silent return to aid debugging.
     */
    if (unlikely(!RANGE_CHECK(fl, 0, FL_INDEX_COUNT) ||
                 !RANGE_CHECK(sl, 0, SL_INDEX_COUNT))) {
        panic(ERR_HEAP_CORRUPT);
        return;
    }

    block_t *prev = block->prev_free;
    block_t *next = block->next_free;

    next->prev_free = prev;
    prev->next_free = next;

    /* Update bitmaps if list is now empty */
    if (t->blocks[fl][sl] == block) {
        t->blocks[fl][sl] = (next == &t->block_null) ? NULL : next;
        if (!t->blocks[fl][sl]) {
            t->sl_bitmap[fl] &= ~(1U << sl);
            if (!t->sl_bitmap[fl])
                t->fl_bitmap &= ~(1U << fl);
        }
    }
}

/* Insert block into appropriate free list */
static inline void block_insert(tlsf_t *t, block_t *block)
{
    int fl, sl;
    mapping_insert(block_size(block), &fl, &sl);

    /* Bounds check: invalid indices indicate heap corruption or code bug.
     * Fail-fast with panic rather than silent return to aid debugging.
     */
    if (unlikely(!RANGE_CHECK(fl, 0, FL_INDEX_COUNT) ||
                 !RANGE_CHECK(sl, 0, SL_INDEX_COUNT))) {
        panic(ERR_HEAP_CORRUPT);
        return;
    }

    block_t *current = t->blocks[fl][sl];
    block->next_free = current ? current : &t->block_null;
    block->prev_free = &t->block_null;

    if (current)
        current->prev_free = block;

    t->blocks[fl][sl] = block;
    t->fl_bitmap |= (1U << fl);
    t->sl_bitmap[fl] |= (1U << sl);
}

/* Block splitting and merging - O(1) operations */

/* Check if block can be split */
static inline bool block_can_split(block_t *block, size_t size)
{
    return block_size(block) >= size + BLOCK_SIZE_MIN + BLOCK_HEADER_OVERHEAD;
}

/* Split block, return remainder */
static inline block_t *block_split(block_t *block, size_t size)
{
    block_t *remaining = (block_t *) ((char *) block_to_ptr(block) + size);

    size_t remain_size = block_size(block) - size - BLOCK_HEADER_OVERHEAD;

    remaining->header = remain_size;
    remaining->prev_phys = block;
    block->header =
        size | (block->header & (BLOCK_BIT_FREE | BLOCK_BIT_PREV_FREE));

    block_link_next(remaining);

    return remaining;
}

/* Absorb next block into current block */
static inline block_t *block_absorb(block_t *prev, block_t *block)
{
    size_t new_size =
        block_size(prev) + block_size(block) + BLOCK_HEADER_OVERHEAD;

    /* Bounds validation: merged size must not exceed heap bounds.
     * A corrupted block header could cause arbitrarily large sizes.
     */
    if (unlikely((char *) block_to_ptr(prev) + new_size > (char *) heap_end)) {
        panic(ERR_HEAP_CORRUPT);
        return prev; /* Unreachable after panic, but satisfies return type */
    }

    prev->header =
        new_size | (prev->header & (BLOCK_BIT_FREE | BLOCK_BIT_PREV_FREE));
    block_link_next(prev);
    return prev;
}

/* Merge with previous physical block if free */
static inline block_t *block_merge_prev(tlsf_t *t, block_t *block)
{
    if (block_is_prev_free(block)) {
        block_t *prev = block->prev_phys;
        block_remove(t, prev);
        block = block_absorb(prev, block);
    }
    return block;
}

/* Merge with next physical block if free */
static inline block_t *block_merge_next(tlsf_t *t, block_t *block)
{
    block_t *next = block_next(block);
    if (block_is_free(next)) {
        block_remove(t, next);
        block = block_absorb(block, next);
    }
    return block;
}

/* Block location - O(1) via bitmap search */

/* Find suitable free block for requested size */
static inline block_t *block_find_free(tlsf_t *t, size_t size)
{
    int fl, sl;
    mapping_search(size, &fl, &sl);

    /* Defensive bounds check using fast bit-op range check */
    if (!RANGE_CHECK(fl, 0, FL_INDEX_COUNT) ||
        !RANGE_CHECK(sl, 0, SL_INDEX_COUNT))
        return NULL;

    /* Search for block in same or larger size class */
    uint32_t sl_map = t->sl_bitmap[fl] & (~0U << sl);
    if (!sl_map) {
        /* No block in first level, search larger classes */
        uint32_t fl_map = t->fl_bitmap & (~0U << (fl + 1));
        if (!fl_map)
            return NULL; /* No suitable block found */

        fl = ffs_bit(fl_map);
        /* Bounds check on fl from bitmap lookup */
        if (!RANGE_CHECK(fl, 0, FL_INDEX_COUNT))
            return NULL;
        sl_map = t->sl_bitmap[fl];
    }

    sl = ffs_bit(sl_map);
    /* Bounds check on sl from bitmap lookup */
    if (!RANGE_CHECK(sl, 0, SL_INDEX_COUNT))
        return NULL;

    return t->blocks[fl][sl];
}

/* Block preparation for allocation */

static inline void *block_prepare_used(tlsf_t *t, block_t *block, size_t size)
{
    if (!block)
        return NULL;

    block_remove(t, block);

    /* Split if block is much larger than needed */
    if (block_can_split(block, size)) {
        block_t *remaining = block_split(block, size);
        block_set_free(remaining);
        block_insert(t, remaining);
        block_mark_as_free(remaining);
    } else {
        block_mark_as_used(block);
    }

    block_set_used(block);
    return block_to_ptr(block);
}

/* Public API - Standard C allocation interface */

/* Adjust size for alignment and minimum requirements */
static inline size_t adjust_size(size_t size)
{
    if (size < BLOCK_SIZE_MIN)
        size = BLOCK_SIZE_MIN;
    return ALIGN4(size);
}

/* O(1) allocation */
void *malloc(uint32_t size)
{
    if (unlikely(!size || size > MALLOC_MAX_SIZE))
        return NULL;

    size = adjust_size(size);

    CRITICAL_ENTER();
    block_t *block = block_find_free(&tlsf_control, size);
    void *ptr = block_prepare_used(&tlsf_control, block, size);
    CRITICAL_LEAVE();

    return ptr;
}

/* O(1) deallocation with immediate coalescing */
void free(void *ptr)
{
    if (!ptr)
        return;

    CRITICAL_ENTER();

    block_t *block = block_from_ptr(ptr);

    /* Validate block is within heap bounds */
    if (unlikely((void *) block < heap_start || (void *) block >= heap_end)) {
        CRITICAL_LEAVE();
        panic(ERR_HEAP_CORRUPT);
        return;
    }

    /* Validate block header sanity - size must not exceed heap bounds */
    size_t bsize = block_size(block);
    if (unlikely(bsize == 0 ||
                 (void *) ((char *) block_to_ptr(block) + bsize) > heap_end)) {
        CRITICAL_LEAVE();
        panic(ERR_HEAP_CORRUPT);
        return;
    }

    /* Validate block is currently used (not double-free) */
    if (unlikely(block_is_free(block))) {
        CRITICAL_LEAVE();
        panic(ERR_HEAP_CORRUPT);
        return;
    }

    block_set_free(block);

    /* Coalesce with adjacent free blocks */
    block = block_merge_prev(&tlsf_control, block);
    block = block_merge_next(&tlsf_control, block);

    /* Insert merged block into free list */
    block_insert(&tlsf_control, block);
    block_mark_as_free(block);

    CRITICAL_LEAVE();
}

/* Zero-initialized allocation with overflow protection */
void *calloc(uint32_t nmemb, uint32_t size)
{
    /* Check for multiplication overflow */
    if (unlikely(nmemb && size > MALLOC_MAX_SIZE / nmemb))
        return NULL;

    uint32_t total = nmemb * size;
    void *ptr = malloc(total);

    if (ptr)
        memset(ptr, 0, total);

    return ptr;
}

/* Reallocation with in-place optimization */
void *realloc(void *ptr, uint32_t size)
{
    if (!ptr)
        return malloc(size);

    if (!size) {
        free(ptr);
        return NULL;
    }

    if (unlikely(size > MALLOC_MAX_SIZE))
        return NULL;

    size = adjust_size(size);

    CRITICAL_ENTER();

    block_t *block = block_from_ptr(ptr);

    /* Validate block */
    if (unlikely((void *) block < heap_start || (void *) block >= heap_end ||
                 block_is_free(block))) {
        CRITICAL_LEAVE();
        panic(ERR_HEAP_CORRUPT);
        return NULL;
    }

    size_t cur_size = block_size(block);

    /* Shrinking: potentially split block */
    if (size <= cur_size) {
        if (block_can_split(block, size)) {
            block_t *remaining = block_split(block, size);
            block_set_free(remaining);
            remaining = block_merge_next(&tlsf_control, remaining);
            block_insert(&tlsf_control, remaining);
            block_mark_as_free(remaining);
        }
        CRITICAL_LEAVE();
        return ptr;
    }

    /* Growing: try to absorb next block */
    block_t *next = block_next(block);
    if (block_is_free(next) &&
        cur_size + block_size(next) + BLOCK_HEADER_OVERHEAD >= size) {
        block_remove(&tlsf_control, next);
        block = block_absorb(block, next);

        if (block_can_split(block, size)) {
            block_t *remaining = block_split(block, size);
            block_set_free(remaining);
            block_insert(&tlsf_control, remaining);
            block_mark_as_free(remaining);
        } else {
            block_mark_as_used(block);
        }
        CRITICAL_LEAVE();
        return ptr;
    }

    CRITICAL_LEAVE();

    /* Fall back to malloc + copy + free.
     *
     * Design note: We release the lock before the slow path to allow other
     * allocations during the copy. This is safe because:
     * 1. cur_size is a local copy captured while holding the lock
     * 2. ptr remains marked as "used" in TLSF until our free(ptr) call
     * 3. Other malloc calls cannot return this memory until after free(ptr)
     *
     * The caller must ensure ptr is not freed or modified by another thread
     * during realloc (standard realloc semantics).
     */
    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, min(cur_size, size));
        free(ptr);
    }

    return new_ptr;
}

/* Initialize TLSF heap */
void mo_heap_init(size_t *zone, uint32_t len)
{
    if (unlikely(!zone || len < sizeof(block_t) * 3))
        return;

    tlsf_t *t = &tlsf_control;

    /* Initialize control structure */
    memset(t, 0, sizeof(*t));
    t->block_null.next_free = &t->block_null;
    t->block_null.prev_free = &t->block_null;

    /* Align pool start and end */
    uintptr_t pool_start = ALIGN4((uintptr_t) zone);
    uintptr_t pool_end = ((uintptr_t) zone + len) & ~(ALIGN_SIZE - 1);

    if (pool_end <= pool_start + sizeof(block_t) * 2)
        return;

    heap_start = (void *) pool_start;
    heap_end = (void *) pool_end;

    /* Create initial free block spanning entire pool */
    block_t *block = (block_t *) pool_start;
    size_t block_size_val = pool_end - pool_start - BLOCK_HEADER_OVERHEAD * 2;

    block->header = block_size_val | BLOCK_BIT_FREE;
    block->prev_phys = NULL;
    block_link_next(block);

    /* Create sentinel block at end */
    block_t *sentinel = block_next(block);

    /* Runtime validation: sentinel must be within heap bounds.
     * This catches off-by-one errors in block_size_val calculation.
     */
    if (unlikely((void *) sentinel >= heap_end ||
                 (void *) sentinel < heap_start)) {
        panic(ERR_HEAP_CORRUPT);
        return;
    }

    sentinel->header = 0; /* Size 0, used, prev_free */
    sentinel->prev_phys = block;
    sentinel->next_free = NULL; /* Clear for debugging clarity */
    sentinel->prev_free = NULL;
    block_set_prev_free(sentinel);

    /* Insert initial block into free list */
    block_insert(t, block);
}
