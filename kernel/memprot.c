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

/* Loads a flexpage into a hardware region */
int32_t mo_load_fpage(fpage_t *fpage, uint8_t region_idx)
{
    return pmp_load_fpage(fpage, region_idx);
}

/* Evicts a flexpage from its hardware region */
int32_t mo_evict_fpage(fpage_t *fpage)
{
    return pmp_evict_fpage(fpage);
}

/* Creates and initializes a memory space */
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

/* Destroys a memory space and all its flexpages */
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
