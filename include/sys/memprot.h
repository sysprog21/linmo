/* Memory Protection Abstractions
 *
 * Software abstractions for managing memory protection at different
 * granularities. These structures build upon hardware protection
 * mechanisms (such as RISC-V PMP) to provide flexible, architecture-
 * independent memory isolation.
 */

#pragma once

#include <types.h>

/* Forward declarations */
struct fpage;
struct as;

/* Flexpage
 *
 * Contiguous physical memory region with hardware-enforced protection.
 * Supports arbitrary base addresses and sizes without alignment constraints.
 */
typedef struct fpage {
    struct fpage *as_next;  /* Next in address space list */
    struct fpage *map_next; /* Next in mapping chain */
    struct fpage *pmp_next; /* Next in PMP queue */
    uint32_t base;          /* Physical base address */
    uint32_t size;          /* Region size */
    uint32_t rwx;           /* R/W/X permission bits */
    uint32_t pmp_id;        /* PMP region index */
    uint32_t flags;         /* Status flags */
    uint32_t priority;      /* Eviction priority */
    int used;               /* Usage counter */
} fpage_t;

/* Memory Space
 *
 * Collection of flexpages forming a task's memory view. Can be shared
 * across multiple tasks.
 */
typedef struct memspace {
    uint32_t as_id;          /* Memory space identifier */
    struct fpage *first;     /* Head of flexpage list */
    struct fpage *pmp_first; /* Head of PMP-loaded list */
    struct fpage *pmp_stack; /* Stack regions */
    uint32_t shared;         /* Shared flag */
} memspace_t;

/* Memory Pool
 *
 * Static memory region descriptor for boot-time PMP initialization.
 */
typedef struct {
    const char *name; /* Pool name */
    uintptr_t start;  /* Start address */
    uintptr_t end;    /* End address */
    uint32_t flags;   /* Access permissions */
    uint32_t tag;     /* Pool type/priority */
} mempool_t;

/* Memory Pool Declaration Helpers
 *
 * Simplifies memory pool initialization with designated initializers.
 * DECLARE_MEMPOOL_FROM_SYMBOLS uses token concatenation to construct
 * linker symbol names automatically.
 */
#define DECLARE_MEMPOOL(name_, start_, end_, flags_, tag_)           \
    {                                                                \
        .name = (name_), .start = (uintptr_t) (start_),              \
        .end = (uintptr_t) (end_), .flags = (flags_), .tag = (tag_), \
    }

#define DECLARE_MEMPOOL_FROM_SYMBOLS(name_, sym_base_, flags_, tag_)   \
    DECLARE_MEMPOOL((name_), &(sym_base_##_start), &(sym_base_##_end), \
                    (flags_), (tag_))

/* Flexpage Management Functions */

/* Creates and initializes a new flexpage.
 * @base : Physical base address
 * @size : Size in bytes
 * @rwx : Permission bits
 * @priority : Eviction priority
 * Returns pointer to created flexpage, or NULL on failure.
 */
fpage_t *mo_fpage_create(uint32_t base,
                         uint32_t size,
                         uint32_t rwx,
                         uint32_t priority);

/* Destroys a flexpage.
 * @fpage : Pointer to flexpage to destroy
 */
void mo_fpage_destroy(fpage_t *fpage);

/* Memory Space Management Functions */

/* Creates and initializes a memory space.
 * @as_id : Memory space identifier
 * @shared : Whether this space can be shared across tasks
 * Returns pointer to created memory space, or NULL on failure.
 */
memspace_t *mo_memspace_create(uint32_t as_id, uint32_t shared);

/* Destroys a memory space and all its flexpages.
 * @mspace : Pointer to memory space to destroy
 */
void mo_memspace_destroy(memspace_t *mspace);

/* Flexpage Hardware Loading Functions */

/* Loads a flexpage into a hardware region.
 * @fpage : Pointer to flexpage to load
 * @region_idx : Hardware region index (0-15)
 * Returns 0 on success, or negative error code on failure.
 */
int32_t mo_load_fpage(fpage_t *fpage, uint8_t region_idx);

/* Evicts a flexpage from its hardware region.
 * @fpage : Pointer to flexpage to evict
 * Returns 0 on success, or negative error code on failure.
 */
int32_t mo_evict_fpage(fpage_t *fpage);
