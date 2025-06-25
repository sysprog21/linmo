#pragma once

#include <hal.h>
#include <lib/list.h>
#include <lib/queue.h>

/* Task Priorities for the Weighted Round-Robin Scheduler.
 *
 * A task's priority is encoded in a 16-bit value:
 * - bits 15-8: Base Priority (Static). This determines the task's "weight".
 *              A lower value means higher priority.
 * - bits  7-0: Dynamic Priority (Counter). This is decremented by the
 * scheduler. A task becomes eligible to run when its counter reaches zero.
 *
 * When a task runs, its counter is reloaded from its base priority. This system
 * ensures that high-priority tasks (with low base values) run more frequently.
 * The enum values duplicate the base priority in both bytes for easy
 * initialization.
 */
enum task_priorities {
    TASK_PRIO_CRIT = 0x0101, /* For critical, must-run tasks. */
    TASK_PRIO_REALTIME = 0x0303,
    TASK_PRIO_HIGH = 0x0707,
    TASK_PRIO_ABOVE = 0x0F0F,
    TASK_PRIO_NORMAL = 0x1F1F, /* Default priority for new tasks. */
    TASK_PRIO_BELOW = 0x3F3F,
    TASK_PRIO_LOW = 0x7F7F,
    TASK_PRIO_IDLE = 0xFFFF /* idle task runs when nothing else is ready. */
};

/* Task Lifecycle States. */
enum task_states {
    TASK_STOPPED,  /* Task has been created but not yet scheduled. */
    TASK_READY,    /* Task is in the ready list, waiting to be scheduled. */
    TASK_RUNNING,  /* Task is currently executing on the CPU. */
    TASK_BLOCKED,  /* Task is waiting for a delay timer to expire. */
    TASK_SUSPENDED /* Task is paused/excluded from scheduling until resumed. */
};

/* Task Control Block (TCB)
 *
 * This structure holds all essential information about a single task,
 * including its saved context, stack details, and scheduling parameters.
 */
typedef struct {
    /* Context and Stack */
    jmp_buf context; /* Saved CPU context (GPRs, SP, PC) for task switching. */
    void *stack; /* Pointer to the base of the task's allocated stack memory. */
    size_t stack_sz;     /* Total size of the stack in bytes. */
    void (*entry)(void); /* The task's entry point function. */

    /* Scheduling */
    uint16_t prio;  /* Encoded priority (base and dynamic counter). */
    uint16_t delay; /* Ticks remaining for a task in the TASK_BLOCKED state. */
    uint16_t id;    /* Unique task ID, assigned by the kernel upon creation. */
    uint8_t state;  /* The current lifecycle state (e.g., TASK_READY). */
    uint8_t flags;  /* Task flags for future extensions (reserved). */

    /* Real-time scheduling support */
    void *rt_prio; /* Opaque pointer for a custom real-time scheduler hook. */
} tcb_t;

/* Kernel Control Block (KCB)
 *
 * A singleton structure that holds the global state of the kernel, including
 * task lists, scheduler status, and system-wide counters.
 */
typedef struct {
    /* Task Management */
    list_t *tasks; /* The master list of all tasks (nodes contain tcb_t). */
    list_node_t *task_current; /* Node of the currently running task. */
    /* Saved context of the main kernel thread before scheduling starts. */
    jmp_buf context;
    uint16_t next_tid; /* Monotonically increasing ID for the next new task. */
    uint16_t task_count; /* Cached count of active tasks for O(1) access. */
    bool preemptive;     /* true for preemptive; false for cooperative. */

    /* Cache the last ready task found to reduce scheduler iterations when
     * multiple ready tasks exist.
     */
    list_node_t *last_ready_hint;

    /* Real-Time Scheduler Hook */
    int32_t (*rt_sched)(void);

    /* Timers */
    list_t *timer_list; /* List of active software timers. */
    /* Global system tick counter, incremented by the timer ISR. */
    volatile uint32_t ticks;
} kcb_t;

/* Global pointer to the singleton Kernel Control Block. */
extern kcb_t *kcb;

/* Critical Section and Scheduler Control */

/* Safety limit for scheduler iterations to prevent livelock. */
#define SCHED_IMAX 500

/* Minimum stack size to prevent stack overflow. */
#define MIN_TASK_STACK_SIZE 256

/* Stack canary checking frequency - check every N context switches to reduce
 * overhead.
 */
#define STACK_CHECK_INTERVAL 32

/* Task lookup cache size for frequently accessed tasks */
#define TASK_CACHE_SIZE 4

/* Core Kernel and Task Management API */

/* Prints a fatal error message and halts the system. */
void panic(int32_t ecode);

/* The main scheduler dispatch function, called by the timer ISR. */
void dispatcher(void);

/* Architecture-specific context switch implementations. */
void _dispatch(void);
void _yield(void);

/* Creates and starts a new task.
 * @task_entry : Pointer to the task's entry function (void func(void)).
 * @stack_size : The desired stack size in bytes. A minimum is enforced.
 * Return The new task's ID on success. Panics on memory allocation failure.
 */
int32_t mo_task_spawn(void *task_entry, uint16_t stack_size);

/* Cancels and removes a task from the system. A task cannot cancel itself.
 * @id The ID of the task to cancel.
 * Return 0 on success, or a negative error code.
 */
int32_t mo_task_cancel(uint16_t id);

/* Voluntarily yields the CPU, allowing the scheduler to run another task. */
void mo_task_yield(void);

/* Blocks the current task for a specified number of system ticks.
 * @ticks The number of system ticks to sleep. The task will be unblocked
 *              after this duration has passed.
 */
void mo_task_delay(uint16_t ticks);

/* Suspends a task, removing it from scheduling temporarily.
 * @id The ID of the task to suspend. A task can suspend itself.
 * Return 0 on success, or a negative error code.
 */
int32_t mo_task_suspend(uint16_t id);

/**
 * @brief Resumes a previously suspended task.
 * @id The ID of the task to resume.
 * Return 0 on success, or a negative error code.
 */
int32_t mo_task_resume(uint16_t id);

/* Changes a task's base priority.
 * @id The ID of the task to modify.
 * @priority The new priority value (from enum task_priorities).
 * Return 0 on success, or a negative error code.
 */
int32_t mo_task_priority(uint16_t id, uint16_t priority);

/* Assigns a task to a custom real-time scheduler.
 * @id The ID of the task to modify.
 * @priority Opaque pointer to custom priority data for the RT scheduler.
 * Return 0 on success, or a negative error code.
 */
int32_t mo_task_rt_priority(uint16_t id, void *priority);

/* Gets the ID of the currently running task.
 * Return The current task's ID.
 */
uint16_t mo_task_id(void);

/* Gets a task's ID from its entry function pointer.
 * @task_entry Pointer to the task's entry function.
 * Return The task's ID, or ERR_TASK_NOT_FOUND if no task matches.
 */
int32_t mo_task_idref(void *task_entry);

/* Puts the CPU into a low-power state, waiting for the next scheduler tick. */
void mo_task_wfi(void);

/* Gets the total number of active tasks in the system. */
uint16_t mo_task_count(void);

/* Gets the current value of the system tick counter. */
uint32_t mo_ticks(void);

/* Gets the system uptime in milliseconds. */
uint64_t mo_uptime(void);

/* Atomically blocks the current task and invokes the scheduler.
 *
 * This internal kernel primitive is the basis for all blocking operations. It
 * must be called from within a NOSCHED_ENTER critical section. It enqueues the
 * current task, sets its state to blocked, and calls the scheduler. The
 * scheduler lock is NOT released by this function; it is released implicitly
 * when the kernel context switches to a new task.
 * @wait_q : The wait queue to which the current task will be added.
 */
void _sched_block(queue_t *wait_q);

/* The main entry point for the user application.
 *
 * This function is called by the kernel during initialization. It should
 * create all initial tasks using 'mo_task_spawn()'. The return value
 * configures the scheduler's operating mode.
 *
 * Return 'true' to enable preemptive scheduling, or 'false' for cooperative.
 */
int32_t app_main(void);
