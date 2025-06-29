/* Mutex and Condition Variable Implementation
 *
 * This implementation provides non-recursive mutexes and condition variables
 * that are independent of the semaphore module.
 */

#include <spinlock.h>
#include <lib/libc.h>
#include <sys/mutex.h>
#include <sys/task.h>

#include "private/error.h"

static spinlock_t mutex_lock = SPINLOCK_INITIALIZER;
static uint32_t mutex_flags = 0;

/* Validate mutex pointer and structure integrity */
static inline bool mutex_is_valid(const mutex_t *m)
{
    return (m && m->magic == MUTEX_MAGIC && m->waiters);
}

/* Validate condition variable pointer and structure integrity */
static inline bool cond_is_valid(const cond_t *c)
{
    return (c && c->magic == COND_MAGIC && c->waiters);
}

/* Find a list node containing a specific data pointer. */
static list_node_t *find_node_by_data(list_t *list, void *data)
{
    if (!list || !data)
        return NULL;

    /* Start from first real node (skip head sentinel) */
    list_node_t *curr = list->head->next;
    while (curr && curr != list->tail) {
        if (curr->data == data)
            return curr;
        curr = curr->next;
    }
    return NULL;
}

/* Atomic block-on-waitlist operation for mutexes.
 * Must be called within NOSCHED critical section.
 */
static void mutex_block_atomic(list_t *waiters)
{
    if (!waiters || !kcb || !get_task_current() || !get_task_current()->data)
        panic(ERR_SEM_OPERATION);

    tcb_t *self = get_task_current()->data;

    /* Add to waiters list */
    if (!list_pushback(waiters, self))
        panic(ERR_SEM_OPERATION);

    /* Block and yield atomically */
    self->state = TASK_BLOCKED;
    _yield(); /* This releases NOSCHED when we context switch */
}

int32_t mo_mutex_init(mutex_t *m)
{
    if (!m)
        return ERR_FAIL;

    /* Initialize to known state */
    m->waiters = NULL;
    m->owner_tid = 0;
    m->magic = 0;

    /* Create waiters list */
    m->waiters = list_create();
    if (!m->waiters)
        return ERR_FAIL;

    /* Mark as valid (last step) */
    m->owner_tid = 0;
    m->magic = MUTEX_MAGIC;

    return ERR_OK;
}

int32_t mo_mutex_destroy(mutex_t *m)
{
    if (!m)
        return ERR_OK; /* Destroying NULL is no-op */

    if (!mutex_is_valid(m))
        return ERR_FAIL;

    spin_lock_irqsave(&mutex_lock, &mutex_flags);

    /* Check if any tasks are waiting */
    if (!list_is_empty(m->waiters)) {
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        return ERR_TASK_BUSY;
    }

    /* Check if mutex is still owned */
    if (m->owner_tid != 0) {
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        return ERR_TASK_BUSY;
    }

    /* Invalidate and cleanup */
    m->magic = 0;
    list_t *waiters = m->waiters;
    m->waiters = NULL;
    m->owner_tid = 0;

    spin_unlock_irqrestore(&mutex_lock, mutex_flags);

    list_destroy(waiters);
    return ERR_OK;
}

int32_t mo_mutex_lock(mutex_t *m)
{
    if (!mutex_is_valid(m))
        panic(ERR_SEM_OPERATION); /* Invalid mutex is programming error */

    uint16_t self_tid = mo_task_id();

    spin_lock_irqsave(&mutex_lock, &mutex_flags);

    /* Non-recursive: reject if caller already owns it */
    if (m->owner_tid == self_tid) {
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        return ERR_TASK_BUSY;
    }

    /* Fast path: mutex is free, acquire immediately */
    if (m->owner_tid == 0) {
        m->owner_tid = self_tid;
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
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
    if (!mutex_is_valid(m))
        return ERR_FAIL;

    uint16_t self_tid = mo_task_id();
    int32_t result = ERR_TASK_BUSY;

    spin_lock_irqsave(&mutex_lock, &mutex_flags);

    if (m->owner_tid == self_tid) {
        /* Already owned by caller (non-recursive) */
        result = ERR_TASK_BUSY;
    } else if (m->owner_tid == 0) {
        /* Mutex is free, acquire it */
        m->owner_tid = self_tid;
        result = ERR_OK;
    }
    /* else: owned by someone else, return ERR_TASK_BUSY */

    spin_unlock_irqrestore(&mutex_lock, mutex_flags);
    return result;
}

int32_t mo_mutex_timedlock(mutex_t *m, uint32_t ticks)
{
    if (!mutex_is_valid(m))
        return ERR_FAIL;

    if (ticks == 0)
        return mo_mutex_trylock(m); /* Zero timeout = try only */

    uint16_t self_tid = mo_task_id();
    uint32_t deadline = mo_ticks() + ticks;

    spin_lock_irqsave(&mutex_lock, &mutex_flags);

    /* Non-recursive check */
    if (m->owner_tid == self_tid) {
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        return ERR_TASK_BUSY;
    }

    /* Fast path: mutex is free */
    if (m->owner_tid == 0) {
        m->owner_tid = self_tid;
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        return ERR_OK;
    }

    /* Must block with timeout */
    tcb_t *self = get_task_current()->data;
    if (!list_pushback(m->waiters, self)) {
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        panic(ERR_SEM_OPERATION);
    }
    self->state = TASK_BLOCKED;

    spin_unlock_irqrestore(&mutex_lock, mutex_flags);

    /* Wait loop with timeout check */
    while (self->state == TASK_BLOCKED && mo_ticks() < deadline)
        mo_task_yield();

    int32_t status;
    spin_lock_irqsave(&mutex_lock, &mutex_flags);

    if (self->state == TASK_BLOCKED) {
        /* Timeout occurred - remove ourselves from wait list */
        list_node_t *self_node = find_node_by_data(m->waiters, self);
        if (self_node) {
            list_remove(m->waiters, self_node);
            free(self_node);
        }
        self->state = TASK_READY;
        status = ERR_TIMEOUT;
    } else {
        /* Success - we were woken by unlock and granted ownership */
        status = ERR_OK;
    }

    spin_unlock_irqrestore(&mutex_lock, mutex_flags);
    return status;
}

int32_t mo_mutex_unlock(mutex_t *m)
{
    if (!mutex_is_valid(m))
        return ERR_FAIL;

    uint16_t self_tid = mo_task_id();

    spin_lock_irqsave(&mutex_lock, &mutex_flags);

    /* Verify caller owns the mutex */
    if (m->owner_tid != self_tid) {
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        return ERR_NOT_OWNER;
    }

    /* Check for waiting tasks */
    if (list_is_empty(m->waiters)) {
        /* No waiters - mutex becomes free */
        m->owner_tid = 0;
    } else {
        /* Transfer ownership to next waiter (FIFO) */
        tcb_t *next_owner = (tcb_t *) list_pop(m->waiters);
        if (next_owner) {
            /* Validate task state before waking */
            if (next_owner->state == TASK_BLOCKED) {
                m->owner_tid = next_owner->id;
                next_owner->state = TASK_READY;
            } else {
                /* Task state inconsistency */
                panic(ERR_SEM_OPERATION);
            }
        } else {
            /* Should not happen if list was not empty */
            m->owner_tid = 0;
        }
    }

    spin_unlock_irqrestore(&mutex_lock, mutex_flags);
    return ERR_OK;
}

bool mo_mutex_owned_by_current(mutex_t *m)
{
    if (!mutex_is_valid(m))
        return false;

    return (m->owner_tid == mo_task_id());
}

int32_t mo_mutex_waiting_count(mutex_t *m)
{
    if (!mutex_is_valid(m))
        return -1;

    int32_t count;
    spin_lock_irqsave(&mutex_lock, &mutex_flags);
    count = m->waiters ? (int32_t) m->waiters->length : 0;
    spin_unlock_irqrestore(&mutex_lock, mutex_flags);

    return count;
}

int32_t mo_cond_init(cond_t *c)
{
    if (!c)
        return ERR_FAIL;

    /* Initialize to known state */
    c->waiters = NULL;
    c->magic = 0;

    /* Create waiters list */
    c->waiters = list_create();
    if (!c->waiters)
        return ERR_FAIL;

    /* Mark as valid */
    c->magic = COND_MAGIC;
    return ERR_OK;
}

int32_t mo_cond_destroy(cond_t *c)
{
    if (!c)
        return ERR_OK; /* Destroying NULL is no-op */

    if (!cond_is_valid(c))
        return ERR_FAIL;

    spin_lock_irqsave(&mutex_lock, &mutex_flags);

    /* Check if any tasks are waiting */
    if (!list_is_empty(c->waiters)) {
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        return ERR_TASK_BUSY;
    }

    /* Invalidate and cleanup */
    c->magic = 0;
    list_t *waiters = c->waiters;
    c->waiters = NULL;

    spin_unlock_irqrestore(&mutex_lock, mutex_flags);

    list_destroy(waiters);
    return ERR_OK;
}

int32_t mo_cond_wait(cond_t *c, mutex_t *m)
{
    if (!cond_is_valid(c) || !mutex_is_valid(m)) {
        /* Invalid parameters are programming errors */
        panic(ERR_SEM_OPERATION);
    }

    /* Verify caller owns the mutex */
    if (!mo_mutex_owned_by_current(m))
        return ERR_NOT_OWNER;

    /* Atomically add to wait list and block */
    spin_lock_irqsave(&mutex_lock, &mutex_flags);
    tcb_t *self = get_task_current()->data;
    if (!list_pushback(c->waiters, self)) {
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        panic(ERR_SEM_OPERATION);
    }
    self->state = TASK_BLOCKED;
    spin_unlock_irqrestore(&mutex_lock, mutex_flags);

    /* Release mutex and yield */
    int32_t unlock_result = mo_mutex_unlock(m);
    if (unlock_result != ERR_OK) {
        /* Failed to unlock - remove from wait list */
        spin_lock_irqsave(&mutex_lock, &mutex_flags);
        list_node_t *self_node = find_node_by_data(c->waiters, self);
        if (self_node) {
            list_remove(c->waiters, self_node);
            free(self_node);
        }
        self->state = TASK_READY;
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        return unlock_result;
    }

    mo_task_yield();

    /* Re-acquire mutex before returning */
    return mo_mutex_lock(m);
}

int32_t mo_cond_timedwait(cond_t *c, mutex_t *m, uint32_t ticks)
{
    if (!cond_is_valid(c) || !mutex_is_valid(m))
        panic(ERR_SEM_OPERATION);

    if (!mo_mutex_owned_by_current(m))
        return ERR_NOT_OWNER;

    if (ticks == 0) {
        /* Zero timeout - don't wait at all */
        return ERR_TIMEOUT;
    }

    uint32_t deadline = mo_ticks() + ticks;

    /* Atomically add to wait list */
    spin_lock_irqsave(&mutex_lock, &mutex_flags);
    tcb_t *self = get_task_current()->data;
    if (!list_pushback(c->waiters, self)) {
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        panic(ERR_SEM_OPERATION);
    }
    self->state = TASK_BLOCKED;
    spin_unlock_irqrestore(&mutex_lock, mutex_flags);

    /* Release mutex */
    int32_t unlock_result = mo_mutex_unlock(m);
    if (unlock_result != ERR_OK) {
        /* Failed to unlock - cleanup */
        spin_lock_irqsave(&mutex_lock, &mutex_flags);
        list_node_t *self_node = find_node_by_data(c->waiters, self);
        if (self_node) {
            list_remove(c->waiters, self_node);
            free(self_node);
        }
        self->state = TASK_READY;
        spin_unlock_irqrestore(&mutex_lock, mutex_flags);
        return unlock_result;
    }

    /* Wait with timeout */
    while (self->state == TASK_BLOCKED && mo_ticks() < deadline) {
        mo_task_yield();
    }

    int32_t wait_status;
    spin_lock_irqsave(&mutex_lock, &mutex_flags);

    if (self->state == TASK_BLOCKED) {
        /* Timeout - remove from wait list */
        list_node_t *self_node = find_node_by_data(c->waiters, self);
        if (self_node) {
            list_remove(c->waiters, self_node);
            free(self_node);
        }
        self->state = TASK_READY;
        wait_status = ERR_TIMEOUT;
    } else {
        /* Signaled successfully */
        wait_status = ERR_OK;
    }

    spin_unlock_irqrestore(&mutex_lock, mutex_flags);

    /* Re-acquire mutex regardless of timeout status */
    int32_t lock_result = mo_mutex_lock(m);

    /* Return timeout status if wait timed out, otherwise lock result */
    return (wait_status == ERR_TIMEOUT) ? ERR_TIMEOUT : lock_result;
}

int32_t mo_cond_signal(cond_t *c)
{
    if (!cond_is_valid(c))
        return ERR_FAIL;

    spin_lock_irqsave(&mutex_lock, &mutex_flags);

    if (!list_is_empty(c->waiters)) {
        tcb_t *waiter = (tcb_t *) list_pop(c->waiters);
        if (waiter) {
            /* Validate task state before waking */
            if (waiter->state == TASK_BLOCKED) {
                waiter->state = TASK_READY;
            } else {
                /* Task state inconsistency */
                panic(ERR_SEM_OPERATION);
            }
        }
    }

    spin_unlock_irqrestore(&mutex_lock, mutex_flags);
    return ERR_OK;
}

int32_t mo_cond_broadcast(cond_t *c)
{
    if (!cond_is_valid(c))
        return ERR_FAIL;

    spin_lock_irqsave(&mutex_lock, &mutex_flags);

    while (!list_is_empty(c->waiters)) {
        tcb_t *waiter = (tcb_t *) list_pop(c->waiters);
        if (waiter) {
            /* Validate task state before waking */
            if (waiter->state == TASK_BLOCKED) {
                waiter->state = TASK_READY;
            } else {
                /* Task state inconsistency */
                panic(ERR_SEM_OPERATION);
            }
        }
    }

    spin_unlock_irqrestore(&mutex_lock, mutex_flags);
    return ERR_OK;
}

int32_t mo_cond_waiting_count(cond_t *c)
{
    if (!cond_is_valid(c))
        return -1;

    int32_t count;
    spin_lock_irqsave(&mutex_lock, &mutex_flags);
    count = c->waiters ? (int32_t) c->waiters->length : 0;
    spin_unlock_irqrestore(&mutex_lock, mutex_flags);

    return count;
}
