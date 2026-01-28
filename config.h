#pragma once

/* Stack Overflow Detection Configuration */
#ifndef CONFIG_STACK_PROTECTION
#define CONFIG_STACK_PROTECTION 1 /* Default: enabled for safety */
#endif

/* TLSF Memory Allocator Configuration
 *
 * FL_INDEX_MAX controls the maximum allocation size supported by the TLSF
 * allocator. The maximum allocatable size is approximately 2^FL_INDEX_MAX
 * bytes.
 *
 * Memory overhead scales with FL_INDEX_MAX:
 *   - Each FL level adds: 4 bytes (sl_bitmap) + 16 bytes (4 block pointers)
 *   - Total control structure: ~20 * (FL_INDEX_MAX - 3) bytes
 *
 * Recommended values:
 *   FL_INDEX_MAX | Max Alloc Size | Use Case
 *   -------------|----------------|-----------------------------
 *   24           |     16 MB      | Small embedded (< 16MB heap)
 *   28           |    256 MB      | Medium embedded (default)
 *   30           |      1 GB      | Large systems (< 1GB heap)
 *   31           |      2 GB      | Full 32-bit address space
 *
 * WARNING: If your heap size exceeds 2^FL_INDEX_MAX, large allocations may
 * fail unexpectedly. The allocator clamps oversized requests to the largest
 * bucket, which may contain blocks smaller than requested.
 */
#ifndef CONFIG_TLSF_FL_INDEX_MAX
#define CONFIG_TLSF_FL_INDEX_MAX 28
#endif
