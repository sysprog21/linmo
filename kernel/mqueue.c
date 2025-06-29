/* message queues backed by the generic queue_t */

#include <lib/malloc.h>
#include <lib/queue.h>

#include <sys/mqueue.h>
#include <sys/task.h>

#include <spinlock.h>

#include "private/error.h"

static spinlock_t queue_lock = SPINLOCK_INITIALIZER;
static uint32_t queue_flags = 0;

mq_t *mo_mq_create(uint16_t max_items)
{
    mq_t *mq = malloc(sizeof *mq);
    if (!mq)
        return NULL;

    mq->q = queue_create(max_items);
    if (!mq->q) {
        free(mq);
        return NULL;
    }
    return mq;
}

int32_t mo_mq_destroy(mq_t *mq)
{
    spin_lock_irqsave(&queue_lock, &queue_flags);

    if (queue_count(mq->q) != 0) { /* refuse to destroy non-empty q */
        spin_unlock_irqrestore(&queue_lock, queue_flags);
        return ERR_MQ_NOTEMPTY;
    }

    queue_destroy(mq->q);
    free(mq);

    spin_unlock_irqrestore(&queue_lock, queue_flags);
    return ERR_OK;
}

int32_t mo_mq_enqueue(mq_t *mq, message_t *msg)
{
    int32_t rc;

    spin_lock_irqsave(&queue_lock, &queue_flags);
    rc = queue_enqueue(mq->q, msg);
    spin_unlock_irqrestore(&queue_lock, queue_flags);

    return rc; /* 0 on success, −1 on full */
}

/* remove oldest message (FIFO) */
message_t *mo_mq_dequeue(mq_t *mq)
{
    message_t *msg;

    spin_lock_irqsave(&queue_lock, &queue_flags);
    msg = queue_dequeue(mq->q);
    spin_unlock_irqrestore(&queue_lock, queue_flags);

    return msg; /* NULL when queue is empty */
}

/* inspect head without removing */
message_t *mo_mq_peek(mq_t *mq)
{
    message_t *msg;

    spin_lock_irqsave(&queue_lock, &queue_flags);
    msg = queue_peek(mq->q);
    spin_unlock_irqrestore(&queue_lock, queue_flags);

    return msg; /* NULL when queue is empty */
}
