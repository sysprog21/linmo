/* RISC-V Physical Memory Protection (PMP) Hardware Layer
 *
 * Low-level interface to RISC-V PMP using TOR (Top-of-Range) mode for
 * flexible region management without alignment constraints.
 */

#pragma once

#include <hal.h>
#include <sys/memprot.h>
#include <types.h>

/* PMP Region Priority Levels (lower value = higher priority)
 *
 * Used for eviction decisions when hardware PMP regions are exhausted.
 */
typedef enum {
    PMP_PRIORITY_KERNEL = 0,
    PMP_PRIORITY_STACK = 1,
    PMP_PRIORITY_SHARED = 2,
    PMP_PRIORITY_TEMPORARY = 3,
    PMP_PRIORITY_COUNT = 4
} pmp_priority_t;

/* PMP Region Configuration */
typedef struct {
    uint32_t addr_start;     /* Start address (inclusive) */
    uint32_t addr_end;       /* End address (exclusive, written to pmpaddr) */
    uint8_t permissions;     /* R/W/X bits (PMPCFG_R | PMPCFG_W | PMPCFG_X) */
    pmp_priority_t priority; /* Eviction priority */
    uint8_t region_id;       /* Hardware region index (0-15) */
    uint8_t locked;          /* Lock bit (cannot modify until reset) */
} pmp_region_t;

/* PMP Global State */
typedef struct {
    pmp_region_t regions[PMP_MAX_REGIONS]; /* Shadow of hardware config */
    uint8_t region_count;                  /* Active region count */
    uint8_t next_region_idx;               /* Next free region index */
    uint32_t initialized;                  /* Initialization flag */
} pmp_config_t;

/* PMP Management Functions */

/* Returns pointer to global PMP configuration */
pmp_config_t *pmp_get_config(void);

/* Initializes the PMP hardware and configuration state.
 * @config : Pointer to pmp_config_t structure to be initialized.
 * Returns 0 on success, or negative error code on failure.
 */
int32_t pmp_init(pmp_config_t *config);

/* Configures a single PMP region in TOR mode.
 * @config : Pointer to PMP configuration state
 * @region : Pointer to pmp_region_t structure with desired configuration
 * Returns 0 on success, or negative error code on failure.
 */
int32_t pmp_set_region(pmp_config_t *config, const pmp_region_t *region);

/* Reads the current configuration of a PMP region.
 * @config : Pointer to PMP configuration state
 * @region_idx : Index of the region to read (0-15)
 * @region : Pointer to pmp_region_t to store the result
 * Returns 0 on success, or negative error code on failure.
 */
int32_t pmp_get_region(const pmp_config_t *config,
                       uint8_t region_idx,
                       pmp_region_t *region);

/* Disables a PMP region.
 * @config : Pointer to PMP configuration state
 * @region_idx : Index of the region to disable (0-15)
 * Returns 0 on success, or negative error code on failure.
 */
int32_t pmp_disable_region(pmp_config_t *config, uint8_t region_idx);

/* Locks a PMP region to prevent further modification.
 * @config : Pointer to PMP configuration state
 * @region_idx : Index of the region to lock (0-15)
 * Returns 0 on success, or negative error code on failure.
 */
int32_t pmp_lock_region(pmp_config_t *config, uint8_t region_idx);

/* Verifies that a memory access is allowed by the current PMP configuration.
 * @config : Pointer to PMP configuration state
 * @addr : Address to check
 * @size : Size of the access in bytes
 * @is_write : 1 for write access, 0 for read access
 * @is_execute : 1 for execute access, 0 for data access
 * Returns 1 if access is allowed, 0 if denied, or negative error code.
 */
int32_t pmp_check_access(const pmp_config_t *config,
                         uint32_t addr,
                         uint32_t size,
                         uint8_t is_write,
                         uint8_t is_execute);

/* Memory Pool Management Functions */

/* Initializes PMP regions from an array of memory pool descriptors.
 * @config : Pointer to PMP configuration state
 * @pools : Array of memory pool descriptors
 * @count : Number of pools in the array
 * Returns 0 on success, or negative error code on failure.
 */
int32_t pmp_init_pools(pmp_config_t *config,
                       const mempool_t *pools,
                       size_t count);

/* Initializes PMP with default kernel memory pools.
 * @config : Pointer to PMP configuration state
 * Returns 0 on success, or negative error code on failure.
 */
int32_t pmp_init_kernel(pmp_config_t *config);

/* Flexpage Hardware Loading Functions */

/* Loads a flexpage into a PMP hardware region.
 * @fpage : Pointer to flexpage to load
 * @region_idx : Hardware PMP region index (0-15)
 * Returns 0 on success, or negative error code on failure.
 */
int32_t pmp_load_fpage(fpage_t *fpage, uint8_t region_idx);

/* Evicts a flexpage from its PMP hardware region.
 * @fpage : Pointer to flexpage to evict
 * Returns 0 on success, or negative error code on failure.
 */
int32_t pmp_evict_fpage(fpage_t *fpage);
