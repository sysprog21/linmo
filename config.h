#pragma once

/* Stack Overflow Detection Configuration */
#ifndef CONFIG_STACK_PROTECTION
#define CONFIG_STACK_PROTECTION 1 /* Default: enabled for safety */
#endif

/* Kernel event tracing configuration */
#ifndef CONFIG_DEBUG_TRACE
#define CONFIG_DEBUG_TRACE 0 /* default: disabled */
#endif

/*
 * Ring buffer slot count — power of 2 recommended for O(1) bitmask
 * index calc instead of modulo.
 */
#ifndef DEBUG_EVENT_BUFFER_SIZE
#define DEBUG_EVENT_BUFFER_SIZE 256
#endif

/*
 * Per-event slot stride in bytes — defines the fixed-size slot that
 * every event occupies in the ring buffer.  Must be a multiple of 4
 * (sizeof(uint32_t)).  Power of 2 recommended.
 */
#ifndef DEBUG_EVENT_STRUCT_SIZE
#define DEBUG_EVENT_STRUCT_SIZE 16
#endif
