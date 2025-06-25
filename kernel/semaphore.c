/* Counting Semaphores for task synchronization.
 *
 * This implementation provides a thread-safe counting semaphore with enhanced
 * error checking and race condition prevention. Tasks can wait (pend) on a
 * semaphore, blocking until a resource is available, and signal (post) to
 * release a resource, potentially waking up a waiting task.
 * The wait queue is served in strict First-In, First-Out (FIFO) order.
 */

#include <hal.h>
#include <spinlock.h>
#include <sys/semaphore.h>
#include <sys/task.h>

#include "private/error.h"

/* Semaphore Control Block structure. */
struct sem_t {
    queue_t *wait_q;        /**< Queue of tasks blocked on this semaphore. */
    volatile int32_t count; /**< Number of available resources (tokens). */
    uint16_t max_waiters;   /**< Maximum capacity of wait queue. */
    uint32_t magic;         /**< Magic number for validation. */
};

/* Magic number for semaphore validation */
#define SEM_MAGIC 0x53454D00 /* "SEM\0" */

static spinlock_t semaphore_lock = SPINLOCK_INITIALIZER;
static uint32_t semaphore_flags = 0;

static inline bool sem_is_valid(const sem_t *s)
{
    return (s && s->magic == SEM_MAGIC && s->wait_q);
}

sem_t *mo_sem_create(uint16_t max_waiters, int32_t initial_count)
{
    /* Enhanced input validation */
    if (!max_waiters || initial_count < 0 || initial_count > SEM_MAX_COUNT)
        return NULL;

    sem_t *sem = malloc(sizeof(sem_t));
    if (!sem)
        return NULL;

    /* Initialize structure to known state */
    sem->wait_q = NULL;
    sem->count = 0;
    sem->max_waiters = 0;
    sem->magic = 0;

    /* Create wait queue */
    sem->wait_q = queue_create(max_waiters);
    if (!sem->wait_q) {
        free(sem);
        return NULL;
    }

    /* Initialize remaining fields atomically */
    sem->count = initial_count;
    sem->max_waiters = max_waiters;
    sem->magic = SEM_MAGIC; /* Mark as valid last */

    return sem;
}

int32_t mo_sem_destroy(sem_t *s)
{
    if (!s)
        return ERR_OK; /* Destroying NULL is a no-op, not an error */

    if (!sem_is_valid(s))
        return ERR_FAIL;

    spin_lock_irqsave(&semaphore_lock, &semaphore_flags);

    /* Check if any tasks are waiting - unsafe to destroy if so */
    if (queue_count(s->wait_q) > 0) {
        spin_unlock_irqrestore(&semaphore_lock, semaphore_flags);
        return ERR_TASK_BUSY;
    }

    /* Invalidate the semaphore to prevent further use */
    s->magic = 0;
    queue_t *wait_q = s->wait_q;
    s->wait_q = NULL;

    spin_unlock_irqrestore(&semaphore_lock, semaphore_flags);

    /* Clean up resources outside critical section */
    queue_destroy(wait_q);
    free(s);
    return ERR_OK;
}

void mo_sem_wait(sem_t *s)
{
    if (!sem_is_valid(s)) {
        /* Invalid semaphore - this is a programming error */
        panic(ERR_SEM_OPERATION);
    }

    spin_lock_irqsave(&semaphore_lock, &semaphore_flags);

    /* Fast path: resource available and no waiters (preserves FIFO) */
    if (s->count > 0 && queue_count(s->wait_q) == 0) {
        s->count--;
        spin_unlock_irqrestore(&semaphore_lock, semaphore_flags);
        return;
    }

    /* Slow path: must wait for resource */
    /* Verify wait queue has capacity (should never fail for valid semaphore) */
    if (queue_count(s->wait_q) >= s->max_waiters) {
        spin_unlock_irqrestore(&semaphore_lock, semaphore_flags);
        panic(ERR_SEM_OPERATION); /* Queue overflow - system error */
    }

    /* Block current task atomically. _sched_block will:
     * 1. Add current task to wait queue
     * 2. Set task state to BLOCKED
     * 3. Call scheduler without releasing NOSCHED lock
     * The lock is released when we context switch to another task.
     */
    _sched_block(s->wait_q);

    /* When we return here, we have been awakened and have acquired the
     * semaphore. The task that signaled us did NOT increment the count - the
     * "token" was passed directly to us, so no further action is needed.
     */
}

int32_t mo_sem_trywait(sem_t *s)
{
    if (!sem_is_valid(s))
        return ERR_FAIL;

    int32_t result = ERR_FAIL;

    spin_lock_irqsave(&semaphore_lock, &semaphore_flags);

    /* Only succeed if resource is available AND no waiters (preserves FIFO) */
    if (s->count > 0 && queue_count(s->wait_q) == 0) {
        s->count--;
        result = ERR_OK;
    }

    spin_unlock_irqrestore(&semaphore_lock, semaphore_flags);
    return result;
}

void mo_sem_signal(sem_t *s)
{
    if (!sem_is_valid(s)) {
        /* Invalid semaphore - this is a programming error */
        panic(ERR_SEM_OPERATION);
    }

    bool should_yield = false;
    tcb_t *awakened_task = NULL;

    spin_lock_irqsave(&semaphore_lock, &semaphore_flags);

    /* Check if any tasks are waiting */
    if (queue_count(s->wait_q) > 0) {
        /* Wake up the oldest waiting task (FIFO order) */
        awakened_task = queue_dequeue(s->wait_q);
        if (awakened_task) {
            /* Validate the awakened task before changing its state */
            if (awakened_task->state == TASK_BLOCKED) {
                awakened_task->state = TASK_READY;
                should_yield = true;
            } else {
                /* Task state inconsistency - this should not happen */
                panic(ERR_SEM_OPERATION);
            }
        }
        /* Note: count is NOT incremented - the "token" is passed directly to
         * the awakened task to prevent race conditions.
         */
    } else {
        /* No waiting tasks - increment available count */
        if (s->count < SEM_MAX_COUNT)
            s->count++;
        /* Silently ignore overflow - semaphore remains at max count */
    }

    spin_unlock_irqrestore(&semaphore_lock, semaphore_flags);

    /* Yield outside critical section to allow awakened task to run.
     * This improves responsiveness if the awakened task has higher priority.
     */
    if (should_yield)
        mo_task_yield();
}

int32_t mo_sem_getvalue(sem_t *s)
{
    if (!sem_is_valid(s))
        return -1;

    /* This is inherently racy - the value may change immediately after being
     * read. The volatile keyword ensures we read the current value, but does
     * not provide atomicity across multiple operations.
     */
    return s->count;
}

int32_t mo_sem_waiting_count(sem_t *s)
{
    if (!sem_is_valid(s))
        return -1;

    int32_t count;

    spin_lock_irqsave(&semaphore_lock, &semaphore_flags);
    count = queue_count(s->wait_q);
    spin_unlock_irqrestore(&semaphore_lock, semaphore_flags);

    return count;
}
