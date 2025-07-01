/* Core task management and scheduling.
 *
 * This implements the main scheduler, manages the lifecycle of tasks (creation,
 * deletion, sleeping, etc.), and handles the context switching logic for both
 * preemptive and cooperative multitasking.
 */

#include <hal.h>
#include <spinlock.h>
#include <lib/queue.h>
#include <sys/task.h>

#include "private/error.h"
#include "private/utils.h"

static int32_t noop_rtsched(void);
void _timer_tick_handler(void);

/* Kernel-wide control block (KCB)
 * Lists are intentionally initialized to NULL and will be created lazily on
 * their first use, preventing null-pointer access errors and simplifying the
 * boot sequence.
 */
static kcb_t kernel_state = {
    .tasks = NULL,
    .task_current = {},
    .rt_sched = noop_rtsched,
    .timer_list = NULL, /* Managed by timer.c, but stored here. */
    .next_tid = 1,      /* Start from 1 to avoid confusion with invalid ID 0 */
    .task_count = 0,
    .ticks = 0,
    .preemptive = true, /* Default to preemptive mode */
    .last_ready_hint = NULL,
    .kcb_lock = SPINLOCK_INITIALIZER,
};
kcb_t *kcb = &kernel_state;

/* Magic number written to both ends of a task's stack for corruption detection.
 */
#define STACK_CANARY 0x33333333U

/* Stack check counter for periodic validation (reduces overhead). */
static uint32_t stack_check_counter = 0;

/* Simple task lookup cache to accelerate frequent ID searches */
static struct {
    uint16_t id;
    tcb_t *task;
} task_cache[TASK_CACHE_SIZE];
static uint8_t cache_index = 0;

static uint32_t task_flags = 0;

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

/* Stack integrity check with reduced frequency */
static void task_stack_check(void)
{
    bool should_check = (++stack_check_counter >= STACK_CHECK_INTERVAL);
    if (should_check)
        stack_check_counter = 0;

    if (!should_check)
        return;

    if (unlikely(!kcb || !get_task_current() || !get_task_current()->data))
        panic(ERR_STACK_CHECK);

    tcb_t *self = get_task_current()->data;
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

/* Delay update with early termination for active tasks */
static list_node_t *delay_update(list_node_t *node, void *arg)
{
    (void) arg;
    if (unlikely(!node || !node->data))
        return NULL;

    tcb_t *t = node->data;
    /* Only process blocked tasks with active delays */
    if (t->state == TASK_BLOCKED && t->delay > 0) {
        if (--t->delay == 0)
            t->state = TASK_READY;
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

/* Scheduler with hint-based ready task search */
static list_node_t *find_next_ready_task(void)
{
    if (unlikely(!get_task_current()))
        return NULL;

    list_node_t *node;
    int itcnt = 0;

    /* Start from hint if available, otherwise start from current */
    if (kcb->last_ready_hint && kcb->last_ready_hint->data) {
        tcb_t *hint_task = kcb->last_ready_hint->data;
        if (hint_task->state == TASK_READY && !hint_task->rt_prio) {
            uint8_t counter = hint_task->prio & 0xFF;
            if (counter == 0) {
                /* Hint task is immediately ready */
                counter = (hint_task->prio >> 8) & 0xFF;
                hint_task->prio = (hint_task->prio & 0xFF00) | counter;
                return kcb->last_ready_hint;
            }
        }
    }

    node = get_task_current();
    while (itcnt++ < SCHED_IMAX) {
        node = list_cnext(kcb->tasks, node);
        if (unlikely(!node || !node->data))
            break;

        tcb_t *task = node->data;
        if (task->state != TASK_READY || task->rt_prio)
            continue;

        /* Decrement the dynamic priority counter. */
        uint8_t counter = task->prio & 0xFF;
        if (counter)
            counter--;
        task->prio = (task->prio & 0xFF00) | counter;

        /* If the counter reaches zero, this task is selected to run. */
        if (counter == 0) {
            /* Reload counter from its base priority for the next cycle. */
            counter = (task->prio >> 8) & 0xFF;
            task->prio = (task->prio & 0xFF00) | counter;
            kcb->last_ready_hint = node; /* Update hint for next time */
            return node;
        }
    }

    kcb->last_ready_hint = NULL; /* Clear invalid hint */
    return NULL;
}

/* Scheduler with reduced overhead */
static uint16_t schedule_next_task(void)
{
    if (unlikely(!get_task_current() || !get_task_current()->data))
        panic(ERR_NO_TASKS);

    /* Mark the previously running task as ready for the next cycle. */
    tcb_t *current_task = get_task_current()->data;
    if (current_task->state == TASK_RUNNING)
        current_task->state = TASK_READY;

    /* Try to find the next ready task */
    list_node_t *next_node = find_next_ready_task();
    if (unlikely(!next_node)) {
        panic(ERR_NO_TASKS);
    }

    /* Update scheduler state */
    set_task_current(next_node);
    tcb_t *next_task = next_node->data;
    next_task->state = TASK_RUNNING;

    return next_task->id;
}

/* Default real-time scheduler stub. */
static int32_t noop_rtsched(void)
{
    return -1;
}

/* The main entry point from the system tick interrupt. */
void dispatcher(void)
{
    uint32_t flag = 0;

    spin_lock_irqsave(&kcb->kcb_lock, &flag);
    kcb->ticks++;
    spin_unlock_irqrestore(&kcb->kcb_lock, flag);
    _timer_tick_handler();
    _dispatch();
}

/* Top-level context-switch for preemptive scheduling. */
void dispatch(void)
{
    if (unlikely(!kcb || !get_task_current() || !get_task_current()->data))
        panic(ERR_NO_TASKS);

    /* Return from longjmp: context is restored, continue task execution. */
    if (setjmp(((tcb_t *) get_task_current()->data)->context) != 0)
        return;

    task_stack_check();
    list_foreach(kcb->tasks, delay_update, NULL);

    /* Hook for real-time scheduler - if it selects a task, use it */
    if (kcb->rt_sched() < 0) {
        schedule_next_task();
    }

    hal_interrupt_tick();
    longjmp(((tcb_t *) get_task_current()->data)->context, 1);
}

/* Cooperative context switch */
void yield(void)
{
    if (unlikely(!kcb || !get_task_current() || !get_task_current()->data))
        return;

    if (setjmp(((tcb_t *) get_task_current()->data)->context) != 0)
        return;

    task_stack_check();

    /* In cooperative mode, delays are only processed on an explicit yield. */
    if (!kcb->preemptive)
        list_foreach(kcb->tasks, delay_update, NULL);

    schedule_next_task();
    longjmp(((tcb_t *) get_task_current()->data)->context, 1);
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

    /* Only initialize essential parts to reduce overhead */
    *(uint32_t *) stack = STACK_CANARY;
    *(uint32_t *) ((uintptr_t) stack + stack_size - sizeof(uint32_t)) =
        STACK_CANARY;

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

    /* Set default priority with counter = 0 for immediate eligibility */
    uint8_t base = (uint8_t) (TASK_PRIO_NORMAL >> 8);
    tcb->prio = ((uint16_t) base << 8);

    /* Initialize stack */
    if (!init_task_stack(tcb, new_stack_size)) {
        free(tcb);
        panic(ERR_STACK_ALLOC);
    }

    /* Minimize critical section duration */
    spin_lock_irqsave(&kcb->kcb_lock, &task_flags);

    if (!kcb->tasks) {
        kcb->tasks = list_create();
        if (!kcb->tasks) {
            spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
            free(tcb->stack);
            free(tcb);
            panic(ERR_KCB_ALLOC);
        }
    }

    list_node_t *node = list_pushback(kcb->tasks, tcb);
    if (!node) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
        free(tcb->stack);
        free(tcb);
        panic(ERR_TCB_ALLOC);
    }

    /* Assign unique ID and update counts */
    tcb->id = kcb->next_tid++;
    kcb->task_count++;

    if (!get_task_current())
        set_task_current(node);

    spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);

    /* Initialize execution context outside critical section */
    hal_context_init(&tcb->context, (size_t) tcb->stack, new_stack_size,
                     (size_t) task_entry);

    printf("task %u: entry=%p stack=%p size=%u\n", tcb->id, task_entry,
           tcb->stack, (unsigned int) new_stack_size);

    /* Add to cache and mark ready */
    cache_task(tcb->id, tcb);
    tcb->state = TASK_READY;
    return tcb->id;
}

int32_t mo_task_cancel(uint16_t id)
{
    if (id == 0 || id == mo_task_id())
        return ERR_TASK_CANT_REMOVE;

    spin_lock_irqsave(&kcb->kcb_lock, &task_flags);
    list_node_t *node = find_task_node_by_id(id);
    if (!node) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
        return ERR_TASK_NOT_FOUND;
    }

    tcb_t *tcb = node->data;
    if (!tcb || tcb->state == TASK_RUNNING) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
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

    /* Clear ready hint if it points to this task */
    if (kcb->last_ready_hint == node)
        kcb->last_ready_hint = NULL;

    spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);

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
    if (!ticks)
        return;

    spin_lock_irqsave(&kcb->kcb_lock, &task_flags);
    if (unlikely(!kcb || !get_task_current() || !get_task_current()->data)) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
        return;
    }

    tcb_t *self = get_task_current()->data;
    self->delay = ticks;
    self->state = TASK_BLOCKED;
    spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);

    mo_task_yield();
}

int32_t mo_task_suspend(uint16_t id)
{
    if (id == 0)
        return ERR_TASK_NOT_FOUND;

    spin_lock_irqsave(&kcb->kcb_lock, &task_flags);
    list_node_t *node = find_task_node_by_id(id);
    if (!node) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
        return ERR_TASK_NOT_FOUND;
    }

    tcb_t *task = node->data;
    if (!task || (task->state != TASK_READY && task->state != TASK_RUNNING &&
                  task->state != TASK_BLOCKED)) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
        return ERR_TASK_CANT_SUSPEND;
    }

    task->state = TASK_SUSPENDED;
    bool is_current = (get_task_current() == node);

    /* Clear ready hint if suspending that task */
    if (kcb->last_ready_hint == node)
        kcb->last_ready_hint = NULL;

    spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);

    if (is_current)
        mo_task_yield();

    return ERR_OK;
}

int32_t mo_task_resume(uint16_t id)
{
    if (id == 0)
        return ERR_TASK_NOT_FOUND;

    spin_lock_irqsave(&kcb->kcb_lock, &task_flags);
    list_node_t *node = find_task_node_by_id(id);
    if (!node) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
        return ERR_TASK_NOT_FOUND;
    }

    tcb_t *task = node->data;
    if (!task || task->state != TASK_SUSPENDED) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
        return ERR_TASK_CANT_RESUME;
    }

    task->state = TASK_READY;
    spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
    return ERR_OK;
}

int32_t mo_task_priority(uint16_t id, uint16_t priority)
{
    if (id == 0 || !is_valid_priority(priority))
        return ERR_TASK_INVALID_PRIO;

    spin_lock_irqsave(&kcb->kcb_lock, &task_flags);
    list_node_t *node = find_task_node_by_id(id);
    if (!node) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
        return ERR_TASK_NOT_FOUND;
    }

    tcb_t *task = node->data;
    if (!task) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
        return ERR_TASK_NOT_FOUND;
    }

    uint8_t base = (uint8_t) (priority >> 8);
    task->prio = ((uint16_t) base << 8) | base;
    spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);

    return ERR_OK;
}

int32_t mo_task_rt_priority(uint16_t id, void *priority)
{
    if (id == 0)
        return ERR_TASK_NOT_FOUND;

    spin_lock_irqsave(&kcb->kcb_lock, &task_flags);
    list_node_t *node = find_task_node_by_id(id);
    if (!node) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
        return ERR_TASK_NOT_FOUND;
    }

    tcb_t *task = node->data;
    if (!task) {
        spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
        return ERR_TASK_NOT_FOUND;
    }

    task->rt_prio = priority;
    spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);
    return ERR_OK;
}

uint16_t mo_task_id(void)
{
    if (unlikely(!kcb || !get_task_current() || !get_task_current()->data))
        return 0;
    return ((tcb_t *) get_task_current()->data)->id;
}

int32_t mo_task_idref(void *task_entry)
{
    if (!task_entry || !kcb->tasks)
        return ERR_TASK_NOT_FOUND;

    spin_lock_irqsave(&kcb->kcb_lock, &task_flags);
    list_node_t *node = list_foreach(kcb->tasks, refcmp, task_entry);
    spin_unlock_irqrestore(&kcb->kcb_lock, task_flags);

    return node ? ((tcb_t *) node->data)->id : ERR_TASK_NOT_FOUND;
}

void mo_task_wfi(void)
{
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
    if (unlikely(!wait_q || !kcb || !get_task_current() ||
                 !get_task_current()->data))
        panic(ERR_SEM_OPERATION);

    tcb_t *self = get_task_current()->data;

    if (queue_enqueue(wait_q, self) != 0) {
        panic(ERR_SEM_OPERATION);
    }

    self->state = TASK_BLOCKED;
    _yield();
}
