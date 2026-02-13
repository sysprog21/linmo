/* Core task management and scheduling.
 *
 * This implements the main scheduler, manages the lifecycle of tasks (creation,
 * deletion, sleeping, etc.), and handles the context switching logic for both
 * preemptive and cooperative multitasking.
 */

#include <hal.h>
#include <lib/libc.h>
#include <lib/queue.h>
#include <pmp.h>
#include <sys/task.h>

#include "private/error.h"
#include "private/utils.h"

static int32_t noop_rtsched(void);
void _timer_tick_handler(void);

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
};
kcb_t *kcb = &kernel_state;

/* Flag to track if scheduler has started - prevents timer IRQ during early
 * init. NOSCHED_LEAVE checks this to avoid enabling timer before scheduler is
 * ready.
 *
 * FIXME(SMP): Global flag assumes single-core. For SMP, use per-CPU scheduler
 * state or atomic operations with memory barriers.
 */
volatile bool scheduler_started = false;

/* Critical section nesting support for ISR-safe CRITICAL_ENTER/LEAVE.
 * Tracks nesting depth and saves the original interrupt state so that
 * CRITICAL_LEAVE restores rather than unconditionally enables interrupts.
 *
 * FIXME(SMP): These variables are global, assuming single-core execution.
 * For SMP support, move to per-CPU storage (e.g., CPU-local structure indexed
 * by hart ID) or use atomic operations with spinlocks. Current assumptions:
 * - Only one execution context runs at a time (task or ISR)
 * - ISRs preempt tasks but ISRs do not nest (MIE stays cleared throughout
 *   ISR execution per RISC-V trap entry behavior)
 * - These globals are only used in preemptive mode; in cooperative mode
 *   the CRITICAL macros are no-ops since tasks yield voluntarily
 */
volatile uint32_t critical_nesting = 0;
volatile int32_t critical_saved_mie = 0;

/* Timer work management for reduced latency.
 *
 * FIXME(SMP): Global timer work state assumes single-core. For SMP, use per-CPU
 * work queues or atomic operations to avoid cross-CPU races.
 */
static volatile uint32_t timer_work_pending = 0;    /* timer work types */
static volatile uint32_t timer_work_generation = 0; /* counter for coalescing */

/* Timer work types for prioritized processing */
#define TIMER_WORK_TICK_HANDLER (1U << 0) /* Standard timer callbacks */
#define TIMER_WORK_DELAY_UPDATE (1U << 1) /* Task delay processing */
#define TIMER_WORK_CRITICAL (1U << 2)     /* High-priority timer work */

/* Kernel stack size for U-mode tasks */
#define KERNEL_STACK_SIZE 1024 /* 1024 bytes per U-mode task */

#if CONFIG_STACK_PROTECTION
/* Stack canary checking frequency - check every N context switches */
#define STACK_CHECK_INTERVAL 32

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

/* Task mode to display character mapping (extensible for future modes) */
static const char task_mode_chars[] = {
    [TASK_MODE_M] = 'M', /* Machine mode */
    [TASK_MODE_U] = 'U', /* User mode */
};

/* Mark task as ready (state-based) */
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

    if (unlikely(*lo_canary_ptr != self->canary ||
                 *hi_canary_ptr != self->canary)) {
        printf("\n*** STACK CORRUPTION: task %u base=%p size=%u\n", self->id,
               self->stack, (unsigned int) self->stack_sz);
        printf("    Canary values: low=0x%08x, high=0x%08x (expected 0x%08x)\n",
               *lo_canary_ptr, *hi_canary_ptr, self->canary);
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

            /* If this is an RT task, set its deadline for the next job.
             * For periodic tasks, deadline should be current_time + period.
             * This ensures tasks are scheduled based on their actual deadlines,
             * not inflated values from previous scheduler calls.
             */
            if (t->rt_prio) {
                typedef struct {
                    uint32_t period;
                    uint32_t deadline;
                } edf_prio_t;
                edf_prio_t *edf = (edf_prio_t *) t->rt_prio;
                extern kcb_t *kcb;
                edf->deadline = kcb->ticks + edf->period;
            }

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

/* Zombie Task Cleanup
 *
 * Scans the task list for terminated (zombie) tasks and frees their resources.
 * Called from dispatcher to ensure cleanup happens in a safe context.
 */
static void task_cleanup_zombies(void)
{
    if (!kcb || !kcb->tasks)
        return;

    list_node_t *node = list_next(kcb->tasks->head);
    while (node && node != kcb->tasks->tail) {
        list_node_t *next = list_next(node);
        tcb_t *tcb = node->data;

        if (tcb && tcb->state == TASK_ZOMBIE && node != kcb->task_current) {
            /* Remove from task list */
            list_remove(kcb->tasks, node);
            kcb->task_count--;

            /* Clear from lookup cache */
            for (int i = 0; i < TASK_CACHE_SIZE; i++) {
                if (task_cache[i].task == tcb) {
                    task_cache[i].id = 0;
                    task_cache[i].task = NULL;
                }
            }

            /* Free all resources */
            if (tcb->mspace)
                mo_memspace_destroy(tcb->mspace);
            free(tcb->stack);
            if (tcb->kernel_stack)
                free(tcb->kernel_stack);
            free(tcb);
        }
        node = next;
    }
}

/* Round-Robin Scheduler Implementation
 *
 * Implements an efficient round-robin scheduler tweaked for small systems.
 * While not achieving true O(1) complexity, this design provides excellent
 * practical performance with strong guarantees for fairness and reliability.
 */

/* Add task to ready state - simple state-based approach */
static void sched_enqueue_task(tcb_t *task)
{
    if (unlikely(!task))
        return;

    /* Ensure task has appropriate time slice for its priority */
    task->time_slice = get_priority_timeslice(task->prio_level);
    task->state = TASK_READY;

    /* Task selection is handled directly through the master task list */
}

/* Remove task from ready queues - state-based approach for compatibility */
void sched_dequeue_task(tcb_t *task)
{
    if (unlikely(!task))
        return;

    /* For tasks that need to be removed from ready state (suspended/cancelled),
     * we rely on the state change. The scheduler will skip non-ready tasks
     * when it encounters them during the round-robin traversal.
     */
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

    /* If time slice expired, mark task as ready for rescheduling.
     * Don't call _dispatch() here - let the normal dispatcher() flow handle it.
     * Calling _dispatch() from within dispatcher() causes double-dispatch bug.
     */
    if (current_task->time_slice == 0) {
        if (current_task->state == TASK_RUNNING)
            current_task->state = TASK_READY;
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

    /* No ready tasks found in preemptive mode - all tasks are blocked.
     * This is normal for periodic RT tasks waiting for their next period.
     * We CANNOT return a BLOCKED task as that would cause it to run.
     * Instead, find ANY task (even blocked) as a placeholder, then wait for
     * interrupt.
     */
    if (kcb->preemptive) {
        /* Select any task as placeholder (dispatcher won't actually switch to
         * it if blocked) */
        list_node_t *any_node = list_next(kcb->tasks->head);
        while (any_node && any_node != kcb->tasks->tail) {
            if (any_node->data) {
                kcb->task_current = any_node;
                tcb_t *any_task = any_node->data;
                return any_task->id;
            }
            any_node = list_next(any_node);
        }
        /* No tasks at all - this is a real error */
        panic(ERR_NO_TASKS);
    }

    /* In cooperative mode, having no ready tasks is an error */
    panic(ERR_NO_TASKS);
    return 0;
}

/* Default real-time scheduler stub. */
static int32_t noop_rtsched(void)
{
    return -1;
}

/* The main entry point from interrupts (timer or ecall).
 * Parameter: from_timer = 1 if called from timer ISR (increment ticks),
 *                       = 0 if called from ecall (don't increment ticks)
 */
void dispatcher(int from_timer)
{
    if (from_timer)
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

    /* Clean up any terminated (zombie) tasks */
    task_cleanup_zombies();

    /* Save current context - only needed for cooperative mode.
     * In preemptive mode, ISR already saved context to stack,
     * so we skip this step to avoid interference.
     */
    if (!kcb->preemptive) {
        /* Cooperative mode: use setjmp/longjmp mechanism */
        if (hal_context_save(((tcb_t *) kcb->task_current->data)->context) != 0)
            return;
    }

#if CONFIG_STACK_PROTECTION
    /* Do stack check less frequently to reduce overhead */
    if (unlikely((kcb->ticks & (STACK_CHECK_INTERVAL - 1)) == 0))
        task_stack_check();
#endif

    /* Batch process task delays for better efficiency.
     * Only process delays if tick has advanced to avoid decrementing multiple
     * times per tick when dispatch() is called multiple times.
     */
    uint32_t ready_count = 0;
    static uint32_t last_delay_update_tick = 0;
    if (kcb->ticks != last_delay_update_tick) {
        list_foreach(kcb->tasks, delay_update_batch, &ready_count);
        last_delay_update_tick = kcb->ticks;
    }

    /* Hook for real-time scheduler - if it selects a task, use it */
    tcb_t *prev_task = kcb->task_current->data;
    int32_t rt_task_id = kcb->rt_sched();

    if (rt_task_id < 0) {
        sched_select_next_task(); /* Use O(n) round-robin scheduler */
    } else {
        /* RT scheduler selected a task - update current task pointer */
        list_node_t *rt_node = find_task_node_by_id((uint16_t) rt_task_id);
        if (rt_node && rt_node->data) {
            tcb_t *rt_task = rt_node->data;
            /* Different task - perform context switch */
            if (rt_node != kcb->task_current) {
                if (kcb->task_current && kcb->task_current->data) {
                    tcb_t *prev = kcb->task_current->data;
                    if (prev->state == TASK_RUNNING)
                        prev->state = TASK_READY;
                }
                /* Switch to RT task */
                kcb->task_current = rt_node;
                rt_task->state = TASK_RUNNING;
                rt_task->time_slice =
                    get_priority_timeslice(rt_task->prio_level);
            }
            /* If same task selected, fall through to do_context_switch
             * which will check if task is blocked and handle appropriately */
        } else {
            /* RT task not found, fall back to round-robin */
            sched_select_next_task();
        }
    }

    /* Check if we're still on the same task (no actual switch needed) */
    tcb_t *next_task = kcb->task_current->data;

    /* In preemptive mode, if selected task has pending delay, keep trying to
     * find ready task. We check delay > 0 instead of state == BLOCKED because
     * schedulers already modified state to RUNNING.
     */
    if (kcb->preemptive) {
        int attempts = 0;
        while (next_task->delay > 0 && attempts < 10) {
            /* Try next task in round-robin */
            kcb->task_current = list_cnext(kcb->tasks, kcb->task_current);
            if (!kcb->task_current || !kcb->task_current->data)
                kcb->task_current = list_next(kcb->tasks->head);
            next_task = kcb->task_current->data;
            attempts++;
        }

        /* If still has delay after all attempts, all tasks are blocked.
         * Just select this task anyway - it will resume and immediately yield
         * again, creating a busy-wait ecall loop until timer interrupt fires
         * and decrements delays.
         */
    }

    /* Update task state and time slice before context switch */
    if (next_task->state != TASK_RUNNING)
        next_task->state = TASK_RUNNING;
    next_task->time_slice = get_priority_timeslice(next_task->prio_level);

    /* Switch PMP configuration if tasks have different memory spaces */
    pmp_switch_context(prev_task->mspace, next_task->mspace);

    /* Perform context switch based on scheduling mode */
    if (kcb->preemptive) {
        /* Same task - no context switch needed */
        if (next_task == prev_task)
            return; /* ISR will restore from current stack naturally */

        /* Preemptive mode: Switch stack pointer.
         * ISR already saved context to prev_task's stack.
         * Switch SP to next_task's stack.
         * When we return, ISR will restore from next_task's stack.
         */
        hal_switch_stack(&prev_task->sp, next_task->sp);

        /* Update kernel stack for next trap entry */
        hal_set_kernel_stack(next_task->kernel_stack,
                             next_task->kernel_stack_size);
    } else {
        /* Cooperative mode: Always call hal_context_restore() because it uses
         * setjmp/longjmp mechanism. Even if same task continues, we must
         * longjmp back to complete the context save/restore cycle.
         */
        hal_interrupt_tick();
        hal_context_restore(next_task->context, 1);
    }
}

/* Cooperative context switch */
void yield(void)
{
    if (unlikely(!kcb || !kcb->task_current || !kcb->task_current->data))
        return;

    /* Process deferred timer work during yield */
    process_deferred_timer_work();

    /* In preemptive mode, can't use setjmp/longjmp - incompatible with ISR
     * stack frames. Trigger dispatcher via ecall, then wait until task becomes
     * READY again.
     */
    if (kcb->preemptive) {
        /* Avoid triggering nested traps when already in trap context.
         * The dispatcher can be invoked directly since the trap handler
         * environment is already established.
         */
        if (trap_nesting_depth > 0) {
            dispatcher(0);
        } else {
            __asm__ volatile("ecall");
        }

        return;
    }

    /* Cooperative mode: use setjmp/longjmp mechanism */
    if (hal_context_save(((tcb_t *) kcb->task_current->data)->context) != 0)
        return;

#if CONFIG_STACK_PROTECTION
    task_stack_check();
#endif

    /* In cooperative mode, delays are only processed on an explicit yield. */
    list_foreach(kcb->tasks, delay_update, NULL);

    /* Save current task before scheduler modifies task_current */
    tcb_t *prev_task = (tcb_t *) kcb->task_current->data;

    sched_select_next_task(); /* Use O(1) priority scheduler */

    /* Switch PMP configuration if tasks have different memory spaces */
    tcb_t *next_task = (tcb_t *) kcb->task_current->data;
    pmp_switch_context(prev_task->mspace, next_task->mspace);

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
    /* Generate random canary for this task */
    tcb->canary = (uint32_t) random();
    /* Ensure canary is never zero */
    if (tcb->canary == 0)
        tcb->canary = 0xDEADBEEFU;

    /* Write canary to both ends of stack */
    *(uint32_t *) stack = tcb->canary;
    *(uint32_t *) ((uintptr_t) stack + stack_size - sizeof(uint32_t)) =
        tcb->canary;
#endif

    tcb->stack = stack;
    tcb->stack_sz = stack_size;
    return true;
}

/* Task Management API */

/* Internal task spawn implementation.
 * Centralizes task creation logic, called by public M-mode and U-mode wrappers.
 */
static int32_t task_spawn_internal(void *task_entry,
                                   uint16_t stack_size_req,
                                   task_mode_t mode)
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
    tcb->mode = mode;
    tcb->in_syscall = false; /* Not in syscall context at creation */

    /* Set default priority with proper scheduler fields */
    tcb->prio = TASK_PRIO_NORMAL;
    tcb->prio_level = extract_priority_level(TASK_PRIO_NORMAL);
    tcb->time_slice = get_priority_timeslice(tcb->prio_level);

    /* Initialize stack */
    if (!init_task_stack(tcb, new_stack_size)) {
        free(tcb);
        panic(ERR_STACK_ALLOC);
    }

    /* Initialize context BEFORE kernel stack allocation.
     * If hal_context_init panics, no kernel stack to leak.
     */
    hal_context_init(&tcb->context, (size_t) tcb->stack, new_stack_size,
                     (size_t) task_entry, tcb->mode);

    /* Allocate kernel stack BEFORE adding to task list to prevent race.
     * If task is added first, another context could cancel it while we're
     * still allocating, causing use-after-free.
     */
    if (tcb->mode) {
        tcb->kernel_stack = malloc(KERNEL_STACK_SIZE);
        if (!tcb->kernel_stack) {
            free(tcb->stack);
            free(tcb);
            panic(ERR_STACK_ALLOC);
        }
        tcb->kernel_stack_size = KERNEL_STACK_SIZE;
    } else {
        tcb->kernel_stack = NULL;
        tcb->kernel_stack_size = 0;
    }

    /* Create memory space for U-mode tasks only.
     * M-mode tasks do not require PMP memory protection.
     */
    if (tcb->mode) {
        tcb->mspace = mo_memspace_create(kcb->next_tid, 0);
        if (!tcb->mspace) {
            free(tcb->kernel_stack);
            free(tcb->stack);
            free(tcb);
            panic(ERR_TCB_ALLOC);
        }

        /* Register stack as flexpage */
        fpage_t *stack_fpage =
            mo_fpage_create((uint32_t) tcb->stack, new_stack_size,
                            PMPCFG_R | PMPCFG_W, PMP_PRIORITY_STACK);
        if (!stack_fpage) {
            mo_memspace_destroy(tcb->mspace);
            free(tcb->kernel_stack);
            free(tcb->stack);
            free(tcb);
            panic(ERR_TCB_ALLOC);
        }

        /* Add stack to memory space */
        stack_fpage->as_next = tcb->mspace->first;
        tcb->mspace->first = stack_fpage;
        tcb->mspace->pmp_stack = stack_fpage;
    } else {
        tcb->mspace = NULL;
    }

    /* Add to task list only after all allocations complete */
    CRITICAL_ENTER();

    if (!kcb->tasks) {
        kcb->tasks = list_create();
        if (!kcb->tasks) {
            CRITICAL_LEAVE();
            if (tcb->kernel_stack)
                free(tcb->kernel_stack);
            free(tcb->stack);
            free(tcb);
            panic(ERR_KCB_ALLOC);
        }
    }

    list_node_t *node = list_pushback(kcb->tasks, tcb);
    if (!node) {
        CRITICAL_LEAVE();
        if (tcb->kernel_stack)
            free(tcb->kernel_stack);
        free(tcb->stack);
        free(tcb);
        panic(ERR_TCB_ALLOC);
    }

    /* Assign unique ID and update counts */
    tcb->id = kcb->next_tid++;
    kcb->task_count++; /* Cached count of active tasks for quick access */

    if (!kcb->task_current)
        kcb->task_current = node;

    CRITICAL_LEAVE();

    /* Initialize SP for preemptive mode.
     * Build initial ISR frame on stack with mepc pointing to task entry.
     * For U-mode tasks, frame is built on kernel stack; for M-mode on user
     * stack.
     */
    void *stack_top = (void *) ((uint8_t *) tcb->stack + new_stack_size);
    tcb->sp =
        hal_build_initial_frame(stack_top, task_entry, tcb->mode,
                                tcb->kernel_stack, tcb->kernel_stack_size);

    printf("task %u: entry=%p stack=%p size=%u mode=%c prio=%u slice=%u\n",
           tcb->id, task_entry, tcb->stack, (unsigned int) new_stack_size,
           task_mode_chars[tcb->mode], tcb->prio_level, tcb->time_slice);

    /* Add to cache and mark ready */
    cache_task(tcb->id, tcb);
    sched_enqueue_task(tcb);

    return tcb->id;
}

/* Creates and starts a new task in kernel (machine) mode.
 * Used by kernel code (logger) and privileged applications.
 *
 * Security (defense-in-depth with multiple layers):
 * 1. Compile-time: private/task.h guard rejects non-kernel builds
 * 2. Runtime: per-task in_syscall check rejects calls from syscall handlers
 * 3. Hardware: read_csr(mstatus) traps immediately if called from U-mode
 */
int32_t mo_task_spawn_kernel(void *task_entry, uint16_t stack_size)
{
    /* Explicit privilege check: reading mstatus from U-mode causes trap.
     * This provides immediate hardware-enforced security before any other code
     * runs. The volatile prevents compiler from optimizing away the check.
     */
    volatile uint32_t mstatus_check __attribute__((unused)) = read_csr(mstatus);

    /* Defense-in-depth: reject M-mode task creation from syscall context.
     * Uses per-task flag (tcb_t.in_syscall) which survives preemption,
     * fixing the concurrency bug where global flag was corrupted.
     */
    if (kcb && kcb->task_current && kcb->task_current->data) {
        tcb_t *self = kcb->task_current->data;
        if (self->in_syscall)
            return -1;
    }

    return task_spawn_internal(task_entry, stack_size, TASK_MODE_M);
}

/* Creates and starts a new task in user mode.
 * Used by kernel bootstrap (main.c) and syscall handlers.
 */
int32_t mo_task_spawn_user(void *task_entry, uint16_t stack_size)
{
    return task_spawn_internal(task_entry, stack_size, TASK_MODE_U);
}

int32_t mo_task_cancel(uint16_t id)
{
    if (id == 0)
        return ERR_TASK_CANT_REMOVE;

    /* Self-termination marks the task as zombie and yields to the scheduler.
     * The dispatcher will reclaim resources after context switch completes.
     */
    if (id == mo_task_id()) {
        tcb_t *self = kcb->task_current->data;
        CRITICAL_ENTER();
        self->state = TASK_ZOMBIE;
        CRITICAL_LEAVE();
        _yield();
        while (1)
            ;
    }

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
    if (tcb->mspace)
        mo_memspace_destroy(tcb->mspace);
    free(tcb->stack);
    if (tcb->kernel_stack)
        free(tcb->kernel_stack);
    free(tcb);
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
    bool is_current = (kcb->task_current->data == task);

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

    /* Enable interrupts before WFI - we're in ISR context with interrupts
     * disabled. WFI needs interrupts enabled to wake up on timer interrupt.
     */
    _ei();

    volatile uint32_t current_ticks = kcb->ticks;
    while (current_ticks == kcb->ticks)
        hal_cpu_idle();

    /* Note: Interrupts will be re-disabled when we return to ISR caller */
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
