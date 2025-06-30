#include <hal.h>

#include "private/error.h"

/* Ordered by enum value for quick linear search */
static const struct error_code error_desc[] = {
    {ERR_OK, "no error"},
    {ERR_FAIL, "generic failure"},

    /* scheduler / tasks */
    {ERR_NO_TASKS, "no ready tasks"},
    {ERR_KCB_ALLOC, "KCB allocation"},
    {ERR_TCB_ALLOC, "TCB allocation"},
    {ERR_STACK_ALLOC, "stack allocation"},
    {ERR_TASK_NOT_FOUND, "task not found"},
    {ERR_TASK_CANT_SUSPEND, "cannot suspend task"},
    {ERR_TASK_CANT_RESUME, "cannot resume task"},
    {ERR_TASK_INVALID_PRIO, "invalid task priority"},
    {ERR_TASK_BUSY, "resource busy"},
    {ERR_NOT_OWNER, "operation not permitted"},

    /* stack guard */
    {ERR_STACK_CHECK, "stack corruption"},

    /* IPC / sync */
    {ERR_PIPE_ALLOC, "pipe allocation"},
    {ERR_PIPE_DEALLOC, "pipe deallocation"},
    {ERR_SEM_ALLOC, "semaphore allocation"},
    {ERR_SEM_DEALLOC, "semaphore deallocation"},
    {ERR_SEM_OPERATION, "semaphore operation"},
    {ERR_MQ_NOTEMPTY, "message queue not empty"},
    {ERR_TIMEOUT, "operation timed out"},

    /* must be last */
    {ERR_UNKNOWN, "unknown error"},
};

<<<<<<< HEAD
<<<<<<< HEAD
const struct error_code *const perror = error_desc;
=======
const struct error_code * const perror = error_desc;
>>>>>>> de976a5 (Constify error_desc and perror)
=======
const struct error_code *const perror = error_desc;
>>>>>>> 888f673 (Ensure consistent indention)
