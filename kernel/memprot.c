/* Memory Protection Management
 *
 * Provides allocation and management functions for flexpages, which are
 * software abstractions representing contiguous physical memory regions with
 * hardware-enforced protection attributes.
 */

#include <lib/libc.h>
#include <lib/malloc.h>
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
