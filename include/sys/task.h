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
    TASK_STOPPED,   /* Task created but not yet scheduled */
    TASK_READY,     /* Task in ready state, waiting to be scheduled */
    TASK_RUNNING,   /* Task currently executing on CPU */
    TASK_BLOCKED,   /* Task waiting for delay timer to expire */
    TASK_SUSPENDED, /* Task paused/excluded from scheduling until resumed */
    TASK_ZOMBIE     /* Task terminated, awaiting resource cleanup */
};

/* Task Privilege Mode */
typedef enum {
    TASK_MODE_M, /* Machine mode - full hardware access */
    TASK_MODE_U  /* User mode - restricted, uses syscalls */
} task_mode_t;

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

/* Forward declaration */
struct memspace;

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

    /* Kernel Stack for U-mode Tasks */
    void *kernel_stack; /* Base address of kernel stack (NULL for M-mode) */
    size_t kernel_stack_size; /* Size of kernel stack in bytes (0 for M-mode) */

    /* Memory Protection */
    struct memspace *mspace; /* Memory space for task isolation */

    /* Scheduling Parameters */
    uint16_t prio;      /* Encoded priority (base and time slice counter) */
    uint8_t prio_level; /* Priority level (0-7, 0 = highest) */
    uint8_t time_slice; /* Current time slice remaining */
    uint16_t delay;     /* Ticks remaining for task in TASK_BLOCKED state */
    uint16_t id;        /* Unique task ID, assigned by kernel upon creation */
    uint8_t state;      /* Current lifecycle state (e.g., TASK_READY) */
    task_mode_t mode;   /* Privilege mode: TASK_MODE_M or TASK_MODE_U */

    /* Syscall Context Tracking (per-task, survives preemption).
     * Volatile because this flag is set/cleared around code that may be
     * preempted by timer interrupt, and checked from different code paths.
     */
    volatile bool in_syscall; /* True while executing within do_syscall */

    /* Real-time Scheduling Support */
    void *rt_prio; /* Opaque pointer for custom real-time scheduler hook */

    /* Stack Protection */
    uint32_t canary; /* Random stack canary for overflow detection */
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
} kcb_t;

/* Global pointer to the singleton Kernel Control Block */
extern kcb_t *kcb;

/* System Configuration Constants */
#define SCHED_IMAX \
    500 /* Safety limit for scheduler iterations to prevent livelock */
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

/* Critical section nesting support - defined in kernel/task.c.
 * These enable ISR-safe critical sections by tracking nesting depth and
 * saving/restoring the original interrupt state rather than unconditionally
 * enabling interrupts on CRITICAL_LEAVE.
 */
extern volatile uint32_t critical_nesting;
extern volatile int32_t critical_saved_mie;

/* Disable/enable ALL maskable interrupts globally.
 * Provides strongest protection against concurrency from both other tasks
 * and all ISRs. Use when modifying data shared with any ISR.
 *
 * ISR-SAFE: These macros correctly handle nesting and restore the original
 * interrupt state. Safe to call from ISR context (e.g., timer callbacks).
 *
 * WARNING: Increases interrupt latency - use NOSCHED macros if protection
 * is only needed against task preemption.
 */
#define CRITICAL_ENTER()                   \
    do {                                   \
        if (kcb->preemptive) {             \
            int32_t _mie = _di();          \
            if (critical_nesting == 0)     \
                critical_saved_mie = _mie; \
            critical_nesting++;            \
        }                                  \
    } while (0)

#define CRITICAL_LEAVE()                               \
    do {                                               \
        if (kcb->preemptive && critical_nesting > 0) { \
            critical_nesting--;                        \
            if (critical_nesting == 0)                 \
                hal_interrupt_set(critical_saved_mie); \
        }                                              \
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

/* Core Kernel and Task Management API
 *
 * ISR-Safety Legend:
 *   [ISR-SAFE]    - Safe to call from interrupt context
 *   [TASK-ONLY]   - Must only be called from task context
 *   [ISR-INTERNAL]- Called by ISR infrastructure, not user code
 */

/* System Control Functions */

/* Prints a fatal error message and halts the system.
 * [ISR-SAFE] Safe from any context (terminates system)
 */
void panic(int32_t ecode);

/* Main scheduler dispatch function, called by timer ISR or ecall.
 * [ISR-INTERNAL] Not for user code - called by trap handler
 */
void dispatcher(int from_timer);

/* Architecture-specific context switch implementations.
 * [ISR-INTERNAL] Not for user code - called by scheduler
 */
void _dispatch(void);
void _yield(void);

/* Task Lifecycle Management */

/* Application task creation macro.
 * [TASK-ONLY] Uses malloc() - NOT safe from ISR context
 *
 * @task_entry : Pointer to the task's entry function (void func(void))
 * @stack_size : The desired stack size in bytes (minimum is enforced)
 *
 * Returns the new task's ID on success. Panics on memory allocation failure.
 *
 * Routes to the correct implementation based on build configuration:
 * - CONFIG_PRIVILEGED: Direct kernel call (M-mode task creation)
 * - Otherwise: Syscall wrapper (U-mode task creation, hardware enforced)
 */
#ifdef CONFIG_PRIVILEGED
/* Include private header for kernel-internal function (not exposed to apps) */
#include "private/task.h"
#define mo_task_spawn(task_entry, stack_size) \
    mo_task_spawn_kernel((task_entry), (stack_size))
#else
int sys_task_spawn(void *task, int stack_size);
#define mo_task_spawn(task_entry, stack_size) \
    sys_task_spawn((task_entry), (stack_size))
#endif

/* Cancels and removes a task from the system. A task cannot cancel itself.
 * [TASK-ONLY] Uses free() - NOT safe from ISR context
 *
 * @id : The ID of the task to cancel
 *
 * Returns 0 on success, or a negative error code
 */
int32_t mo_task_cancel(uint16_t id);

/* Task Scheduling Control */

/* Voluntarily yields the CPU, allowing the scheduler to run another task.
 * [TASK-ONLY] Invokes scheduler - NOT safe from ISR context
 */
void mo_task_yield(void);

/* Blocks the current task for a specified number of system ticks.
 * [TASK-ONLY] Blocks caller - NOT safe from ISR context
 *
 * @ticks : The number of system ticks to sleep. The task will be unblocked
 *          after this duration has passed.
 */
void mo_task_delay(uint16_t ticks);

/* Suspends a task, removing it from scheduling temporarily.
 * [TASK-ONLY] Modifies scheduler state - NOT safe from ISR context
 *
 * @id : The ID of the task to suspend. A task can suspend itself.
 *
 * Returns 0 on success, or a negative error code
 */
int32_t mo_task_suspend(uint16_t id);

/* Resumes a previously suspended task.
 * [TASK-ONLY] Modifies scheduler state - NOT safe from ISR context
 *
 * @id : The ID of the task to resume
 *
 * Returns 0 on success, or a negative error code
 */
int32_t mo_task_resume(uint16_t id);

/* Task Priority Management */

/* Changes a task's base priority.
 * [TASK-ONLY] Modifies scheduler state - NOT safe from ISR context
 *
 * @id       : The ID of the task to modify
 * @priority : The new priority value (from enum task_priorities)
 *
 * Returns 0 on success, or a negative error code
 */
int32_t mo_task_priority(uint16_t id, uint16_t priority);

/* Assigns a task to a custom real-time scheduler.
 * [TASK-ONLY] Modifies scheduler state - NOT safe from ISR context
 *
 * @id       : The ID of the task to modify
 * @priority : Opaque pointer to custom priority data for the RT scheduler
 *
 * Returns 0 on success, or a negative error code
 */
int32_t mo_task_rt_priority(uint16_t id, void *priority);

/* Task Information and Status */

/* Gets the ID of the currently running task.
 * [ISR-SAFE] Read-only access to current task pointer
 *
 * Returns the current task's ID
 */
uint16_t mo_task_id(void);

/* Gets a task's ID from its entry function pointer.
 * [ISR-SAFE] Read-only search through task list
 *
 * @task_entry : Pointer to the task's entry function
 *
 * Returns the task's ID, or ERR_TASK_NOT_FOUND if no task matches
 */
int32_t mo_task_idref(void *task_entry);

/* Puts the CPU into a low-power state, waiting for the next scheduler tick.
 * [TASK-ONLY] Executes WFI instruction - context dependent
 */
void mo_task_wfi(void);

/* Gets the total number of active tasks in the system.
 * [ISR-SAFE] Read-only access to cached counter
 */
uint16_t mo_task_count(void);

/* System Time Functions */

/* Gets the current value of the system tick counter.
 * [ISR-SAFE] Read-only access to volatile counter
 */
uint32_t mo_ticks(void);

/* Gets the system uptime in milliseconds.
 * [ISR-SAFE] Calculated from tick counter
 */
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

/* Atomically blocks the current task and invokes the scheduler.
 *
 * This internal kernel primitive is the basis for all blocking operations.
 * Support for mutex lock data structure.
 *
 * @waiters : The wait list to which the current task will be added
 */
void mutex_block_atomic(list_t *waiters);

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
