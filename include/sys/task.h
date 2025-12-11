#pragma once

/* Task Management and Scheduling
 *
 * Provides a lightweight task model with shared address space and
 * efficient round-robin scheduling. The scheduler uses the master task
 * list with optimized algorithms for better performance than naive O(n)
 * implementations, while maintaining simplicity and compatibility.
 *
 * The scheduler provides O(n) complexity but with small constant factors
 * and excellent practical performance for typical embedded workloads.
 * Priority-aware time slice allocation ensures good responsiveness.
 */

#include <hal.h>
#include <lib/list.h>
#include <lib/queue.h>

/* Task Priority
 *
 * Task priorities are encoded in a 16-bit value with simplified mapping:
 * - Bits 15-8: Base Priority (Static) - mapped to priority level 0-7
 * - Bits  7-0: Time Slice Counter - decremented each tick when running
 *
 * Lower base priority values mean higher priority (level 0 = highest).
 */
enum task_priorities {
    TASK_PRIO_CRIT = 0x0101,     /* Critical, must-run tasks (level 0) */
    TASK_PRIO_REALTIME = 0x0303, /* Real-time tasks (level 1) */
    TASK_PRIO_HIGH = 0x0707,     /* High priority tasks (level 2) */
    TASK_PRIO_ABOVE = 0x0F0F,    /* Above normal priority (level 3) */
    TASK_PRIO_NORMAL = 0x1F1F,   /* Default priority for new tasks (level 4) */
    TASK_PRIO_BELOW = 0x3F3F,    /* Below normal priority (level 5) */
    TASK_PRIO_LOW = 0x7F7F,      /* Low priority tasks (level 6) */
    TASK_PRIO_IDLE = 0xFFFF      /* runs when nothing else ready (level 7) */
};

/* Task Lifecycle States */
enum task_states {
    TASK_STOPPED,  /* Task created but not yet scheduled */
    TASK_READY,    /* Task in ready state, waiting to be scheduled */
    TASK_RUNNING,  /* Task currently executing on CPU */
    TASK_BLOCKED,  /* Task waiting for delay timer to expire */
    TASK_SUSPENDED /* Task paused/excluded from scheduling until resumed */
};

/* Priority Level Constants for Priority-Aware Time Slicing */
#define TASK_PRIORITY_LEVELS 8  /* Number of priority levels (0-7) */
#define TASK_HIGHEST_PRIORITY 0 /* Highest priority level */
#define TASK_LOWEST_PRIORITY 7  /* Lowest priority level */

/* Time slice allocation per priority level (in system ticks) */
#define TASK_TIMESLICE_CRIT 1     /* Critical tasks: minimal slice */
#define TASK_TIMESLICE_REALTIME 2 /* Real-time tasks: small slice */
#define TASK_TIMESLICE_HIGH 3     /* High priority: normal slice */
#define TASK_TIMESLICE_ABOVE 4    /* Above normal: normal slice */
#define TASK_TIMESLICE_NORMAL 5   /* Normal priority: standard slice */
#define TASK_TIMESLICE_BELOW 7    /* Below normal: longer slice */
#define TASK_TIMESLICE_LOW 10     /* Low priority: longer slice */
#define TASK_TIMESLICE_IDLE 15    /* Idle tasks: longest slice */

/* Task Control Block (TCB)
 *
 * Contains all essential information about a single task, including saved
 * context, stack details, and scheduling parameters.
 */
typedef struct tcb {
    /* Context and Stack Management */
    jmp_buf context; /* Saved CPU context (GPRs, SP, PC) for task switching */
    void *sp;        /* Saved stack pointer for preemptive context switch */
    void *stack;     /* Pointer to base of task's allocated stack memory */
    size_t stack_sz; /* Total size of the stack in bytes */
    void (*entry)(void); /* Task's entry point function */

    /* Scheduling Parameters */
    uint16_t prio;      /* Encoded priority (base and time slice counter) */
    uint8_t prio_level; /* Priority level (0-7, 0 = highest) */
    uint8_t time_slice; /* Current time slice remaining */
    uint16_t delay;     /* Ticks remaining for task in TASK_BLOCKED state */
    uint16_t id;        /* Unique task ID, assigned by kernel upon creation */
    uint8_t state;      /* Current lifecycle state (e.g., TASK_READY) */
    uint8_t flags;      /* Task flags for future extensions (reserved) */

    /* Real-time Scheduling Support */
    void *rt_prio; /* Opaque pointer for custom real-time scheduler hook */

    /* Stack Protection */
    uint32_t canary; /* Random stack canary for overflow detection */

    /* State transition support */
    /* Ready queue membership node (only one per task) */
    list_node_t rq_node;
} tcb_t;

/* Kernel Control Block (KCB)
 *
 * Singleton structure holding global kernel state, including task lists,
 * scheduler status, and system-wide counters.
 */
typedef struct {
    /* Task Management */
    list_t *tasks; /* Master list of all tasks (nodes contain tcb_t) */
    list_node_t *task_current; /* Node of currently running task */
    jmp_buf context; /* Saved context of main kernel thread before scheduling */
    uint16_t next_tid;   /* Monotonically increasing ID for next new task */
    uint16_t task_count; /* Cached count of active tasks for quick access */
    bool preemptive;     /* true = preemptive; false = cooperative */

    /* Real-Time Scheduler Hook */
    int32_t (*rt_sched)(void); /* Custom real-time scheduler function */

    /* Timer Management */
    list_t *timer_list;      /* List of active software timers */
    volatile uint32_t ticks; /* Global system tick, incremented by timer */

    /* Scheduling attributions */
    uint8_t ready_bitmap; /* 8-bit priority bitmap */
    list_t
        *ready_queues[TASK_PRIORITY_LEVELS]; /* Separate queue per priority */
    list_node_t *rr_cursors[TASK_PRIORITY_LEVELS]; /* Round-robin position */
} kcb_t;

/* Global pointer to the singleton Kernel Control Block */
extern kcb_t *kcb;

/* System Configuration Constants */
#define MIN_TASK_STACK_SIZE \
    256 /* Minimum stack size to prevent stack overflow */
#define TASK_CACHE_SIZE \
    4 /* Task lookup cache size for frequently accessed tasks */

/* Critical Section Macros
 *
 * Two levels of protection are provided:
 * 1. CRITICAL_* macros disable ALL maskable interrupts globally
 * 2. NOSCHED_* macros disable ONLY the scheduler timer interrupt
 */

/* Disable/enable ALL maskable interrupts globally.
 * Provides strongest protection against concurrency from both other tasks
 * and all ISRs. Use when modifying data shared with any ISR.
 * WARNING: Increases interrupt latency - use NOSCHED macros if protection
 * is only needed against task preemption.
 */
#define CRITICAL_ENTER()     \
    do {                     \
        if (kcb->preemptive) \
            _di();           \
    } while (0)

#define CRITICAL_LEAVE()     \
    do {                     \
        if (kcb->preemptive) \
            _ei();           \
    } while (0)

/* Flag indicating scheduler has started - prevents timer IRQ during early
 * initializations.
 */
extern volatile bool scheduler_started;

/* Disable/enable ONLY the scheduler timer interrupt.
 * Lighter-weight critical section that prevents task preemption but allows
 * other hardware interrupts (e.g., UART) to be serviced, minimizing latency.
 * Use when protecting data shared between tasks.
 *
 * NOSCHED_LEAVE only enables timer if scheduler has started, preventing
 * premature timer interrupts during early initialization (e.g., logger init).
 */
#define NOSCHED_ENTER()              \
    do {                             \
        if (kcb->preemptive)         \
            hal_timer_irq_disable(); \
    } while (0)

#define NOSCHED_LEAVE()                           \
    do {                                          \
        if (kcb->preemptive && scheduler_started) \
            hal_timer_irq_enable();               \
    } while (0)

/* Core Kernel and Task Management API */

/* System Control Functions */

/* Prints a fatal error message and halts the system */
void panic(int32_t ecode);

/* Main scheduler dispatch function, called by timer ISR or ecall */
void dispatcher(int from_timer);

/* Architecture-specific context switch implementations */
void _dispatch(void);
void _yield(void);

/* Task Lifecycle Management */

/* Creates and starts a new task in machine mode.
 * @task_entry : Pointer to the task's entry function (void func(void))
 * @stack_size : The desired stack size in bytes (minimum is enforced)
 *
 * Returns the new task's ID on success. Panics on memory allocation failure.
 */
int32_t mo_task_spawn(void *task_entry, uint16_t stack_size);

/* Creates and starts a new task in user mode.
 * User mode tasks run with reduced privileges and must use syscalls to access
 * kernel services. This provides memory protection and privilege separation.
 *
 * @task_entry : Pointer to the task's entry function (void func(void))
 * @stack_size : The desired stack size in bytes (minimum is enforced)
 *
 * Returns the new task's ID on success. Panics on memory allocation failure.
 */
int32_t mo_task_spawn_user(void *task_entry, uint16_t stack_size);

/* Cancels and removes a task from the system. A task cannot cancel itself.
 * @id : The ID of the task to cancel
 *
 * Returns 0 on success, or a negative error code
 */
int32_t mo_task_cancel(uint16_t id);

/* Task Scheduling Control */

/* Voluntarily yields the CPU, allowing the scheduler to run another task */
void mo_task_yield(void);

/* Blocks the current task for a specified number of system ticks.
 * @ticks : The number of system ticks to sleep. The task will be unblocked
 *          after this duration has passed.
 */
void mo_task_delay(uint16_t ticks);

/* Suspends a task, removing it from scheduling temporarily.
 * @id : The ID of the task to suspend. A task can suspend itself.
 *
 * Returns 0 on success, or a negative error code
 */
int32_t mo_task_suspend(uint16_t id);

/* Resumes a previously suspended task.
 * @id : The ID of the task to resume
 *
 * Returns 0 on success, or a negative error code
 */
int32_t mo_task_resume(uint16_t id);

/* Task Priority Management */

/* Changes a task's base priority.
 * @id       : The ID of the task to modify
 * @priority : The new priority value (from enum task_priorities)
 *
 * Returns 0 on success, or a negative error code
 */
int32_t mo_task_priority(uint16_t id, uint16_t priority);

/* Assigns a task to a custom real-time scheduler.
 * @id       : The ID of the task to modify
 * @priority : Opaque pointer to custom priority data for the RT scheduler
 *
 * Returns 0 on success, or a negative error code
 */
int32_t mo_task_rt_priority(uint16_t id, void *priority);

/* Task Information and Status */

/* Gets the ID of the currently running task.
 *
 * Returns the current task's ID
 */
uint16_t mo_task_id(void);

/* Gets a task's ID from its entry function pointer.
 * @task_entry : Pointer to the task's entry function
 *
 * Returns the task's ID, or ERR_TASK_NOT_FOUND if no task matches
 */
int32_t mo_task_idref(void *task_entry);

/* Puts the CPU into a low-power state, waiting for the next scheduler tick */
void mo_task_wfi(void);

/* Gets the total number of active tasks in the system */
uint16_t mo_task_count(void);

/* System Time Functions */

/* Gets the current value of the system tick counter */
uint32_t mo_ticks(void);

/* Gets the system uptime in milliseconds */
uint64_t mo_uptime(void);

/* Internal Kernel Primitives */

/* Atomically blocks the current task and invokes the scheduler.
 *
 * This internal kernel primitive is the basis for all blocking operations. It
 * must be called from within a NOSCHED_ENTER critical section. It enqueues the
 * current task, sets its state to blocked, and calls the scheduler. The
 * scheduler lock is NOT released by this function; it is released implicitly
 * when the kernel context switches to a new task.
 *
 * @wait_q : The wait queue to which the current task will be added
 */
void _sched_block(queue_t *wait_q);

/* Dequeue path for task with TASK_BLOCKED state. It must be called before task
 * state set as TASK_BLOCKED. Currently, this API is used in mutex lock case.*/
void _sched_block_dequeue(tcb_t *blocked_task);

/* Enqueue path for the task with TASK_BLOCKED state. This API is the mainly
 * enqueuing path for semaphore and mutex operations. */
void _sched_block_enqueue(tcb_t *blocked_task);

/* Application Entry Point */

/* The main entry point for the user application.
 *
 * This function is called by the kernel during initialization. It should
 * create all initial tasks using 'mo_task_spawn()'. The return value
 * configures the scheduler's operating mode.
 *
 * Returns 'true' to enable preemptive scheduling, or 'false' for cooperative
 */
int32_t app_main(void);
