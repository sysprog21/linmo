#pragma once

/* Stack Overflow Detection Configuration */
#ifndef CONFIG_STACK_PROTECTION
#define CONFIG_STACK_PROTECTION 1 /* Default: enabled for safety */
#endif

/* Kernel event tracing configuration */
#ifndef CONFIG_DEBUG_TRACE
#define CONFIG_DEBUG_TRACE 1
#endif

#ifndef DEBUG_EVENT_BUFFER_SIZE
#define DEBUG_EVENT_BUFFER_SIZE 256
#endif
