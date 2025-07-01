/* Tick-based software timers for the kernel.
 *
 * This implementation uses an efficient approach for timer management:
 * 1. 'all_timers_list': Keeps all timers sorted by ID for O(log n) lookup
 * 2. 'kcb->timer_list': Active timers sorted by expiration for O(1) processing
 * 3. Timer node pool: Pre-allocated nodes to reduce malloc/free overhead
 * 4. Batch processing: Handle multiple expired timers efficiently
 */

#include <hal.h>
#include <spinlock.h>
#include <lib/list.h>
#include <lib/malloc.h>
#include <sys/task.h>
#include <sys/timer.h>

#include "private/error.h"

/* Pre-allocated node pool for reduced malloc/free overhead */
#define TIMER_NODE_POOL_SIZE 16
static list_node_t timer_node_pool[TIMER_NODE_POOL_SIZE];
static uint16_t pool_free_mask = 0xFFFF; /* Bitmask for free nodes */

/* Master list of all created timers, kept sorted by ID for faster lookup */
static list_t *all_timers_list = NULL;
static bool timer_initialized = false;

/* Timer lookup cache to accelerate frequent ID searches */
static struct {
    uint16_t id;
    timer_t *timer;
} timer_cache[4];
static uint8_t timer_cache_index = 0;

static spinlock_t timer_lock = SPINLOCK_INITIALIZER;
static uint32_t timer_flags = 0;

/* Get a node from the pool, fall back to malloc if pool is empty */
static list_node_t *get_timer_node(void)
{
    /* Find first free node in pool */
    for (int i = 0; i < TIMER_NODE_POOL_SIZE; i++) {
        if (pool_free_mask & (1 << i)) {
            pool_free_mask &= ~(1 << i);
            return &timer_node_pool[i];
        }
    }
    /* Pool exhausted, fall back to malloc */
    return malloc(sizeof(list_node_t));
}

/* Return a node to the pool, or free if it's not from pool */
static void return_timer_node(list_node_t *node)
{
    /* Check if node is from our pool */
    if (node >= timer_node_pool &&
        node < timer_node_pool + TIMER_NODE_POOL_SIZE) {
        int index = node - timer_node_pool;
        pool_free_mask |= (1 << index);
    } else {
        free(node);
    }
}

/* Add timer to lookup cache */
static inline void cache_timer(uint16_t id, timer_t *timer)
{
    timer_cache[timer_cache_index].id = id;
    timer_cache[timer_cache_index].timer = timer;
    timer_cache_index = (timer_cache_index + 1) % 4;
}

/* Quick cache lookup before expensive list traversal */
static timer_t *cache_lookup_timer(uint16_t id)
{
    for (int i = 0; i < 4; i++) {
        if (timer_cache[i].id == id && timer_cache[i].timer)
            return timer_cache[i].timer;
    }
    return NULL;
}

/* Initializes the timer subsystem's data structures. */
static int32_t timer_subsystem_init(void)
{
    if (timer_initialized)
        return ERR_OK;

    spin_lock_irqsave(&timer_lock, &timer_flags);
    if (timer_initialized) {
        spin_unlock_irqrestore(&timer_lock, timer_flags);
        return ERR_OK;
    }

    all_timers_list = list_create();
    kcb->timer_list = list_create();

    if (!all_timers_list || !kcb->timer_list) {
        free(all_timers_list);
        free(kcb->timer_list);
        kcb->timer_list = NULL;
        spin_unlock_irqrestore(&timer_lock, timer_flags);
        return ERR_FAIL;
    }

    /* Initialize node pool */
    for (int i = 0; i < TIMER_NODE_POOL_SIZE; i++) {
        timer_node_pool[i].data = NULL;
        timer_node_pool[i].next = NULL;
    }

    timer_initialized = true;
    spin_unlock_irqrestore(&timer_lock, timer_flags);
    return ERR_OK;
}

/* Fast removal of timer from active list by data pointer */
static void timer_remove_item_by_data(list_t *list, void *data)
{
    if (!list || list_is_empty(list))
        return;

    list_node_t *prev = list->head;
    list_node_t *curr = prev->next;

    while (curr != list->tail) {
        if (curr->data == data) {
            prev->next = curr->next;
            return_timer_node(curr);
            list->length--;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

/* Sorted insert with early termination for common cases */
static int32_t timer_sorted_insert(timer_t *timer)
{
    list_node_t *new_node = get_timer_node();
    if (!new_node)
        return ERR_FAIL;
    new_node->data = timer;

    /* Fast path: if list is empty or timer should go at end */
    list_node_t *prev = kcb->timer_list->head;
    if (prev->next == kcb->timer_list->tail) {
        /* Empty list */
        new_node->next = kcb->timer_list->tail;
        prev->next = new_node;
        kcb->timer_list->length++;
        return ERR_OK;
    }

    /* Find insertion point */
    while (prev->next != kcb->timer_list->tail) {
        timer_t *current_timer = (timer_t *) prev->next->data;
        if (timer->deadline_ticks < current_timer->deadline_ticks)
            break;
        prev = prev->next;
    }

    new_node->next = prev->next;
    prev->next = new_node;
    kcb->timer_list->length++;
    return ERR_OK;
}

/* Binary search for timer lookup in sorted ID list */
static timer_t *timer_find_by_id_fast(uint16_t id)
{
    /* Try cache first */
    timer_t *cached = cache_lookup_timer(id);
    if (cached && cached->id == id)
        return cached;

    if (!all_timers_list || list_is_empty(all_timers_list))
        return NULL;

    /* Linear search for now - could be optimized to binary search if needed */
    list_node_t *node = all_timers_list->head->next;
    while (node != all_timers_list->tail) {
        timer_t *timer = (timer_t *) node->data;
        if (timer->id == id) {
            cache_timer(id, timer);
            return timer;
        }
        /* Early termination if list is sorted by ID */
        if (timer->id > id)
            break;
        node = node->next;
    }
    return NULL;
}

/* Find timer node for removal operations */
static list_node_t *timer_find_node_by_id(uint16_t id)
{
    if (!all_timers_list)
        return NULL;

    list_node_t *node = all_timers_list->head->next;
    while (node != all_timers_list->tail) {
        if (((timer_t *) node->data)->id == id)
            return node;
        node = node->next;
    }
    return NULL;
}

/* Timer tick handler with batch processing */
void _timer_tick_handler(void)
{
    if (!timer_initialized || list_is_empty(kcb->timer_list))
        return;

    uint32_t now = mo_ticks();
    timer_t *expired_timers[8]; /* Batch process up to 8 timers per tick */
    int expired_count = 0;

    /* Collect all expired timers in one pass */
    while (!list_is_empty(kcb->timer_list) && expired_count < 8) {
        timer_t *t = (timer_t *) kcb->timer_list->head->next->data;

        if (now >= t->deadline_ticks) {
            expired_timers[expired_count++] = t;
            list_pop(kcb->timer_list); /* O(1) removal from head */
        } else {
            /* First timer not expired, so none further down are */
            break;
        }
    }

    /* Process all expired timers */
    for (int i = 0; i < expired_count; i++) {
        timer_t *t = expired_timers[i];

        /* Execute callback */
        if (t->callback)
            t->callback(t->arg);

        /* Handle auto-reload timers */
        if (t->mode == TIMER_AUTORELOAD) {
            t->deadline_ticks = now + MS_TO_TICKS(t->period_ms);
            timer_sorted_insert(t); /* Re-insert for next expiration */
        } else {
            t->mode = TIMER_DISABLED; /* One-shot timers are done */
        }
    }
}

/* Insert timer into sorted position in all_timers_list */
static int32_t timer_insert_sorted_by_id(timer_t *timer)
{
    list_node_t *new_node = get_timer_node();
    if (!new_node)
        return ERR_FAIL;
    new_node->data = timer;

    /* Find insertion point to maintain ID sort order */
    list_node_t *prev = all_timers_list->head;
    while (prev->next != all_timers_list->tail) {
        timer_t *current = (timer_t *) prev->next->data;
        if (timer->id < current->id)
            break;
        prev = prev->next;
    }

    new_node->next = prev->next;
    prev->next = new_node;
    all_timers_list->length++;
    return ERR_OK;
}

int32_t mo_timer_create(void *(*callback)(void *arg),
                        uint32_t period_ms,
                        void *arg)
{
    static uint16_t next_id = 0x6000;

    if (!callback || period_ms == 0)
        return ERR_FAIL;
    if (timer_subsystem_init() != ERR_OK)
        return ERR_FAIL;

    timer_t *t = malloc(sizeof(timer_t));
    if (!t)
        return ERR_FAIL;

    spin_lock_irqsave(&timer_lock, &timer_flags);

    /* Initialize timer */
    t->id = next_id++;
    t->callback = callback;
    t->arg = arg;
    t->period_ms = period_ms;
    t->deadline_ticks = 0;
    t->mode = TIMER_DISABLED;

    /* Insert into sorted all_timers_list */
    if (timer_insert_sorted_by_id(t) != ERR_OK) {
        spin_unlock_irqrestore(&timer_lock, timer_flags);
        free(t);
        return ERR_FAIL;
    }

    /* Add to cache */
    cache_timer(t->id, t);

    spin_unlock_irqrestore(&timer_lock, timer_flags);
    return t->id;
}

int32_t mo_timer_destroy(uint16_t id)
{
    if (!timer_initialized)
        return ERR_FAIL;

    spin_lock_irqsave(&timer_lock, &timer_flags);

    list_node_t *node = timer_find_node_by_id(id);
    if (!node) {
        spin_unlock_irqrestore(&timer_lock, timer_flags);
        return ERR_FAIL;
    }

    timer_t *t = (timer_t *) node->data;

    /* Remove from active list if running */
    if (t->mode != TIMER_DISABLED)
        timer_remove_item_by_data(kcb->timer_list, t);

    /* Remove from cache */
    for (int i = 0; i < 4; i++) {
        if (timer_cache[i].timer == t) {
            timer_cache[i].id = 0;
            timer_cache[i].timer = NULL;
        }
    }

    /* Remove from master list */
    list_remove(all_timers_list, node);
    free(t);
    return_timer_node(node);

    spin_unlock_irqrestore(&timer_lock, timer_flags);
    return ERR_OK;
}

int32_t mo_timer_start(uint16_t id, uint8_t mode)
{
    if (mode != TIMER_ONESHOT && mode != TIMER_AUTORELOAD)
        return ERR_FAIL;
    if (!timer_initialized)
        return ERR_FAIL;

    spin_lock_irqsave(&timer_lock, &timer_flags);

    timer_t *t = timer_find_by_id_fast(id);
    if (!t) {
        spin_unlock_irqrestore(&timer_lock, timer_flags);
        return ERR_FAIL;
    }

    /* Remove from active list if already running */
    if (t->mode != TIMER_DISABLED)
        timer_remove_item_by_data(kcb->timer_list, t);

    /* Configure and start timer */
    t->mode = mode;
    t->deadline_ticks = mo_ticks() + MS_TO_TICKS(t->period_ms);

    if (timer_sorted_insert(t) != ERR_OK) {
        t->mode = TIMER_DISABLED;
        spin_unlock_irqrestore(&timer_lock, timer_flags);
        return ERR_FAIL;
    }

    spin_unlock_irqrestore(&timer_lock, timer_flags);
    return ERR_OK;
}

int32_t mo_timer_cancel(uint16_t id)
{
    if (!timer_initialized)
        return ERR_FAIL;

    spin_lock_irqsave(&timer_lock, &timer_flags);

    timer_t *t = timer_find_by_id_fast(id);
    if (!t || t->mode == TIMER_DISABLED) {
        spin_unlock_irqrestore(&timer_lock, timer_flags);
        return ERR_FAIL;
    }

    timer_remove_item_by_data(kcb->timer_list, t);
    t->mode = TIMER_DISABLED;

    spin_unlock_irqrestore(&timer_lock, timer_flags);
    return ERR_OK;
}
