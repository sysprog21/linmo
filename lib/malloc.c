#include <lib/libc.h>
#include <sys/task.h>
#include <types.h>
#include <spinlock.h>

#include "private/utils.h"

/* Memory allocator using first-fit strategy with selective coalescing.
 *
 * Performance characteristics:
 * - malloc(): O(n) worst case; searches linearly from heap start; coalesces
 *             free blocks when fragmentation threshold is reached.
 * - free(): O(1) average case; marks memory areas as unused with immediate
 *           forward coalescing and optional backward coalescing.
 *
 * This implementation prioritizes fast allocation/deallocation with proper
 * fragmentation management to minimize memory waste.
 */

typedef struct __memblock {
    struct __memblock *next; /* pointer to the next block */
    size_t size;             /* block size, LSB = used flag */
} memblock_t;

static memblock_t *first_free;
static void *heap_start, *heap_end;
static uint32_t free_blocks_count; /* track fragmentation */
static spinlock_t malloc_lock = SPINLOCK_INITIALIZER;
static uint32_t malloc_flags = 0;

/* Block manipulation macros */
#define IS_USED(b) ((b)->size & 1L)
#define GET_SIZE(b) ((b)->size & ~1L)
#define MARK_USED(b) ((b)->size |= 1L)
#define MARK_FREE(b) ((b)->size &= ~1L)

/* Memory layout validation */
#define IS_VALID_BLOCK(b)                                     \
    ((void *) (b) >= heap_start && (void *) (b) < heap_end && \
     (size_t) (b) % sizeof(size_t) == 0)

/* Fragmentation threshold - coalesce when free blocks exceed this ratio */
#define COALESCE_THRESHOLD 8

/* Validate block integrity */
static inline bool validate_block(memblock_t *block)
{
    if (!IS_VALID_BLOCK(block))
        return false;

    size_t size = GET_SIZE(block);
    if (!size || size > MALLOC_MAX_SIZE)
        return false;

    /* Check if block extends beyond heap */
    if ((uint8_t *) block + sizeof(memblock_t) + size > (uint8_t *) heap_end)
        return false;

    return true;
}

/* O(1) with immediate forward coalescing, conditional backward coalescing */
void free(void *ptr)
{
    if (!ptr)
        return;

    spin_lock_irqsave(&malloc_lock, &malloc_flags);

    memblock_t *p = ((memblock_t *) ptr) - 1;

    /* Validate the block being freed */
    if (!validate_block(p) || !IS_USED(p)) {
        spin_unlock_irqrestore(&malloc_lock, malloc_flags);
        return; /* Invalid or double-free */
    }

    MARK_FREE(p);
    free_blocks_count++;

    /* Forward merge if the next block is free and physically adjacent */
    if (p->next && !IS_USED(p->next) &&
        (uint8_t *) p + sizeof(memblock_t) + GET_SIZE(p) ==
            (uint8_t *) p->next) {
        p->size = GET_SIZE(p) + sizeof(memblock_t) + GET_SIZE(p->next);
        p->next = p->next->next;
        free_blocks_count--;
    }

    /* Backward merge: optimized single-pass search with early termination */
    memblock_t *prev = NULL;
    memblock_t *current = first_free;
    while (current && current != p) {
        prev = current;
        current = current->next;
    }

    if (prev && !IS_USED(prev) &&
        (uint8_t *) prev + sizeof(memblock_t) + GET_SIZE(prev) ==
            (uint8_t *) p) {
        prev->size = GET_SIZE(prev) + sizeof(memblock_t) + GET_SIZE(p);
        prev->next = p->next;
        free_blocks_count--;
    }

    spin_unlock_irqrestore(&malloc_lock, malloc_flags);
}

/* Selective coalescing: only when fragmentation becomes significant */
static void selective_coalesce(void)
{
    memblock_t *p = first_free;
    uint32_t coalesced = 0;

    while (p && p->next) {
        /* Merge only when blocks are FREE *and* adjacent in memory */
        uint8_t *pend = (uint8_t *) p + sizeof(memblock_t) + GET_SIZE(p);
        if (!IS_USED(p) && !IS_USED(p->next) && pend == (uint8_t *) p->next) {
            p->size = GET_SIZE(p) + sizeof(memblock_t) + GET_SIZE(p->next);
            p->next = p->next->next;
            coalesced++;
            free_blocks_count--;
        } else {
            p = p->next;
        }
    }
}

/* O(n) first-fit allocation with selective coalescing */
void *malloc(uint32_t size)
{
    /* Input validation */
    if (unlikely(!size || size > MALLOC_MAX_SIZE))
        return NULL;

    size = ALIGN4(size);

    /* Ensure minimum allocation size */
    if (size < MALLOC_MIN_SIZE)
        size = MALLOC_MIN_SIZE;

    spin_lock_irqsave(&malloc_lock, &malloc_flags);

    /* Trigger coalescing only when fragmentation is high */
    if (free_blocks_count > COALESCE_THRESHOLD)
        selective_coalesce();

    memblock_t *p = first_free;
    while (p) {
        if (!validate_block(p)) {
            spin_unlock_irqrestore(&malloc_lock, malloc_flags);
            return NULL; /* Heap corruption detected */
        }

        if (!IS_USED(p) && GET_SIZE(p) >= size) {
            size_t remaining = GET_SIZE(p) - size;

            /* Split block only if remainder is large enough to be useful */
            if (remaining >= sizeof(memblock_t) + MALLOC_MIN_SIZE) {
                memblock_t *new_block =
                    (memblock_t *) ((size_t) p + sizeof(memblock_t) + size);
                new_block->next = p->next;
                new_block->size = remaining - sizeof(memblock_t);
                MARK_FREE(new_block);
                p->next = new_block;
                p->size = size;
                free_blocks_count++; /* New free block created */
            }

            MARK_USED(p);
            if (free_blocks_count > 0)
                free_blocks_count--;

            spin_unlock_irqrestore(&malloc_lock, malloc_flags);
            return (void *) (p + 1);
        }
        p = p->next;
    }

    spin_unlock_irqrestore(&malloc_lock, malloc_flags);
    return NULL; /* allocation failed */
}

/* Initializes memory allocator with enhanced validation */
void mo_heap_init(size_t *zone, uint32_t len)
{
    memblock_t *start, *end;

    if (unlikely(!zone || len < 2 * sizeof(memblock_t) + MALLOC_MIN_SIZE))
        return; /* Invalid parameters */

    len = ALIGN4(len);
    start = (memblock_t *) zone;
    end = (memblock_t *) ((size_t) zone + len - sizeof(memblock_t));

    start->next = end;
    start->size = len - 2 * sizeof(memblock_t);
    MARK_FREE(start);

    end->next = NULL;
    end->size = 0;
    MARK_USED(end); /* end block marks heap boundary */

    first_free = start;
    heap_start = (void *) zone;
    heap_end = (void *) ((size_t) end + sizeof(memblock_t));
    free_blocks_count = 1;
}

/* Allocates zero-initialized memory with overflow protection */
void *calloc(uint32_t nmemb, uint32_t size)
{
    /* Check for multiplication overflow */
    if (unlikely(nmemb && size > MALLOC_MAX_SIZE / nmemb))
        return NULL;

    uint32_t total_size = nmemb * size;
    void *buf = malloc(total_size);

    if (buf)
        memset(buf, 0, total_size);

    return buf;
}

/* Reallocates memory with improved efficiency */
void *realloc(void *ptr, uint32_t size)
{
    if (unlikely(size > MALLOC_MAX_SIZE))
        return NULL;

    if (!ptr)
        return malloc(size);

    if (!size) {
        free(ptr);
        return NULL;
    }

    memblock_t *old_block = ((memblock_t *) ptr) - 1;

    /* Validate the existing block */
    if (!validate_block(old_block) || !IS_USED(old_block))
        return NULL;

    size_t old_size = GET_SIZE(old_block);

    /* If shrinking or size is close, reuse existing block */
    if (size <= old_size &&
        old_size - size < sizeof(memblock_t) + MALLOC_MIN_SIZE)
        return ptr;

    void *new_buf = malloc(size);
    if (new_buf) {
        memcpy(new_buf, ptr, min(old_size, size));
        free(ptr);
    }

    return new_buf;
}
