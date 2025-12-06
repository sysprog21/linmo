/* Counting Semaphores for task synchronization.
 *
 * This implementation provides a thread-safe counting semaphore with enhanced
 * error checking and race condition prevention. Tasks can wait (pend) on a
 * semaphore, blocking until a resource is available, and signal (post) to
 * release a resource, potentially waking up a waiting task.
 * The wait queue is served in strict First-In, First-Out (FIFO) order.
 */

#include <hal.h>
#include <sys/semaphore.h>
#include <sys/task.h>

#include "private/error.h"
#include "private/utils.h"

/* Semaphore Control Block structure. */
struct sem_t {
    queue_t *wait_q;        /**< Queue of tasks blocked on this semaphore. */
    volatile int32_t count; /**< Number of available resources (tokens). */
    uint16_t max_waiters;   /**< Maximum capacity of wait queue. */
    uint32_t magic;         /**< Magic number for validation. */
};

/* Magic number for semaphore validation */
#define SEM_MAGIC 0x53454D00 /* "SEM\0" */

static inline bool sem_is_valid(const sem_t *s)
{
    return s && s->magic == SEM_MAGIC && s->wait_q && s->max_waiters > 0 &&
           s->count >= 0 && s->count <= SEM_MAX_COUNT;
}

static inline void sem_invalidate(sem_t *s)
{
    if (s) {
        s->magic = 0xDEADBEEF; /* Clear magic to prevent reuse */
        s->count = -1;
        s->max_waiters = 0;
    }
}

sem_t *mo_sem_create(uint16_t max_waiters, int32_t initial_count)
{
    /* Enhanced input validation */
    if (unlikely(!max_waiters || initial_count < 0 ||
                 initial_count > SEM_MAX_COUNT))
        return NULL;

    sem_t *sem = malloc(sizeof(sem_t));
    if (unlikely(!sem))
        return NULL;

    /* Initialize structure to known safe state */
    sem->wait_q = NULL;
    sem->count = 0;
    sem->max_waiters = 0;
    sem->magic = 0;

    /* Create wait queue */
    sem->wait_q = queue_create(max_waiters);
    if (unlikely(!sem->wait_q)) {
        free(sem);
        return NULL;
    }

    /* Initialize remaining fields atomically */
    sem->count = initial_count;
    sem->max_waiters = max_waiters;
    sem->magic = SEM_MAGIC; /* Mark as valid last to prevent races */

    return sem;
}

int32_t mo_sem_destroy(sem_t *s)
{
    if (!s)
        return ERR_OK; /* Destroying NULL is a no-op, not an error */

    if (unlikely(!sem_is_valid(s)))
        return ERR_FAIL;

    NOSCHED_ENTER();

    /* Check if any tasks are waiting - unsafe to destroy if so */
    if (unlikely(queue_count(s->wait_q) > 0)) {
        NOSCHED_LEAVE();
        return ERR_TASK_BUSY;
    }

    /* Atomically invalidate the semaphore to prevent further use */
    sem_invalidate(s);
    queue_t *wait_q = s->wait_q;
    s->wait_q = NULL;

    NOSCHED_LEAVE();

    /* Clean up resources outside critical section */
    queue_destroy(wait_q);
    free(s);
    return ERR_OK;
}

void mo_sem_wait(sem_t *s)
{
    if (unlikely(!sem_is_valid(s))) {
        /* Invalid semaphore - this is a programming error */
        panic(ERR_SEM_OPERATION);
    }

    NOSCHED_ENTER();

    /* Fast path: resource available and no waiters (preserves FIFO ordering) */
    if (likely(s->count > 0 && queue_count(s->wait_q) == 0)) {
        s->count--;
        NOSCHED_LEAVE();
        return;
    }

    /* Slow path: must wait for resource */
    /* Verify wait queue has capacity before attempting to block */
    if (unlikely(queue_count(s->wait_q) >= s->max_waiters)) {
        NOSCHED_LEAVE();
        panic(ERR_SEM_OPERATION); /* Queue overflow - system error */
    }

    /* Block current task atomically. _sched_block will:
     * 1. Add current task to wait queue
     * 2. Set task state to BLOCKED
     * 3. Call scheduler without releasing NOSCHED lock
     * The lock is released when we context switch to another task.
     */
    _sched_block(s->wait_q);

    /* When we return here, we have been awakened and acquired the semaphore.
     * The signaling task passed the "token" directly to us without incrementing
     * the count, so no further action is needed.
     */
}

int32_t mo_sem_trywait(sem_t *s)
{
    if (unlikely(!sem_is_valid(s)))
        return ERR_FAIL;

    int32_t result = ERR_FAIL;

    NOSCHED_ENTER();

    /* Only succeed if resource available AND no waiters (preserves FIFO) */
    if (s->count > 0 && queue_count(s->wait_q) == 0) {
        s->count--;
        result = ERR_OK;
    }

    NOSCHED_LEAVE();
    return result;
}

void mo_sem_signal(sem_t *s)
{
    if (unlikely(!sem_is_valid(s))) {
        /* Invalid semaphore - this is a programming error */
        panic(ERR_SEM_OPERATION);
    }

    bool should_yield = false;
    tcb_t *awakened_task = NULL;

    NOSCHED_ENTER();

    /* Check if any tasks are waiting for resources */
    if (queue_count(s->wait_q) > 0) {
        /* Wake up the oldest waiting task (FIFO order) */
        awakened_task = queue_dequeue(s->wait_q);
        if (likely(awakened_task)) {
            /* Validate awakened task state consistency */
            if (likely(awakened_task->state == TASK_BLOCKED)) {
                _sched_block_enqueue(awakened_task);
                should_yield = true;
            } else {
                /* Task state inconsistency - this should not happen */
                panic(ERR_SEM_OPERATION);
            }
        }
        /* Note: count is NOT incremented - the "token" is passed directly to
         * the awakened task to prevent race conditions where the count could
         * be decremented by another task between our increment and the
         * awakened task's execution.
         */
    } else {
        /* No waiting tasks - increment available resource count */
        if (likely(s->count < SEM_MAX_COUNT))
            s->count++;

        /* Silently ignore overflow - semaphore remains at max count.
         * This prevents wraparound while maintaining system stability.
         */
    }

    NOSCHED_LEAVE();

    /* Yield outside critical section if we awakened a task.
     * This improves system responsiveness by allowing the awakened task to run
     * immediately if it has higher priority.
     */
    if (should_yield)
        mo_task_yield();
}

int32_t mo_sem_getvalue(sem_t *s)
{
    if (unlikely(!sem_is_valid(s)))
        return -1;

    /* This read is inherently racy - the value may change immediately after
     * being read. The volatile keyword ensures we read the current value from
     * memory, but does not provide atomicity across multiple operations.
     * Callers should not rely on this value for synchronization decisions.
     */
    return s->count;
}

int32_t mo_sem_waiting_count(sem_t *s)
{
    if (unlikely(!sem_is_valid(s)))
        return -1;

    int32_t count;

    NOSCHED_ENTER();
    count = queue_count(s->wait_q);
    NOSCHED_LEAVE();

    return count;
}
