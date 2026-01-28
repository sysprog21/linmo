#pragma once

/* Utility Macros and Functions
 *
 * Provides essential utility macros and inline functions used throughout
 * the kernel for optimization, alignment, power-of-2 operations, and
 * compile-time analysis hints.
 */

#include <lib/libc.h>

/* Compiler Optimization Hints
 *
 * Branch prediction hints help the compiler and processor optimize for
 * the most common code paths.
 */
#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)

/* Compiler Attributes */
#define UNUSED __attribute__((unused)) /* Suppress unused warnings */

/* Array and Memory Utilities */

/* Calculate number of elements in a statically allocated array */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

/* Align pointer forward to the next 4-byte boundary
 * Essential for maintaining alignment requirements on RISC-V
 */
#define ALIGN4(x) ((((uint32_t) (x) + 3u) >> 2) << 2)

/* Fast range check using bit operations.
 * Checks if x is in range [minx, minx + size - 1].
 * Faster than (x >= minx && x < minx + size) due to fewer branches.
 * For any variable range checking:
 *     if (x >= minx && x <= maxx) ...
 * it is faster to use bit operation:
 *     if ((signed)((x - minx) | (maxx - x)) >= 0) ...
 */
#define RANGE_CHECK(x, minx, size) \
    ((int32_t) (((x) - (minx)) | ((minx) + (size) - 1 - (x))) >= 0)

/* Power-of-2 Utility Functions
 *
 * Efficient bit manipulation functions for power-of-2 operations,
 * commonly used for buffer sizing and memory allocation.
 */

/* Check if a value is a power of 2
 * Uses bit trick: power of 2 has only one bit set, so (x & (x-1)) == 0
 */
static inline bool ispowerof2(uint32_t x)
{
    return x && !(x & (x - 1));
}

/* Round up to the next power of 2
 * Uses bit manipulation to efficiently find the next power of 2
 * greater than or equal to the input value
 */
static inline uint32_t nextpowerof2(uint32_t x)
{
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x++;

    return x;
}
