/* Mutex and Condition Variable Implementation
 *
 * This implementation provides non-recursive mutexes and condition variables
 * that are independent of the semaphore module.
 */

#include <lib/libc.h>
#include <sys/mutex.h>
#include <sys/task.h>

#include "private/error.h"
#include "private/utils.h"

/* Validate mutex pointer and structure integrity */
static inline bool mutex_is_valid(const mutex_t *m)
{
    return m && m->magic == MUTEX_MAGIC && m->waiters &&
           (m->owner_tid == 0 || m->owner_tid < UINT16_MAX);
}

/* Validate condition variable pointer and structure integrity */
static inline bool cond_is_valid(const cond_t *c)
{
    return c && c->magic == COND_MAGIC && c->waiters;
}

/* Invalidate mutex during destruction to prevent reuse */
static inline void mutex_invalidate(mutex_t *m)
{
    if (m) {
        m->magic = 0xDEADBEEF;
        m->owner_tid = UINT16_MAX; /* Invalid TID */
    }
}

/* Invalidate condition variable during destruction */
static inline void cond_invalidate(cond_t *c)
{
    if (c)
        c->magic = 0xDEADBEEF;
}

/* Remove current task from waiter list, avoiding the need to search through
 * the entire list.
 */
static bool remove_self_from_waiters(list_t *waiters)
{
    if (unlikely(!waiters || !kcb || !kcb->task_current))
        return false;

    tcb_t *self = tcb_from_global_node(kcb->task_current);

    /* Search for and remove self from waiters list */
    list_node_t *curr_mutex_node = waiters->head->next;
    while (curr_mutex_node && curr_mutex_node != waiters->tail) {
        if (tcb_from_mutex_node(curr_mutex_node) == self) {
            list_remove(waiters, curr_mutex_node);
            return true;
        }
        curr_mutex_node = curr_mutex_node->next;
    }
    return false;
}

/* Atomic block operation with enhanced error checking */
static void mutex_block_atomic(list_t *waiters)
{
    if (unlikely(!waiters || !kcb || !kcb->task_current))
        panic(ERR_SEM_OPERATION);

    tcb_t *self = tcb_from_global_node(kcb->task_current);

    /* Add to waiters list */
    if (unlikely(!list_pushback(waiters, &self->mutex_node)))
        panic(ERR_SEM_OPERATION);

    /* Block and yield atomically */
    self->state = TASK_BLOCKED;
    _yield(); /* This releases NOSCHED when we context switch */
}

int32_t mo_mutex_init(mutex_t *m)
{
    if (unlikely(!m))
        return ERR_FAIL;

    /* Initialize to known safe state */
    m->waiters = NULL;
    m->owner_tid = 0;
    m->magic = 0;

    /* Create waiters list */
    m->waiters = list_create();
    if (unlikely(!m->waiters))
        return ERR_FAIL;

    /* Mark as valid atomically (last step) */
    m->owner_tid = 0;
    m->magic = MUTEX_MAGIC;

    return ERR_OK;
}

int32_t mo_mutex_destroy(mutex_t *m)
{
    if (!m)
        return ERR_OK; /* Destroying NULL is no-op */

    if (unlikely(!mutex_is_valid(m)))
        return ERR_FAIL;

    NOSCHED_ENTER();

    /* Check if any tasks are waiting */
    if (unlikely(!list_is_empty(m->waiters))) {
        NOSCHED_LEAVE();
        return ERR_TASK_BUSY;
    }

    /* Check if mutex is still owned */
    if (unlikely(m->owner_tid != 0)) {
        NOSCHED_LEAVE();
        return ERR_TASK_BUSY;
    }

    /* Invalidate atomically and cleanup */
    mutex_invalidate(m);
    list_t *waiters = m->waiters;
    m->waiters = NULL;
    m->owner_tid = 0;

    NOSCHED_LEAVE();

    /* Clean up resources outside critical section */
    list_destroy(waiters);
    return ERR_OK;
}

int32_t mo_mutex_lock(mutex_t *m)
{
    if (unlikely(!mutex_is_valid(m)))
        panic(ERR_SEM_OPERATION); /* Invalid mutex is programming error */

    uint16_t self_tid = mo_task_id();

    NOSCHED_ENTER();

    /* Non-recursive: reject if caller already owns it */
    if (unlikely(m->owner_tid == self_tid)) {
        NOSCHED_LEAVE();
        return ERR_TASK_BUSY;
    }

    /* Fast path: mutex is free, acquire immediately */
    if (likely(m->owner_tid == 0)) {
        m->owner_tid = self_tid;
        NOSCHED_LEAVE();
        return ERR_OK;
    }

    /* Slow path: mutex is owned, must block atomically */
    mutex_block_atomic(m->waiters);

    /* When we return here, we've been woken by mo_mutex_unlock()
     * and ownership has been transferred to us. */
    return ERR_OK;
}

int32_t mo_mutex_trylock(mutex_t *m)
{
    if (unlikely(!mutex_is_valid(m)))
        return ERR_FAIL;

    uint16_t self_tid = mo_task_id();
    int32_t result = ERR_TASK_BUSY;

    NOSCHED_ENTER();

    if (unlikely(m->owner_tid == self_tid)) {
        /* Already owned by caller (non-recursive) */
        result = ERR_TASK_BUSY;
    } else if (m->owner_tid == 0) {
        /* Mutex is free, acquire it */
        m->owner_tid = self_tid;
        result = ERR_OK;
    }
    /* else: owned by someone else, return ERR_TASK_BUSY */

    NOSCHED_LEAVE();
    return result;
}

int32_t mo_mutex_timedlock(mutex_t *m, uint32_t ticks)
{
    if (unlikely(!mutex_is_valid(m)))
        return ERR_FAIL;

    if (ticks == 0)
        return mo_mutex_trylock(m); /* Zero timeout = try only */

    uint16_t self_tid = mo_task_id();

    NOSCHED_ENTER();

    /* Non-recursive check */
    if (unlikely(m->owner_tid == self_tid)) {
        NOSCHED_LEAVE();
        return ERR_TASK_BUSY;
    }

    /* Fast path: mutex is free */
    if (m->owner_tid == 0) {
        m->owner_tid = self_tid;
        NOSCHED_LEAVE();
        return ERR_OK;
    }

    /* Slow path: must block with timeout using delay mechanism */
    tcb_t *self = tcb_from_global_node(kcb->task_current);
    if (unlikely(!list_pushback(m->waiters, &self->mutex_node))) {
        NOSCHED_LEAVE();
        panic(ERR_SEM_OPERATION);
    }

    /* Set up timeout using task delay mechanism */
    self->delay = ticks;
    self->state = TASK_BLOCKED;

    NOSCHED_LEAVE();

    /* Yield and let the scheduler handle timeout via delay mechanism */
    mo_task_yield();

    /* Check result after waking up */
    int32_t result;

    NOSCHED_ENTER();
    if (self->state == TASK_BLOCKED) {
        /* We woke up due to timeout, not mutex unlock */
        if (remove_self_from_waiters(m->waiters)) {
            self->state = TASK_READY;
            result = ERR_TIMEOUT;
        } else {
            /* Race condition: we were both timed out and unlocked */
            /* Check if we now own the mutex */
            result = (m->owner_tid == self_tid) ? ERR_OK : ERR_TIMEOUT;
        }
    } else {
        /* We were woken by mutex unlock - check ownership */
        result = (m->owner_tid == self_tid) ? ERR_OK : ERR_FAIL;
    }
    NOSCHED_LEAVE();

    return result;
}

int32_t mo_mutex_unlock(mutex_t *m)
{
    if (unlikely(!mutex_is_valid(m)))
        return ERR_FAIL;

    uint16_t self_tid = mo_task_id();

    NOSCHED_ENTER();

    /* Verify caller owns the mutex */
    if (unlikely(m->owner_tid != self_tid)) {
        NOSCHED_LEAVE();
        return ERR_NOT_OWNER;
    }

    /* Check for waiting tasks */
    if (list_is_empty(m->waiters)) {
        /* No waiters - mutex becomes free */
        m->owner_tid = 0;
    } else {
        /* Transfer ownership to next waiter (FIFO) */
        list_node_t *next_owner_node = (list_node_t *) list_pop(m->waiters);
        tcb_t *next_owner = tcb_from_mutex_node(next_owner_node);
        if (likely(next_owner)) {
            /* Validate task state before waking */
            if (likely(next_owner->state == TASK_BLOCKED)) {
                m->owner_tid = next_owner->id;
                next_owner->state = TASK_READY;
                /* Clear any pending timeout since we're granting ownership */
                next_owner->delay = 0;
            } else {
                /* Task state inconsistency */
                panic(ERR_SEM_OPERATION);
            }
        } else {
            /* Should not happen if list was not empty */
            m->owner_tid = 0;
        }
    }

    NOSCHED_LEAVE();
    return ERR_OK;
}

bool mo_mutex_owned_by_current(mutex_t *m)
{
    if (unlikely(!mutex_is_valid(m)))
        return false;

    return (m->owner_tid == mo_task_id());
}

int32_t mo_mutex_waiting_count(mutex_t *m)
{
    if (unlikely(!mutex_is_valid(m)))
        return -1;

    int32_t count;
    NOSCHED_ENTER();
    count = m->waiters ? (int32_t) m->waiters->length : 0;
    NOSCHED_LEAVE();

    return count;
}

int32_t mo_cond_init(cond_t *c)
{
    if (unlikely(!c))
        return ERR_FAIL;

    /* Initialize to known safe state */
    c->waiters = NULL;
    c->magic = 0;

    /* Create waiters list */
    c->waiters = list_create();
    if (unlikely(!c->waiters))
        return ERR_FAIL;

    /* Mark as valid atomically */
    c->magic = COND_MAGIC;
    return ERR_OK;
}

int32_t mo_cond_destroy(cond_t *c)
{
    if (!c)
        return ERR_OK; /* Destroying NULL is no-op */

    if (unlikely(!cond_is_valid(c)))
        return ERR_FAIL;

    NOSCHED_ENTER();

    /* Check if any tasks are waiting */
    if (unlikely(!list_is_empty(c->waiters))) {
        NOSCHED_LEAVE();
        return ERR_TASK_BUSY;
    }

    /* Invalidate atomically and cleanup */
    cond_invalidate(c);
    list_t *waiters = c->waiters;
    c->waiters = NULL;

    NOSCHED_LEAVE();

    /* Clean up resources outside critical section */
    list_destroy(waiters);
    return ERR_OK;
}

int32_t mo_cond_wait(cond_t *c, mutex_t *m)
{
    if (unlikely(!cond_is_valid(c) || !mutex_is_valid(m))) {
        /* Invalid parameters are programming errors */
        panic(ERR_SEM_OPERATION);
    }

    /* Verify caller owns the mutex */
    if (unlikely(!mo_mutex_owned_by_current(m)))
        return ERR_NOT_OWNER;

    tcb_t *self = tcb_from_global_node(kcb->task_current);

    /* Atomically add to wait list */
    NOSCHED_ENTER();
    if (unlikely(!list_pushback(c->waiters, &self->mutex_node))) {
        NOSCHED_LEAVE();
        panic(ERR_SEM_OPERATION);
    }
    self->state = TASK_BLOCKED;
    NOSCHED_LEAVE();

    /* Release mutex */
    int32_t unlock_result = mo_mutex_unlock(m);
    if (unlikely(unlock_result != ERR_OK)) {
        /* Failed to unlock - remove from wait list and restore state */
        NOSCHED_ENTER();
        remove_self_from_waiters(c->waiters);
        self->state = TASK_READY;
        NOSCHED_LEAVE();
        return unlock_result;
    }

    /* Yield and wait to be signaled */
    mo_task_yield();

    /* Re-acquire mutex before returning */
    return mo_mutex_lock(m);
}

int32_t mo_cond_timedwait(cond_t *c, mutex_t *m, uint32_t ticks)
{
    if (unlikely(!cond_is_valid(c) || !mutex_is_valid(m)))
        panic(ERR_SEM_OPERATION);

    if (unlikely(!mo_mutex_owned_by_current(m)))
        return ERR_NOT_OWNER;

    if (ticks == 0) {
        /* Zero timeout - don't wait at all */
        return ERR_TIMEOUT;
    }

    tcb_t *self = tcb_from_global_node(kcb->task_current);

    /* Atomically add to wait list with timeout */
    NOSCHED_ENTER();
    if (unlikely(!list_pushback(c->waiters, &self->mutex_node))) {
        NOSCHED_LEAVE();
        panic(ERR_SEM_OPERATION);
    }
    self->delay = ticks;
    self->state = TASK_BLOCKED;
    NOSCHED_LEAVE();

    /* Release mutex */
    int32_t unlock_result = mo_mutex_unlock(m);
    if (unlikely(unlock_result != ERR_OK)) {
        /* Failed to unlock - cleanup and restore */
        NOSCHED_ENTER();
        remove_self_from_waiters(c->waiters);
        self->state = TASK_READY;
        self->delay = 0;
        NOSCHED_LEAVE();
        return unlock_result;
    }

    /* Yield and wait for signal or timeout */
    mo_task_yield();

    /* Determine why we woke up */
    int32_t wait_status;
    NOSCHED_ENTER();

    if (self->state == TASK_BLOCKED) {
        /* Timeout occurred - remove from wait list */
        remove_self_from_waiters(c->waiters);
        self->state = TASK_READY;
        self->delay = 0;
        wait_status = ERR_TIMEOUT;
    } else {
        /* Signaled successfully */
        wait_status = ERR_OK;
    }

    NOSCHED_LEAVE();

    /* Re-acquire mutex regardless of timeout status */
    int32_t lock_result = mo_mutex_lock(m);

    /* Return timeout status if wait timed out, otherwise lock result */
    return (wait_status == ERR_TIMEOUT) ? ERR_TIMEOUT : lock_result;
}

int32_t mo_cond_signal(cond_t *c)
{
    if (unlikely(!cond_is_valid(c)))
        return ERR_FAIL;

    NOSCHED_ENTER();

    if (!list_is_empty(c->waiters)) {
        tcb_t *waiter = (tcb_t *) list_pop(c->waiters);
        if (likely(waiter)) {
            /* Validate task state before waking */
            if (likely(waiter->state == TASK_BLOCKED)) {
                waiter->state = TASK_READY;
                /* Clear any pending timeout since we're signaling */
                waiter->delay = 0;
            } else {
                /* Task state inconsistency */
                panic(ERR_SEM_OPERATION);
            }
        }
    }

    NOSCHED_LEAVE();
    return ERR_OK;
}

int32_t mo_cond_broadcast(cond_t *c)
{
    if (unlikely(!cond_is_valid(c)))
        return ERR_FAIL;

    NOSCHED_ENTER();

    /* Wake all waiting tasks */
    while (!list_is_empty(c->waiters)) {
        tcb_t *waiter = (tcb_t *) list_pop(c->waiters);
        if (likely(waiter)) {
            /* Validate task state before waking */
            if (likely(waiter->state == TASK_BLOCKED)) {
                waiter->state = TASK_READY;
                /* Clear any pending timeout since we're broadcasting */
                waiter->delay = 0;
            } else {
                /* Task state inconsistency */
                panic(ERR_SEM_OPERATION);
            }
        }
    }

    NOSCHED_LEAVE();
    return ERR_OK;
}

int32_t mo_cond_waiting_count(cond_t *c)
{
    if (unlikely(!cond_is_valid(c)))
        return -1;

    int32_t count;
    NOSCHED_ENTER();
    count = c->waiters ? (int32_t) c->waiters->length : 0;
    NOSCHED_LEAVE();

    return count;
}
