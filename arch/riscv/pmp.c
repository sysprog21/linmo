/* RISC-V Physical Memory Protection (PMP) Implementation
 *
 * Provides hardware-enforced memory isolation using PMP in TOR mode.
 */

#include <hal.h>

#include "csr.h"
#include "pmp.h"
#include "private/error.h"

/* Static Memory Pools for Boot-time PMP Initialization
 *
 * Defines kernel memory regions protected at boot. Each pool specifies
 * a memory range and access permissions.
 */
static const mempool_t kernel_mempools[] = {
    DECLARE_MEMPOOL("kernel_text",
                    &_stext,
                    &_etext,
                    PMPCFG_PERM_RX,
                    PMP_PRIORITY_KERNEL),
    DECLARE_MEMPOOL("kernel_data",
                    &_sdata,
                    &_edata,
                    PMPCFG_PERM_RW,
                    PMP_PRIORITY_KERNEL),
    DECLARE_MEMPOOL("kernel_bss",
                    &_sbss,
                    &_ebss,
                    PMPCFG_PERM_RW,
                    PMP_PRIORITY_KERNEL),
    DECLARE_MEMPOOL("kernel_heap",
                    &_heap_start,
                    &_heap_end,
                    PMPCFG_PERM_RW,
                    PMP_PRIORITY_KERNEL),
    DECLARE_MEMPOOL("kernel_stack",
                    &_stack_bottom,
                    &_stack_top,
                    PMPCFG_PERM_RW,
                    PMP_PRIORITY_KERNEL),
};

#define KERNEL_MEMPOOL_COUNT \
    (sizeof(kernel_mempools) / sizeof(kernel_mempools[0]))

int32_t pmp_init_pools(pmp_config_t *config,
                       const mempool_t *pools,
                       size_t count)
{
    if (!config || !pools || count == 0)
        return ERR_PMP_INVALID_REGION;

    /* Initialize PMP hardware and state */
    int32_t ret = pmp_init(config);
    if (ret < 0)
        return ret;

    /* Configure each memory pool as a PMP region */
    for (size_t i = 0; i < count; i++) {
        const mempool_t *pool = &pools[i];

        /* Validate pool boundaries */
        if (pool->start >= pool->end)
            return ERR_PMP_ADDR_RANGE;

        /* Prepare PMP region configuration */
        pmp_region_t region = {
            .addr_start = pool->start,
            .addr_end = pool->end,
            .permissions = pool->flags & (PMPCFG_R | PMPCFG_W | PMPCFG_X),
            .priority = pool->tag,
            .region_id = i,
            .locked = 0,
        };

        /* Configure the PMP region */
        ret = pmp_set_region(config, &region);
        if (ret < 0)
            return ret;
    }

    return ERR_OK;
}

int32_t pmp_init_kernel(pmp_config_t *config)
{
    return pmp_init_pools(config, kernel_mempools, KERNEL_MEMPOOL_COUNT);
}
