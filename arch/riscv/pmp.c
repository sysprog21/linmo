/* RISC-V Physical Memory Protection (PMP) Implementation
 *
 * Provides hardware-enforced memory isolation using PMP in TOR mode.
 */

#include <hal.h>

#include "csr.h"
#include "pmp.h"
#include "private/error.h"

/* PMP CSR Access Helpers
 *
 * RISC-V CSR instructions require compile-time constant addresses encoded in
 * the instruction itself. These helpers use switch-case dispatch to provide
 * runtime indexed access to PMP configuration and address registers.
 *
 * - pmpcfg0-3: Four 32-bit configuration registers (16 regions, 8 bits each)
 * - pmpaddr0-15: Sixteen address registers for TOR (Top-of-Range) mode
 */

/* Read PMP configuration register by index (0-3) */
static uint32_t __attribute__((unused)) read_pmpcfg(uint8_t idx)
{
    switch (idx) {
    case 0:
        return read_csr_num(CSR_PMPCFG0);
    case 1:
        return read_csr_num(CSR_PMPCFG1);
    case 2:
        return read_csr_num(CSR_PMPCFG2);
    case 3:
        return read_csr_num(CSR_PMPCFG3);
    default:
        return 0;
    }
}

/* Write PMP configuration register by index (0-3) */
static void __attribute__((unused)) write_pmpcfg(uint8_t idx, uint32_t val)
{
    switch (idx) {
    case 0:
        write_csr_num(CSR_PMPCFG0, val);
        break;
    case 1:
        write_csr_num(CSR_PMPCFG1, val);
        break;
    case 2:
        write_csr_num(CSR_PMPCFG2, val);
        break;
    case 3:
        write_csr_num(CSR_PMPCFG3, val);
        break;
    }
}

/* Read PMP address register by index (0-15) */
static uint32_t __attribute__((unused)) read_pmpaddr(uint8_t idx)
{
    switch (idx) {
    case 0:
        return read_csr_num(CSR_PMPADDR0);
    case 1:
        return read_csr_num(CSR_PMPADDR1);
    case 2:
        return read_csr_num(CSR_PMPADDR2);
    case 3:
        return read_csr_num(CSR_PMPADDR3);
    case 4:
        return read_csr_num(CSR_PMPADDR4);
    case 5:
        return read_csr_num(CSR_PMPADDR5);
    case 6:
        return read_csr_num(CSR_PMPADDR6);
    case 7:
        return read_csr_num(CSR_PMPADDR7);
    case 8:
        return read_csr_num(CSR_PMPADDR8);
    case 9:
        return read_csr_num(CSR_PMPADDR9);
    case 10:
        return read_csr_num(CSR_PMPADDR10);
    case 11:
        return read_csr_num(CSR_PMPADDR11);
    case 12:
        return read_csr_num(CSR_PMPADDR12);
    case 13:
        return read_csr_num(CSR_PMPADDR13);
    case 14:
        return read_csr_num(CSR_PMPADDR14);
    case 15:
        return read_csr_num(CSR_PMPADDR15);
    default:
        return 0;
    }
}

/* Write PMP address register by index (0-15) */
static void __attribute__((unused)) write_pmpaddr(uint8_t idx, uint32_t val)
{
    switch (idx) {
    case 0:
        write_csr_num(CSR_PMPADDR0, val);
        break;
    case 1:
        write_csr_num(CSR_PMPADDR1, val);
        break;
    case 2:
        write_csr_num(CSR_PMPADDR2, val);
        break;
    case 3:
        write_csr_num(CSR_PMPADDR3, val);
        break;
    case 4:
        write_csr_num(CSR_PMPADDR4, val);
        break;
    case 5:
        write_csr_num(CSR_PMPADDR5, val);
        break;
    case 6:
        write_csr_num(CSR_PMPADDR6, val);
        break;
    case 7:
        write_csr_num(CSR_PMPADDR7, val);
        break;
    case 8:
        write_csr_num(CSR_PMPADDR8, val);
        break;
    case 9:
        write_csr_num(CSR_PMPADDR9, val);
        break;
    case 10:
        write_csr_num(CSR_PMPADDR10, val);
        break;
    case 11:
        write_csr_num(CSR_PMPADDR11, val);
        break;
    case 12:
        write_csr_num(CSR_PMPADDR12, val);
        break;
    case 13:
        write_csr_num(CSR_PMPADDR13, val);
        break;
    case 14:
        write_csr_num(CSR_PMPADDR14, val);
        break;
    case 15:
        write_csr_num(CSR_PMPADDR15, val);
        break;
    }
}

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
