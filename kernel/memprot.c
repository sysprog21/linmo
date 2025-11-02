/* Memory Protection Management
 *
 * Provides allocation and management functions for flexpages, which are
 * software abstractions representing contiguous physical memory regions with
 * hardware-enforced protection attributes.
 */

#include <lib/libc.h>
#include <lib/malloc.h>
#include <pmp.h>
#include <sys/memprot.h>

/* Creates and initializes a flexpage */
fpage_t *mo_fpage_create(uint32_t base,
                         uint32_t size,
                         uint32_t rwx,
                         uint32_t priority)
{
    fpage_t *fpage = malloc(sizeof(fpage_t));
    if (!fpage)
        return NULL;

    /* Initialize all fields */
    fpage->as_next = NULL;
    fpage->map_next = NULL;
    fpage->pmp_next = NULL;
    fpage->base = base;
    fpage->size = size;
    fpage->rwx = rwx;
    fpage->pmp_id = 0; /* Not loaded into PMP initially */
    fpage->flags = 0;  /* No flags set initially */
    fpage->priority = priority;
    fpage->used = 0; /* Not in use initially */

    return fpage;
}

/* Destroys a flexpage */
void mo_fpage_destroy(fpage_t *fpage)
{
    if (!fpage)
        return;

    free(fpage);
}

/* Selects victim flexpage for eviction using priority-based algorithm.
 *
 * @mspace : Pointer to memory space
 * Returns pointer to victim flexpage, or NULL if no evictable page found.
 */
fpage_t *select_victim_fpage(memspace_t *mspace)
{
    if (!mspace)
        return NULL;

    fpage_t *victim = NULL;
    uint32_t lowest_prio = 0;

    /* Select page with highest priority value (lowest priority).
     * Kernel regions (priority 0) are never selected. */
    for (fpage_t *fp = mspace->pmp_first; fp; fp = fp->pmp_next) {
        if (fp->priority > lowest_prio) {
            victim = fp;
            lowest_prio = fp->priority;
        }
    }

    return victim;
}

/* Loads a flexpage into a PMP hardware region.
 *
 * @fpage : Pointer to flexpage to load
 * @region_idx : Hardware PMP region index (0-15)
 * Returns 0 on success, or negative error code on failure.
 */
int32_t pmp_load_fpage(fpage_t *fpage, uint8_t region_idx)
{
    if (!fpage)
        return -1;

    pmp_config_t *config = pmp_get_config();
    if (!config)
        return -1;

    /* Configure PMP region from flexpage attributes */
    pmp_region_t region = {
        .addr_start = fpage->base,
        .addr_end = fpage->base + fpage->size,
        .permissions = fpage->rwx,
        .priority = fpage->priority,
        .region_id = region_idx,
        .locked = 0,
    };

    int32_t ret = pmp_set_region(config, &region);
    if (ret == 0) {
        fpage->pmp_id = region_idx;
    }

    return ret;
}

/* Evicts a flexpage from its PMP hardware region.
 *
 * @fpage : Pointer to flexpage to evict
 * Returns 0 on success, or negative error code on failure.
 */
int32_t pmp_evict_fpage(fpage_t *fpage)
{
    if (!fpage)
        return -1;

    /* Only evict if actually loaded into PMP */
    if (fpage->pmp_id == 0)
        return 0;

    pmp_config_t *config = pmp_get_config();
    if (!config)
        return -1;

    int32_t ret = pmp_disable_region(config, fpage->pmp_id);
    if (ret == 0) {
        fpage->pmp_id = 0;
    }

    return ret;
}

/* Creates and initializes a memory space.
 *
 * @as_id : Memory space identifier
 * @shared : Whether this space can be shared across tasks
 * Returns pointer to created memory space, or NULL on failure.
 */
memspace_t *mo_memspace_create(uint32_t as_id, uint32_t shared)
{
    memspace_t *mspace = malloc(sizeof(memspace_t));
    if (!mspace)
        return NULL;

    mspace->as_id = as_id;
    mspace->first = NULL;
    mspace->pmp_first = NULL;
    mspace->pmp_stack = NULL;
    mspace->shared = shared;

    return mspace;
}

/* Destroys a memory space and all its flexpages.
 *
 * @mspace : Pointer to memory space to destroy
 */
void mo_memspace_destroy(memspace_t *mspace)
{
    if (!mspace)
        return;

    /* Free all flexpages in the list */
    fpage_t *fp = mspace->first;
    while (fp) {
        fpage_t *next = fp->as_next;
        mo_fpage_destroy(fp);
        fp = next;
    }

    free(mspace);
}
