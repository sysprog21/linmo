#pragma once

enum {
    ERR_OK = 0,
    ERR_FAIL = -1,

    /* Scheduler / task management */
    ERR_NO_TASKS = -16383,
    ERR_KCB_ALLOC,
    ERR_TCB_ALLOC,
    ERR_STACK_ALLOC,
    ERR_TASK_CANT_REMOVE,
    ERR_TASK_NOT_FOUND,
    ERR_TASK_CANT_SUSPEND,
    ERR_TASK_CANT_RESUME,
    ERR_TASK_INVALID_PRIO,
    ERR_TASK_INVALID_ENTRY,
    ERR_TASK_BUSY,
    ERR_NOT_OWNER,

    /* Stack guard */
    ERR_STACK_CHECK,

    /* IPC / synchronization */
    ERR_PIPE_ALLOC,
    ERR_PIPE_DEALLOC,
    ERR_SEM_ALLOC,
    ERR_SEM_DEALLOC,
    ERR_SEM_OPERATION,
    ERR_MQ_NOTEMPTY,
    ERR_TIMEOUT,

    /* must remain the last entry */
    ERR_UNKNOWN
};

struct error_code {
    int32_t code;
    char *const desc;
};

extern const struct error_code *const perror;
