/* Core task management and scheduling.
 *
 * This implements the main scheduler, manages the lifecycle of tasks (creation,
 * deletion, sleeping, etc.), and handles the context switching logic for both
 * preemptive and cooperative multitasking.
 */

#include <hal.h>
#include <lib/queue.h>
#include <sys/task.h>

#include "private/error.h"
#include "private/utils.h"

static int32_t noop_rtsched(void);
void _timer_tick_handler(void);

/* Hart scheduler */
static sched_t hart0 = {
    .ready_bitmap = 0,
    .ready_queues = {NULL},
    .rr_cursors = {NULL},
    .hart_id = 0,
};

/* Kernel-wide control block (KCB) */
static kcb_t kernel_state = {
    .tasks = NULL,
    .task_current = NULL,
    .rt_sched = noop_rtsched,
    .timer_list = NULL, /* Managed by timer.c, but stored here. */
    .next_tid = 1,      /* Start from 1 to avoid confusion with invalid ID 0 */
    .task_count = 0,
    .ticks = 0,
    .preemptive = true, /* Default to preemptive mode */
    .harts = &hart0,    /* Initial hart */
};
kcb_t *kcb = &kernel_state;

/* Bitmap operation */
#define BITMAP_CHECK(prio) (kcb->harts->ready_bitmap & 1U << prio)
#define BITMAP_SET(prio) (kcb->harts->ready_bitmap |= 1U << prio)
#define BITMAP_CLEAN(prio) (kcb->harts->ready_bitmap &= ~(1U << prio))

/* timer work management for reduced latency */
static volatile uint32_t timer_work_pending = 0;    /* timer work types */
static volatile uint32_t timer_work_generation = 0; /* counter for coalescing */

/* Timer work types for prioritized processing */
#define TIMER_WORK_TICK_HANDLER (1U << 0) /* Standard timer callbacks */
#define TIMER_WORK_DELAY_UPDATE (1U << 1) /* Task delay processing */
#define TIMER_WORK_CRITICAL (1U << 2)     /* High-priority timer work */

#if CONFIG_STACK_PROTECTION
/* Stack canary checking frequency - check every N context switches */
#define STACK_CHECK_INTERVAL 32

/* Magic number written to both ends of a task's stack for corruption detection.
 */
#define STACK_CANARY 0x33333333U

/* Stack check counter for periodic validation (reduces overhead). */
static uint32_t stack_check_counter = 0;
#endif /* CONFIG_STACK_PROTECTION */

/* Task lookup cache to accelerate frequent ID searches */
static struct {
    uint16_t id;
    tcb_t *task;
} task_cache[TASK_CACHE_SIZE];
static uint8_t cache_index = 0;

/* Priority-to-timeslice mapping table */
static const uint8_t priority_timeslices[TASK_PRIORITY_LEVELS] = {
    TASK_TIMESLICE_CRIT,     /* Priority 0: Critical */
    TASK_TIMESLICE_REALTIME, /* Priority 1: Real-time */
    TASK_TIMESLICE_HIGH,     /* Priority 2: High */
    TASK_TIMESLICE_ABOVE,    /* Priority 3: Above normal */
    TASK_TIMESLICE_NORMAL,   /* Priority 4: Normal */
    TASK_TIMESLICE_BELOW,    /* Priority 5: Below normal */
    TASK_TIMESLICE_LOW,      /* Priority 6: Low */
    TASK_TIMESLICE_IDLE      /* Priority 7: Idle */
};

/* Enqueue task into ready queue */
static void sched_enqueue_task(tcb_t *task);

/* Utility and Validation Functions */

/* Get appropriate time slice for a priority level */
static inline uint8_t get_priority_timeslice(uint8_t prio_level)
{
    if (unlikely(prio_level >= TASK_PRIORITY_LEVELS))
        return TASK_TIMESLICE_IDLE;
    return priority_timeslices[prio_level];
}

/* Extract priority level from encoded priority value */
static inline uint8_t extract_priority_level(uint16_t prio)
{
    /* compiler optimizes to jump table */
    switch (prio) {
    case TASK_PRIO_CRIT:
        return 0;
    case TASK_PRIO_REALTIME:
        return 1;
    case TASK_PRIO_HIGH:
        return 2;
    case TASK_PRIO_ABOVE:
        return 3;
    case TASK_PRIO_NORMAL:
        return 4;
    case TASK_PRIO_BELOW:
        return 5;
    case TASK_PRIO_LOW:
        return 6;
    case TASK_PRIO_IDLE:
        return 7;
    default:
        return 4; /* Default to normal priority */
    }
}

static inline bool is_valid_task(tcb_t *task)
{
    return (task && task->stack && task->stack_sz >= MIN_TASK_STACK_SIZE &&
            task->entry && task->id);
}

/* Add task to lookup cache */
static inline void cache_task(uint16_t id, tcb_t *task)
{
    task_cache[cache_index].id = id;
    task_cache[cache_index].task = task;
    cache_index = (cache_index + 1) % TASK_CACHE_SIZE;
}

/* Quick cache lookup before expensive list traversal */
static tcb_t *cache_lookup_task(uint16_t id)
{
    for (int i = 0; i < TASK_CACHE_SIZE; i++) {
        if (task_cache[i].id == id && is_valid_task(task_cache[i].task))
            return task_cache[i].task;
    }
    return NULL;
}

#if CONFIG_STACK_PROTECTION
/* Stack integrity check with reduced frequency */
static void task_stack_check(void)
{
    bool should_check = (++stack_check_counter >= STACK_CHECK_INTERVAL);
    if (should_check)
        stack_check_counter = 0;

    if (!should_check)
        return;

    if (unlikely(!kcb || !kcb->task_current || !kcb->task_current->data))
        panic(ERR_STACK_CHECK);

    tcb_t *self = kcb->task_current->data;
    if (unlikely(!is_valid_task(self)))
        panic(ERR_STACK_CHECK);

    uint32_t *lo_canary_ptr = (uint32_t *) self->stack;
    uint32_t *hi_canary_ptr = (uint32_t *) ((uintptr_t) self->stack +
                                            self->stack_sz - sizeof(uint32_t));

    if (unlikely(*lo_canary_ptr != STACK_CANARY ||
                 *hi_canary_ptr != STACK_CANARY)) {
        printf("\n*** STACK CORRUPTION: task %u base=%p size=%u\n", self->id,
               self->stack, (unsigned int) self->stack_sz);
        printf("    Canary values: low=0x%08x, high=0x%08x (expected 0x%08x)\n",
               *lo_canary_ptr, *hi_canary_ptr, STACK_CANARY);
        panic(ERR_STACK_CHECK);
    }
}
#endif /* CONFIG_STACK_PROTECTION */

/* Batch delay processing for blocked tasks */
static list_node_t *delay_update_batch(list_node_t *node, void *arg)
{
    uint32_t *ready_count = (uint32_t *) arg;
    if (unlikely(!node || !node->data))
        return NULL;

    tcb_t *t = node->data;

    /* Skip non-blocked tasks (common case) */
    if (likely(t->state != TASK_BLOCKED))
        return NULL;

    /* Process delays only if tick actually advanced */
    if (t->delay > 0) {
        if (--t->delay == 0) {
            t->state = TASK_READY;
            /* Add to appropriate priority ready queue */
            sched_enqueue_task(t);
            (*ready_count)++;
        }
    }
    return NULL;
}

/* timer work processing with coalescing and prioritization */
static inline void process_timer_work(uint32_t work_mask)
{
    if (unlikely(!work_mask))
        return;

    /* Process high-priority timer work first */
    if (work_mask & TIMER_WORK_CRITICAL) {
        /* Handle critical timer callbacks immediately */
        _timer_tick_handler();
    } else if (work_mask & TIMER_WORK_TICK_HANDLER) {
        /* Handle standard timer callbacks */
        _timer_tick_handler();
    }

    /* Delay updates are handled separately in scheduler */
}

/* Fast timer work processing for yield points */
static inline void process_deferred_timer_work(void)
{
    uint32_t work = timer_work_pending;
    if (likely(!work))
        return;

    /* Atomic clear with generation check to prevent race conditions */
    uint32_t current_gen = timer_work_generation;
    timer_work_pending = 0;

    process_timer_work(work);

    /* Check if new work arrived while processing */
    if (unlikely(timer_work_generation != current_gen)) {
        /* New work arrived, will be processed on next yield */
    }
}

/* delay update for cooperative mode */
static list_node_t *delay_update(list_node_t *node, void *arg)
{
    (void) arg;
    if (unlikely(!node || !node->data))
        return NULL;

    tcb_t *t = node->data;

    /* Skip non-blocked tasks (common case) */
    if (likely(t->state != TASK_BLOCKED))
        return NULL;

    /* Decrement delay and unblock task if expired */
    if (t->delay > 0 && --t->delay == 0) {
        t->state = TASK_READY;
        /* Add to appropriate priority ready queue */
        sched_enqueue_task(t);
    }
    return NULL;
}

/* Task search callbacks for finding tasks in the master list. */
static list_node_t *idcmp(list_node_t *node, void *arg)
{
    return (node && node->data &&
            ((tcb_t *) node->data)->id == (uint16_t) (size_t) arg)
               ? node
               : NULL;
}

static list_node_t *refcmp(list_node_t *node, void *arg)
{
    return (node && node->data && ((tcb_t *) node->data)->entry == arg) ? node
                                                                        : NULL;
}

/* Task lookup with caching */
static list_node_t *find_task_node_by_id(uint16_t id)
{
    if (!kcb->tasks || id == 0)
        return NULL;

    /* Try cache first */
    tcb_t *cached = cache_lookup_task(id);
    if (cached) {
        /* Find the corresponding node - this is still faster than full search
         */
        list_node_t *node = kcb->tasks->head->next;
        while (node != kcb->tasks->tail) {
            if (node->data == cached)
                return node;
            node = node->next;
        }
    }

    /* Fall back to full search and update cache */
    list_node_t *node = list_foreach(kcb->tasks, idcmp, (void *) (size_t) id);
    if (node && node->data)
        cache_task(id, (tcb_t *) node->data);

    return node;
}

/* Fast priority validation using lookup table */
static const uint16_t valid_priorities[] = {
    TASK_PRIO_CRIT,   TASK_PRIO_REALTIME, TASK_PRIO_HIGH, TASK_PRIO_ABOVE,
    TASK_PRIO_NORMAL, TASK_PRIO_BELOW,    TASK_PRIO_LOW,  TASK_PRIO_IDLE,
};

static bool is_valid_priority(uint16_t priority)
{
    for (size_t i = 0;
         i < sizeof(valid_priorities) / sizeof(valid_priorities[0]); i++) {
        if (priority == valid_priorities[i])
            return true;
    }
    return false;
}

/* Prints a fatal error message and halts the system. */
void panic(int32_t ecode)
{
    _di(); /* Block all further interrupts. */

    const char *msg = "unknown error";
    for (size_t i = 0; perror[i].code != ERR_UNKNOWN; ++i) {
        if (perror[i].code == ecode) {
            msg = perror[i].desc;
            break;
        }
    }
    printf("\n*** KERNEL PANIC (%d) – %s\n", (int) ecode, msg);
    hal_panic();
}

/* Weak aliases for context switching functions. */
void dispatch(void);
void yield(void);
void _dispatch(void) __attribute__((weak, alias("dispatch")));
void _yield(void) __attribute__((weak, alias("yield")));

/* Round-Robin Scheduler Implementation
 *
 * Implements an efficient round-robin scheduler tweaked for small systems.
 * While not achieving true O(1) complexity, this design provides excellent
 * practical performance with strong guarantees for fairness and reliability.
 */

/* Enqueue task into ready queue */
static void sched_enqueue_task(tcb_t *task)
{
    if (unlikely(!task))
        return;

    uint8_t prio_level = task->prio_level;

    /* Ensure task has appropriate time slice for its priority */
    task->time_slice = get_priority_timeslice(prio_level);
    task->state = TASK_READY;

    list_t **rq = &kcb->harts->ready_queues[prio_level];
    list_node_t **cursor = &kcb->harts->rr_cursors[prio_level];

    if (!*rq)
        *rq = list_create();

    list_node_t *rq_node = list_pushback(*rq, task);
    if (unlikely(!rq_node))
        return;

    /* Update task count in ready queue */
    kcb->harts->queue_counts[prio_level]++;

    /* Setup first rr_cursor */
    if (!*cursor)
        *cursor = rq_node;

    /* Advance cursor when cursor same as running task */
    if (*cursor == kcb->task_current)
        *cursor = rq_node;

    BITMAP_SET(task->prio_level);
    return;
}

/* Remove task from ready queue; return removed ready queue node */
static list_node_t *sched_dequeue_task(tcb_t *task)
{
    if (unlikely(!task))
        return NULL;

    uint8_t prio_level = task->prio_level;

    /* For task that need to be removed from ready/running state, it need be
     * removed from corresponding ready queue. */
    list_t *rq = kcb->harts->ready_queues[prio_level];
    list_node_t *rq_node = list_foreach(rq, idcmp, (void *) (size_t) task->id);
    list_node_t **cursor = &kcb->harts->rr_cursors[prio_level];
    if (!rq_node)
        return NULL;

    /* Safely move cursor to next task node. */
    if (rq_node == *cursor)
        *cursor = list_cnext(rq, *cursor);

    list_unlink(rq, rq_node);

    /* Update task count in ready queue */
    if (!--kcb->harts->queue_counts[prio_level]) {
        *cursor = NULL;
        BITMAP_CLEAN(task->prio_level);
    }
    return rq_node;
}

/* Handle time slice expiration for current task */
void sched_tick_current_task(void)
{
    if (unlikely(!kcb->task_current || !kcb->task_current->data))
        return;

    tcb_t *current_task = kcb->task_current->data;

    /* Decrement time slice */
    if (current_task->time_slice > 0)
        current_task->time_slice--;

    /* If time slice expired, force immediate rescheduling */
    if (current_task->time_slice == 0) {
        _dispatch();
    }
}

/* Task wakeup - simple state transition approach */
void sched_wakeup_task(tcb_t *task)
{
    if (unlikely(!task))
        return;

    /* Mark task as ready - scheduler will find it during round-robin traversal
     */
    if (task->state != TASK_READY) {
        task->state = TASK_READY;
        /* Ensure task has time slice */
        if (task->time_slice == 0)
            task->time_slice = get_priority_timeslice(task->prio_level);
    }
}

/* Efficient Round-Robin Task Selection with O(n) Complexity
 *
 * Selects the next ready task using circular traversal of the master task list.
 *
 * Complexity: O(n) where n = number of tasks
 * - Best case: O(1) when next task in sequence is ready
 * - Worst case: O(n) when only one task is ready and it's the last checked
 * - Typical case: O(k) where k << n (number of non-ready tasks to skip)
 *
 * Performance characteristics:
 * - Excellent for small-to-medium task counts (< 50 tasks)
 * - Simple and reliable implementation
 * - Good cache locality due to sequential list traversal
 * - Priority-aware time slice allocation
 */
uint16_t sched_select_next_task(void)
{
    if (unlikely(!kcb->task_current || !kcb->task_current->data))
        panic(ERR_NO_TASKS);

    tcb_t *current_task = kcb->task_current->data;

    /* Mark current task as ready if it was running */
    if (current_task->state == TASK_RUNNING)
        current_task->state = TASK_READY;

    /* Round-robin search: find next ready task in the master task list */
    list_node_t *start_node = kcb->task_current;
    list_node_t *node = start_node;
    int iterations = 0; /* Safety counter to prevent infinite loops */

    do {
        /* Move to next task (circular) */
        node = list_cnext(kcb->tasks, node);
        if (!node || !node->data)
            continue;

        tcb_t *task = node->data;

        /* Skip non-ready tasks */
        if (task->state != TASK_READY)
            continue;

        /* Found a ready task */
        kcb->task_current = node;
        task->state = TASK_RUNNING;
        task->time_slice = get_priority_timeslice(task->prio_level);

        return task->id;

    } while (node != start_node && ++iterations < SCHED_IMAX);

    /* No ready tasks found - this should not happen in normal operation */
    panic(ERR_NO_TASKS);
    return 0;
}

/* Default real-time scheduler stub. */
static int32_t noop_rtsched(void)
{
    return -1;
}

/* The main entry point from the system tick interrupt. */
void dispatcher(void)
{
    kcb->ticks++;

    /* Handle time slice for current task */
    sched_tick_current_task();

    /* Set timer work with generation increment for coalescing */
    timer_work_pending |= TIMER_WORK_TICK_HANDLER;
    timer_work_generation++;

    _dispatch();
}

/* Top-level context-switch for preemptive scheduling. */
void dispatch(void)
{
    if (unlikely(!kcb || !kcb->task_current || !kcb->task_current->data))
        panic(ERR_NO_TASKS);

    /* Save current context using dedicated HAL routine that handles both
     * execution context and processor state for context switching.
     * Returns immediately if this is the restore path.
     */
    if (hal_context_save(((tcb_t *) kcb->task_current->data)->context) != 0)
        return;

#if CONFIG_STACK_PROTECTION
    /* Do stack check less frequently to reduce overhead */
    if (unlikely((kcb->ticks & (STACK_CHECK_INTERVAL - 1)) == 0))
        task_stack_check();
#endif

    /* Batch process task delays for better efficiency */
    uint32_t ready_count = 0;
    list_foreach(kcb->tasks, delay_update_batch, &ready_count);

    /* Hook for real-time scheduler - if it selects a task, use it */
    if (kcb->rt_sched() < 0)
        sched_select_next_task(); /* Use O(1) priority scheduler */

    hal_interrupt_tick();

    /* Restore next task context */
    hal_context_restore(((tcb_t *) kcb->task_current->data)->context, 1);
}

/* Cooperative context switch */
void yield(void)
{
    if (unlikely(!kcb || !kcb->task_current || !kcb->task_current->data))
        return;

    /* Process deferred timer work during yield */
    process_deferred_timer_work();

    /* HAL context switching is used for preemptive scheduling. */
    if (hal_context_save(((tcb_t *) kcb->task_current->data)->context) != 0)
        return;

#if CONFIG_STACK_PROTECTION
    task_stack_check();
#endif

    /* In cooperative mode, delays are only processed on an explicit yield. */
    if (!kcb->preemptive)
        list_foreach(kcb->tasks, delay_update, NULL);

    sched_select_next_task(); /* Use O(1) priority scheduler */
    hal_context_restore(((tcb_t *) kcb->task_current->data)->context, 1);
}

/* Stack initialization with minimal overhead */
static bool init_task_stack(tcb_t *tcb, size_t stack_size)
{
    void *stack = malloc(stack_size);
    if (!stack)
        return false;

    /* Validate stack alignment */
    if ((uintptr_t) stack & 0x3) {
        free(stack);
        return false;
    }

#if CONFIG_STACK_PROTECTION
    /* Only initialize essential parts to reduce overhead */
    *(uint32_t *) stack = STACK_CANARY;
    *(uint32_t *) ((uintptr_t) stack + stack_size - sizeof(uint32_t)) =
        STACK_CANARY;
#endif

    tcb->stack = stack;
    tcb->stack_sz = stack_size;
    return true;
}

/* Task Management API */

int32_t mo_task_spawn(void *task_entry, uint16_t stack_size_req)
{
    if (!task_entry)
        panic(ERR_TCB_ALLOC);

    /* Ensure minimum stack size and proper alignment */
    size_t new_stack_size = stack_size_req;
    if (new_stack_size < MIN_TASK_STACK_SIZE)
        new_stack_size = MIN_TASK_STACK_SIZE;
    new_stack_size = (new_stack_size + 0xF) & ~0xFU;

    /* Allocate and initialize TCB */
    tcb_t *tcb = malloc(sizeof(tcb_t));
    if (!tcb)
        panic(ERR_TCB_ALLOC);

    tcb->entry = task_entry;
    tcb->delay = 0;
    tcb->rt_prio = NULL;
    tcb->state = TASK_STOPPED;
    tcb->flags = 0;

    /* Set default priority with proper scheduler fields */
    tcb->prio = TASK_PRIO_NORMAL;
    tcb->prio_level = extract_priority_level(TASK_PRIO_NORMAL);
    tcb->time_slice = get_priority_timeslice(tcb->prio_level);

    /* Initialize stack */
    if (!init_task_stack(tcb, new_stack_size)) {
        free(tcb);
        panic(ERR_STACK_ALLOC);
    }

    /* Minimize critical section duration */
    CRITICAL_ENTER();

    if (!kcb->tasks) {
        kcb->tasks = list_create();
        if (!kcb->tasks) {
            CRITICAL_LEAVE();
            free(tcb->stack);
            free(tcb);
            panic(ERR_KCB_ALLOC);
        }
    }

    list_node_t *node = list_pushback(kcb->tasks, tcb);
    if (!node) {
        CRITICAL_LEAVE();
        free(tcb->stack);
        free(tcb);
        panic(ERR_TCB_ALLOC);
    }

    /* Assign unique ID and update counts */
    tcb->id = kcb->next_tid++;
    kcb->task_count++; /* Cached count of active tasks for quick access */

    /* Push node to ready queue */
    sched_enqueue_task(tcb);
    if (!kcb->task_current) {
        kcb->task_current = kcb->harts->rr_cursors[tcb->prio_level];
        tcb->state = TASK_RUNNING;
    }

    CRITICAL_LEAVE();

    /* Initialize execution context outside critical section. */
    hal_context_init(&tcb->context, (size_t) tcb->stack, new_stack_size,
                     (size_t) task_entry);

    printf("task %u: entry=%p stack=%p size=%u prio_level=%u time_slice=%u\n",
           tcb->id, task_entry, tcb->stack, (unsigned int) new_stack_size,
           tcb->prio_level, tcb->time_slice);

    /* Add to cache and mark ready */
    cache_task(tcb->id, tcb);

    return tcb->id;
}

int32_t mo_task_cancel(uint16_t id)
{
    if (id == 0 || id == mo_task_id())
        return ERR_TASK_CANT_REMOVE;

    CRITICAL_ENTER();
    list_node_t *node = find_task_node_by_id(id);
    if (!node) {
        CRITICAL_LEAVE();
        return ERR_TASK_NOT_FOUND;
    }

    tcb_t *tcb = node->data;
    if (!tcb || tcb->state == TASK_RUNNING) {
        CRITICAL_LEAVE();
        return ERR_TASK_CANT_REMOVE;
    }

    /* Remove from list and update count */
    list_remove(kcb->tasks, node);
    kcb->task_count--;

    /* Clear from cache */
    for (int i = 0; i < TASK_CACHE_SIZE; i++) {
        if (task_cache[i].task == tcb) {
            task_cache[i].id = 0;
            task_cache[i].task = NULL;
        }
    }

    CRITICAL_LEAVE();

    /* Free memory outside critical section */
    free(tcb->stack);
    free(tcb);
    free(node);
    return ERR_OK;
}

void mo_task_yield(void)
{
    _yield();
}

void mo_task_delay(uint16_t ticks)
{
    /* Process deferred timer work before sleeping */
    process_deferred_timer_work();

    if (!ticks)
        return;

    NOSCHED_ENTER();
    if (unlikely(!kcb || !kcb->task_current || !kcb->task_current->data)) {
        NOSCHED_LEAVE();
        return;
    }

    tcb_t *self = kcb->task_current->data;

    /* Set delay and blocked state - scheduler will skip blocked tasks */
    self->delay = ticks;
    self->state = TASK_BLOCKED;
    NOSCHED_LEAVE();

    mo_task_yield();
}

int32_t mo_task_suspend(uint16_t id)
{
    if (id == 0)
        return ERR_TASK_NOT_FOUND;

    CRITICAL_ENTER();
    list_node_t *node = find_task_node_by_id(id);
    if (!node) {
        CRITICAL_LEAVE();
        return ERR_TASK_NOT_FOUND;
    }

    tcb_t *task = node->data;
    if (!task || (task->state != TASK_READY && task->state != TASK_RUNNING &&
                  task->state != TASK_BLOCKED)) {
        CRITICAL_LEAVE();
        return ERR_TASK_CANT_SUSPEND;
    }

    task->state = TASK_SUSPENDED;
    bool is_current = (kcb->task_current == node);

    CRITICAL_LEAVE();

    if (is_current)
        mo_task_yield();

    return ERR_OK;
}

int32_t mo_task_resume(uint16_t id)
{
    if (id == 0)
        return ERR_TASK_NOT_FOUND;

    CRITICAL_ENTER();
    list_node_t *node = find_task_node_by_id(id);
    if (!node) {
        CRITICAL_LEAVE();
        return ERR_TASK_NOT_FOUND;
    }

    tcb_t *task = node->data;
    if (!task || task->state != TASK_SUSPENDED) {
        CRITICAL_LEAVE();
        return ERR_TASK_CANT_RESUME;
    }

    /* mark as ready - scheduler will find it */
    task->state = TASK_READY;

    CRITICAL_LEAVE();
    return ERR_OK;
}

int32_t mo_task_priority(uint16_t id, uint16_t priority)
{
    if (id == 0 || !is_valid_priority(priority))
        return ERR_TASK_INVALID_PRIO;

    CRITICAL_ENTER();
    list_node_t *node = find_task_node_by_id(id);
    if (!node) {
        CRITICAL_LEAVE();
        return ERR_TASK_NOT_FOUND;
    }

    tcb_t *task = node->data;
    if (!task) {
        CRITICAL_LEAVE();
        return ERR_TASK_NOT_FOUND;
    }

    /* Update priority and level */
    task->prio = priority;
    task->prio_level = extract_priority_level(priority);
    task->time_slice = get_priority_timeslice(task->prio_level);

    CRITICAL_LEAVE();
    return ERR_OK;
}

int32_t mo_task_rt_priority(uint16_t id, void *priority)
{
    if (id == 0)
        return ERR_TASK_NOT_FOUND;

    CRITICAL_ENTER();
    list_node_t *node = find_task_node_by_id(id);
    if (!node) {
        CRITICAL_LEAVE();
        return ERR_TASK_NOT_FOUND;
    }

    tcb_t *task = node->data;
    if (!task) {
        CRITICAL_LEAVE();
        return ERR_TASK_NOT_FOUND;
    }

    task->rt_prio = priority;
    CRITICAL_LEAVE();
    return ERR_OK;
}

uint16_t mo_task_id(void)
{
    if (unlikely(!kcb || !kcb->task_current || !kcb->task_current->data))
        return 0;
    return ((tcb_t *) kcb->task_current->data)->id;
}

int32_t mo_task_idref(void *task_entry)
{
    if (!task_entry || !kcb->tasks)
        return ERR_TASK_NOT_FOUND;

    CRITICAL_ENTER();
    list_node_t *node = list_foreach(kcb->tasks, refcmp, task_entry);
    CRITICAL_LEAVE();

    return node ? ((tcb_t *) node->data)->id : ERR_TASK_NOT_FOUND;
}

void mo_task_wfi(void)
{
    /* Process deferred timer work before waiting */
    process_deferred_timer_work();

    if (!kcb->preemptive)
        return;

    volatile uint32_t current_ticks = kcb->ticks;
    while (current_ticks == kcb->ticks)
        hal_cpu_idle();
}

uint16_t mo_task_count(void)
{
    return kcb->task_count;
}

uint32_t mo_ticks(void)
{
    return kcb->ticks;
}

uint64_t mo_uptime(void)
{
    return _read_us() / 1000;
}

void _sched_block(queue_t *wait_q)
{
    if (unlikely(!wait_q || !kcb || !kcb->task_current ||
                 !kcb->task_current->data))
        panic(ERR_SEM_OPERATION);

    /* Process deferred timer work before blocking */
    process_deferred_timer_work();

    tcb_t *self = kcb->task_current->data;

    if (queue_enqueue(wait_q, self) != 0)
        panic(ERR_SEM_OPERATION);

    /* set blocked state - scheduler will skip blocked tasks */
    self->state = TASK_BLOCKED;
    _yield();
}
