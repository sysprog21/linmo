#pragma once

/* Utility Macros and Functions
 *
 * Provides essential utility macros and inline functions used throughout
 * the kernel for optimization, alignment, power-of-2 operations, and
 * compile-time analysis hints.
 */

#include <lib/libc.h>
#include <stddef.h>

/*
 * container_of - get the pointer to the parent structure from a member pointer
 *
 * @ptr:    pointer to the struct member
 * @type:   type of the parent structure
 * @member: name of the member within the parent structure
 *
 * This macro computes the address of the parent structure by subtracting
 * the member's offset within the structure.
 */
#define container_of(ptr, type, member) \
    ((type *) ((char *) (ptr) - offsetof(type, member)))

/* tcb list node helpers */
#define tcb_from_global_node(p) container_of(p, tcb_t, global_node)
#define tcb_from_mutex_node(p) container_of(p, tcb_t, mutex_node)

/* timer list node helpers */
#define timer_from_node(p) container_of(p, timer_t, t_node)
#define timer_from_running_node(p) container_of(p, timer_t, t_running_node)

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
