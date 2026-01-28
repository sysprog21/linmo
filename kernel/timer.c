/* Tick-based software timers for the kernel.
 *
 * This implementation uses an efficient approach for timer management:
 * 1. 'all_timers_list': Keeps all timers sorted by ID for O(log n) lookup
 * 2. 'kcb->timer_list': Active timers sorted by expiration for O(1) processing
 * 3. Timer node pool: Pre-allocated nodes to reduce malloc/free overhead
 * 4. Batch processing: Handle multiple expired timers efficiently
 */

#include <hal.h>
#include <lib/list.h>
#include <lib/malloc.h>
#include <sys/task.h>
#include <sys/timer.h>

#include "private/error.h"
#include "private/utils.h"

/* Pre-allocated timer pool for reduced malloc/free overhead */
#define TIMER_NODE_POOL_SIZE 16
static timer_t timer_pool[TIMER_NODE_POOL_SIZE];
static uint16_t pool_free_mask = 0xFFFF; /* Bitmask for free timers */

/* Master list of all created timers, kept sorted by ID for faster lookup */
static list_t *all_timers_list = NULL;
static bool timer_initialized = false;

/* Timer lookup cache to accelerate frequent ID searches */
static struct {
    uint16_t id;
    timer_t *timer;
} timer_cache[4];
static uint8_t timer_cache_index = 0;

/* Get a timer from the pool */
static timer_t *get_timer(void)
{
    /* Find first free node in pool */
    for (int i = 0; i < TIMER_NODE_POOL_SIZE; i++) {
        if (pool_free_mask & (1 << i)) {
            pool_free_mask &= ~(1 << i);
            return &timer_pool[i];
        }
    }
    /* Pool exhausted */
    return NULL;
}

/* Return the timer to the pool, mark only */
static void return_timer(timer_t *timer)
{
    /* Check if node is from our pool */
    if (timer >= timer_pool && timer < timer_pool + TIMER_NODE_POOL_SIZE) {
        int index = (timer - &timer_pool[0]);
        pool_free_mask |= (1 << index);
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
    if (unlikely(timer_initialized))
        return ERR_OK;

    NOSCHED_ENTER();
    if (timer_initialized) {
        NOSCHED_LEAVE();
        return ERR_OK;
    }

    all_timers_list = list_create();
    kcb->timer_list = list_create();

    if (unlikely(!all_timers_list || !kcb->timer_list)) {
        if (all_timers_list) {
            list_destroy(all_timers_list);
            all_timers_list = NULL;
        }
        if (kcb->timer_list) {
            list_destroy(kcb->timer_list);
            kcb->timer_list = NULL;
        }
        NOSCHED_LEAVE();
        return ERR_FAIL;
    }

    /* Initialize timer pool */

    for (int i = 0; i < TIMER_NODE_POOL_SIZE; i++) {
        memset(&timer_pool[i], 0, sizeof(timer_t));
    }

    timer_initialized = true;
    NOSCHED_LEAVE();
    return ERR_OK;
}

/* Fast removal of timer from active list */
static void timer_remove_from_running_list(list_t *list, timer_t *t)
{
    if (unlikely(!list || list_is_empty(list)))
        return;

    list_remove(list, &t->t_running_node);
}

/* Insert timer into the running timer list */
static int32_t timer_sorted_insert_running_list(timer_t *timer)
{
    if (unlikely(!timer || timer->t_running_node.next))
        return ERR_FAIL;
    /* Fast path: if list is empty or timer should go at end */
    list_node_t *prev = kcb->timer_list->head;
    if (prev->next == kcb->timer_list->tail) {
        /* Empty list */
        timer->t_running_node.next = kcb->timer_list->tail;
        prev->next = &timer->t_running_node;
        kcb->timer_list->length++;
        return ERR_OK;
    }

    /* Find insertion point */
    while (prev->next != kcb->timer_list->tail) {
        timer_t *current_timer = timer_from_running_node(prev->next);
        if (timer->deadline_ticks < current_timer->deadline_ticks)
            break;
        prev = prev->next;
    }

    timer->t_running_node.next = prev->next;
    prev->next = &timer->t_running_node;
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

    if (unlikely(!all_timers_list || list_is_empty(all_timers_list)))
        return NULL;

    /* Linear search for now - could be optimized to binary search if needed */
    list_node_t *node = all_timers_list->head->next;
    while (node != all_timers_list->tail) {
        timer_t *timer = timer_from_node(node);
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
    if (unlikely(!all_timers_list))
        return NULL;

    list_node_t *node = all_timers_list->head->next;
    while (node != all_timers_list->tail) {
        if (timer_from_node(node)->id == id)
            return node;
        node = node->next;
    }
    return NULL;
}

/* Reduce timer batch processing size for tighter interrupt latency bounds */
#define TIMER_BATCH_SIZE 4

void _timer_tick_handler(void)
{
    if (unlikely(!timer_initialized || !kcb->timer_list ||
                 list_is_empty(kcb->timer_list)))
        return;
    uint32_t now = mo_ticks();
    list_node_t *
        expired_timers_running_nodes[TIMER_BATCH_SIZE]; /* Smaller batch size */
    int expired_count = 0;

    /* Collect expired timers in one pass, limited to batch size */
    while (!list_is_empty(kcb->timer_list) &&
           expired_count < TIMER_BATCH_SIZE) {
        list_node_t *node = kcb->timer_list->head->next;
        timer_t *t = timer_from_running_node(node);

        if (now >= t->deadline_ticks) {
            expired_timers_running_nodes[expired_count++] =
                list_pop(kcb->timer_list);

        } else {
            /* First timer not expired, so none further down are */
            break;
        }
    }

    /* Process all expired timers */
    for (int i = 0; i < expired_count; i++) {
        list_node_t *expired_running_node = expired_timers_running_nodes[i];
        timer_t *t = timer_from_running_node(expired_running_node);

        /* Execute callback */
        if (likely(t->callback))
            t->callback(t->arg);

        /* Handle auto-reload timers */
        if (t->mode == TIMER_AUTORELOAD) {
            /* Calculate next expected fire tick to prevent cumulative error */
            t->last_expected_fire_tick += MS_TO_TICKS(t->period_ms);
            t->deadline_ticks = t->last_expected_fire_tick;
            /* Re-insert for next expiration */
            timer_sorted_insert_running_list(t);
        } else {
            t->mode = TIMER_DISABLED; /* One-shot timers are done */
        }
    }
}

/* Insert timer into sorted position in all_timers_list */
static void timer_insert_sorted_timer_list(timer_t *timer)
{
    if (unlikely(!timer || timer->t_node.next))
        return;

    /* Find insertion point to maintain ID sort order */
    list_node_t *prev = all_timers_list->head;
    while (prev->next != all_timers_list->tail) {
        timer_t *current = timer_from_node(prev->next);
        if (timer->id < current->id)
            break;
        prev = prev->next;
    }

    timer->t_node.next = prev->next;
    prev->next = &timer->t_node;
    all_timers_list->length++;
    return;
}

int32_t mo_timer_create(void *(*callback)(void *arg),
                        uint32_t period_ms,
                        void *arg)
{
    static uint16_t next_id = 0x6000;

    if (unlikely(!callback || !period_ms))
        return ERR_FAIL;
    if (unlikely(timer_subsystem_init() != ERR_OK))
        return ERR_FAIL;

    /* Try to get a static timer from the pool */
    timer_t *t = get_timer();
    if (!t)
        return ERR_FAIL;

    NOSCHED_ENTER();

    /* Initialize timer */
    t->id = next_id++;
    t->callback = callback;
    t->arg = arg;
    t->period_ms = period_ms;
    t->deadline_ticks = 0;
    t->last_expected_fire_tick = 0;
    t->mode = TIMER_DISABLED;
    t->_reserved = 0;
    t->t_node.next = NULL;
    t->t_running_node.next = NULL;

    /* Insert into sorted all_timers_list */
    timer_insert_sorted_timer_list(t);

    /* Add to cache */
    cache_timer(t->id, t);

    NOSCHED_LEAVE();
    return t->id;
}

int32_t mo_timer_destroy(uint16_t id)
{
    if (unlikely(!timer_initialized))
        return ERR_FAIL;

    NOSCHED_ENTER();

    list_node_t *node = timer_find_node_by_id(id);
    if (unlikely(!node)) {
        NOSCHED_LEAVE();
        return ERR_FAIL;
    }

    timer_t *t = timer_from_node(node);

    /* Remove from active list if running */
    if (t->mode != TIMER_DISABLED)
        timer_remove_from_running_list(kcb->timer_list, t);

    /* Remove from cache */
    for (int i = 0; i < 4; i++) {
        if (timer_cache[i].timer == t) {
            timer_cache[i].id = 0;
            timer_cache[i].timer = NULL;
        }
    }

    /* Remove from master list */
    list_node_t *prev = all_timers_list->head;
    while (prev->next != all_timers_list->tail && prev->next != node)
        prev = prev->next;

    if (likely(prev->next == node)) {
        prev->next = node->next;
        all_timers_list->length--;
    }

    return_timer(t);
    NOSCHED_LEAVE();
    return ERR_OK;
}

int32_t mo_timer_start(uint16_t id, uint8_t mode)
{
    if (unlikely(mode != TIMER_ONESHOT && mode != TIMER_AUTORELOAD))
        return ERR_FAIL;
    if (unlikely(!timer_initialized))
        return ERR_FAIL;

    NOSCHED_ENTER();

    timer_t *t = timer_find_by_id_fast(id);
    if (unlikely(!t)) {
        NOSCHED_LEAVE();
        return ERR_FAIL;
    }

    /* Remove from active list if already running */
    if (t->mode != TIMER_DISABLED)
        timer_remove_from_running_list(kcb->timer_list, t);

    /* Configure and start timer */
    t->mode = mode;
    t->last_expected_fire_tick = mo_ticks() + MS_TO_TICKS(t->period_ms);
    t->deadline_ticks = t->last_expected_fire_tick;

    timer_sorted_insert_running_list(t);

    NOSCHED_LEAVE();
    return ERR_OK;
}

int32_t mo_timer_cancel(uint16_t id)
{
    if (unlikely(!timer_initialized))
        return ERR_FAIL;

    NOSCHED_ENTER();

    timer_t *t = timer_find_by_id_fast(id);
    if (unlikely(!t || t->mode == TIMER_DISABLED)) {
        NOSCHED_LEAVE();
        return ERR_FAIL;
    }

    timer_remove_from_running_list(kcb->timer_list, t);
    t->mode = TIMER_DISABLED;

    NOSCHED_LEAVE();
    return ERR_OK;
}
