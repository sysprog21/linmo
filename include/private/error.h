#pragma once

/* Centralizes all error codes used throughout the kernel. Error codes use
 * automatic enumeration starting from -16383 to avoid conflicts with POSIX
 * errno values. Each subsystem has its own logical grouping for easier
 * debugging and maintenance.
 */

enum {
    /* Success and Generic Errors */
    ERR_OK = 0,    /* Operation completed successfully */
    ERR_FAIL = -1, /* Generic failure */

    /* Task Management and Scheduler Errors (auto-numbered from -16383) */
    ERR_NO_TASKS = -16383,  /* No tasks available for scheduling */
    ERR_KCB_ALLOC,          /* Kernel Control Block allocation failed */
    ERR_TCB_ALLOC,          /* Task Control Block allocation failed */
    ERR_STACK_ALLOC,        /* Task stack allocation failed */
    ERR_TASK_CANT_REMOVE,   /* Task cannot be removed (e.g., self-remove) */
    ERR_TASK_NOT_FOUND,     /* Task ID not found in system */
    ERR_TASK_CANT_SUSPEND,  /* Task cannot be suspended */
    ERR_TASK_CANT_RESUME,   /* Task cannot be resumed */
    ERR_TASK_INVALID_PRIO,  /* Invalid task priority specified */
    ERR_TASK_INVALID_ENTRY, /* Invalid task entry point */
    ERR_TASK_BUSY,          /* Task is busy or in wrong state */
    ERR_NOT_OWNER,          /* Operation requires ownership */

    /* Memory Protection Errors */
    ERR_STACK_CHECK,  /* Stack overflow or corruption detected */
    ERR_HEAP_CORRUPT, /* Heap corruption or invalid free detected */

    /* PMP Configuration Errors */
    ERR_PMP_INVALID_REGION, /* Invalid PMP region parameters */
    ERR_PMP_NO_REGIONS,     /* No free PMP regions available */
    ERR_PMP_LOCKED,         /* Region is locked by higher priority */
    ERR_PMP_SIZE_MISMATCH,  /* Size doesn't meet alignment requirements */
    ERR_PMP_ADDR_RANGE,     /* Address range is invalid */
    ERR_PMP_NOT_INIT,       /* PMP not initialized */

    /* IPC and Synchronization Errors */
    ERR_PIPE_ALLOC,    /* Pipe allocation failed */
    ERR_PIPE_DEALLOC,  /* Pipe deallocation failed */
    ERR_SEM_ALLOC,     /* Semaphore allocation failed */
    ERR_SEM_DEALLOC,   /* Semaphore deallocation failed */
    ERR_SEM_OPERATION, /* Semaphore operation failed */
    ERR_MQ_NOTEMPTY,   /* Message queue is not empty */
    ERR_TIMEOUT,       /* Operation timed out */

    /* Sentinel - must remain the last entry */
    ERR_UNKNOWN /* Unknown or unclassified error */
};

/* Error Code Description Structure
 *
 * Maps error codes to human-readable descriptions for debugging
 * and error reporting purposes.
 */
struct error_code {
    int32_t code;     /* The error code value */
    char *const desc; /* Human-readable description */
};

/* Global error code lookup table */
extern const struct error_code *const perror;
