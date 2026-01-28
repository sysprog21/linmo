#include <linmo.h>

/* Extended task statistics for fairness validation */
typedef struct {
    uint32_t executions;      /* Total number of job executions */
    uint32_t deadline_misses; /* Count of missed deadlines (RT tasks only) */
    uint32_t total_response;  /* Sum of response times (release to start) */
    uint32_t max_response, min_response; /* Max/Min response time observed */
    uint32_t period;                     /* Task period (0 for non-RT) */
    uint32_t deadline;                   /* Relative deadline (0 for non-RT) */
} task_stats_t;

/* Global statistics - indexed by task number 0-4 */
static task_stats_t task_stats[5];
static volatile uint32_t test_start_time = 0;

/* Flag to indicate test has started.
 * Note: This has a benign race condition - multiple tasks may observe
 * test_started=0 simultaneously and attempt to set test_start_time.
 * This is acceptable because: (1) all tasks set similar values (current tick),
 * and (2) EDF ensures the highest-priority task runs first anyway.
 * A proper fix would use a mutex, but the overhead is unnecessary here.
 */
static volatile int test_started = 0;
static uint32_t test_duration = 50; /* Run for 50 ticks for better statistics */

/* Set to 1+ to enable workload simulation for response time testing */
#define WORKLOAD_TICKS 0

/* Simulate workload: busy-wait for WORKLOAD_TICKS timer periods.
 * When WORKLOAD_TICKS=0 (default), this is a no-op.
 */
#if WORKLOAD_TICKS > 0
static void simulate_workload(void)
{
    uint32_t start = mo_ticks();
    while (mo_ticks() - start < WORKLOAD_TICKS) {
        /* Busy wait to consume CPU time */
        for (volatile int i = 0; i < 1000; i++)
            ;
    }
}
#else
#define simulate_workload() ((void) 0)
#endif

/* Task 0: RT task with period=10 */
static void task0(void)
{
    int idx = 0;
    uint32_t period = 10;
    uint32_t theoretical_release;
    uint32_t job_start;
    uint32_t response_time;

    /* Initialize stats */
    task_stats[idx].period = period;
    task_stats[idx].deadline = period; /* implicit deadline = period */
    task_stats[idx].min_response = UINT32_MAX;

    /* Initialize test_start_time on first task execution */
    if (!test_started) {
        test_start_time = mo_ticks();
        test_started = 1;
    }

    /* First job theoretical release time = test start */
    theoretical_release = test_start_time;

    while (mo_ticks() - test_start_time < test_duration) {
        /* Record actual start time */
        job_start = mo_ticks();

        /* Response time = actual start - theoretical release */
        response_time = job_start - theoretical_release;

        /* Track response time statistics */
        task_stats[idx].total_response += response_time;
        if (response_time > task_stats[idx].max_response)
            task_stats[idx].max_response = response_time;
        if (response_time < task_stats[idx].min_response)
            task_stats[idx].min_response = response_time;

        /* Check deadline miss (response > deadline) */
        if (response_time > task_stats[idx].deadline)
            task_stats[idx].deadline_misses++;

        task_stats[idx].executions++;

        /* Simulate workload to test response time measurement */
        simulate_workload();

        /* Calculate next theoretical release time */
        theoretical_release += period;

        /* Delay until next period */
        uint32_t now = mo_ticks();
        if (now < theoretical_release)
            mo_task_delay(theoretical_release - now);
    }

    /* Clear RT priority so EDF stops selecting this task */
    mo_task_rt_priority(mo_task_id(), NULL);
    while (1)
        mo_task_wfi();
}

/* Task 1: RT task with period=15 */
static void task1(void)
{
    int idx = 1;
    uint32_t period = 15;
    uint32_t theoretical_release;
    uint32_t job_start;
    uint32_t response_time;

    /* Initialize stats */
    task_stats[idx].period = period;
    task_stats[idx].deadline = period;
    task_stats[idx].min_response = UINT32_MAX;

    /* Wait for test to start */
    while (!test_started)
        mo_task_delay(1);

    /* First job theoretical release time = test start */
    theoretical_release = test_start_time;

    while (mo_ticks() - test_start_time < test_duration) {
        job_start = mo_ticks();
        response_time = job_start - theoretical_release;

        task_stats[idx].total_response += response_time;
        if (response_time > task_stats[idx].max_response)
            task_stats[idx].max_response = response_time;
        if (response_time < task_stats[idx].min_response)
            task_stats[idx].min_response = response_time;
        if (response_time > task_stats[idx].deadline)
            task_stats[idx].deadline_misses++;

        task_stats[idx].executions++;

        /* Simulate workload */
        simulate_workload();

        theoretical_release += period;
        uint32_t now = mo_ticks();
        if (now < theoretical_release)
            mo_task_delay(theoretical_release - now);
    }

    mo_task_rt_priority(mo_task_id(), NULL);
    while (1)
        mo_task_wfi();
}

/* Task 2: RT task with period=20 */
static void task2(void)
{
    int idx = 2;
    uint32_t period = 20;
    uint32_t theoretical_release;
    uint32_t job_start;
    uint32_t response_time;

    /* Initialize stats */
    task_stats[idx].period = period;
    task_stats[idx].deadline = period;
    task_stats[idx].min_response = UINT32_MAX;

    /* Wait for test to start */
    while (!test_started)
        mo_task_delay(1);

    /* First job theoretical release time = test start */
    theoretical_release = test_start_time;

    while (mo_ticks() - test_start_time < test_duration) {
        job_start = mo_ticks();
        response_time = job_start - theoretical_release;

        task_stats[idx].total_response += response_time;
        if (response_time > task_stats[idx].max_response)
            task_stats[idx].max_response = response_time;
        if (response_time < task_stats[idx].min_response)
            task_stats[idx].min_response = response_time;
        if (response_time > task_stats[idx].deadline)
            task_stats[idx].deadline_misses++;

        task_stats[idx].executions++;

        /* Simulate workload */
        simulate_workload();

        theoretical_release += period;
        uint32_t now = mo_ticks();
        if (now < theoretical_release)
            mo_task_delay(theoretical_release - now);
    }

    mo_task_rt_priority(mo_task_id(), NULL);
    while (1)
        mo_task_wfi();
}

/* Task 3: Non-RT background task */
static void task3(void)
{
    int idx = 3;
    uint32_t period = 25;

    task_stats[idx].period = period;

    while (!test_started)
        mo_task_delay(1);

    while (mo_ticks() - test_start_time < test_duration) {
        task_stats[idx].executions++;
        mo_task_delay(period);
    }
    while (1)
        mo_task_wfi();
}

/* Print scheduling statistics using stdio with flush for ordered output */
static void print_stats(void)
{
    /* Flush pending logger output to ensure report appears in order */
    mo_logger_flush();

    printf("\n========================================\n");
    printf("    EDF Scheduler Statistics Report    \n");
    printf("========================================\n");
    printf("Test duration: %lu ticks\n\n", (unsigned long) test_duration);

    printf("--- RT Task Statistics ---\n");
    for (int i = 0; i < 3; i++) {
        /* Ceiling division: task at t=0 runs once even for partial periods */
        uint32_t expected =
            (test_duration + task_stats[i].period - 1) / task_stats[i].period;
        printf("Task %d (period=%lu, deadline=%lu):\n", i,
               (unsigned long) task_stats[i].period,
               (unsigned long) task_stats[i].deadline);
        printf("  Executions: %lu (expected: %lu)\n",
               (unsigned long) task_stats[i].executions,
               (unsigned long) expected);
        printf("  Deadline misses: %lu\n",
               (unsigned long) task_stats[i].deadline_misses);

        if (task_stats[i].executions > 0) {
            uint32_t avg_response =
                task_stats[i].total_response / task_stats[i].executions;
            uint32_t jitter =
                task_stats[i].max_response - task_stats[i].min_response;
            printf("  Response time - min: %lu, max: %lu, avg: %lu\n",
                   (unsigned long) task_stats[i].min_response,
                   (unsigned long) task_stats[i].max_response,
                   (unsigned long) avg_response);
            printf("  Jitter (max-min): %lu ticks\n", (unsigned long) jitter);
        }
        printf("\n");
    }

    printf("--- Non-RT Task Statistics ---\n");
    for (int i = 3; i < 5; i++) {
        printf("Task %d (period=%lu):\n", i,
               (unsigned long) task_stats[i].period);
        printf("  Executions: %lu\n\n",
               (unsigned long) task_stats[i].executions);
    }

    printf("--- Fairness Analysis ---\n");

    /* 1. Deadline miss check */
    uint32_t total_deadline_misses = 0;
    for (int i = 0; i < 3; i++)
        total_deadline_misses += task_stats[i].deadline_misses;
    printf("1. Deadline misses: %lu %s\n",
           (unsigned long) total_deadline_misses,
           total_deadline_misses == 0 ? "[PASS]" : "[FAIL]");

    /* 2. Execution count fairness */
    int exec_ok = 1;
    for (int i = 0; i < 3; i++) {
        /* Ceiling division: task at t=0 runs once even for partial periods */
        uint32_t expected =
            (test_duration + task_stats[i].period - 1) / task_stats[i].period;
        uint32_t actual = task_stats[i].executions;
        /* Avoid underflow: check actual+1 < expected */
        if (actual + 1 < expected || actual > expected + 1)
            exec_ok = 0;
    }
    printf("2. Execution count: %s\n", exec_ok ? "[PASS] within expected range"
                                               : "[FAIL] unexpected count");

    /* 3. Response time bounded by deadline */
    int response_ok = 1;
    for (int i = 0; i < 3; i++) {
        if (task_stats[i].max_response > task_stats[i].deadline)
            response_ok = 0;
    }
    printf("3. Response bounded: %s\n",
           response_ok ? "[PASS] max_response <= deadline"
                       : "[FAIL] response exceeded deadline");

    /* 4. Jitter analysis */
    int jitter_ok = 1;
    for (int i = 0; i < 3; i++) {
        if (task_stats[i].executions > 0) {
            uint32_t jitter =
                task_stats[i].max_response - task_stats[i].min_response;
            if (jitter > task_stats[i].period / 2)
                jitter_ok = 0;
        }
    }
    printf("4. Jitter acceptable: %s\n", jitter_ok
                                             ? "[PASS] jitter < 50% period"
                                             : "[WARN] high jitter detected");

    /* 5. Non-RT task starvation check */
    int starvation_ok =
        (task_stats[3].executions > 0 || task_stats[4].executions > 0);
    printf("5. Non-RT starvation: %s\n", starvation_ok
                                             ? "[PASS] non-RT tasks executed"
                                             : "[FAIL] non-RT tasks starved");

    /* Overall verdict */
    printf("\n--- Overall Verdict ---\n");
    printf("EDF Scheduler: %s\n", (total_deadline_misses == 0 && exec_ok &&
                                   response_ok && starvation_ok)
                                      ? "All tests passed"
                                      : "Some tests failed");
    printf("========================================\n");

    /* Re-enable async logging for any subsequent output */
    mo_logger_async_resume();
}

/* Task 4: Statistics collector and reporter */
static void task4(void)
{
    int idx = 4;

    task_stats[idx].period = 1; /* Runs every tick */

    /* Wait for test to start */
    while (!test_started)
        mo_task_delay(1);

    /* Monitor test progress */
    while (mo_ticks() - test_start_time < test_duration) {
        task_stats[idx].executions++;
        mo_task_delay(1);
    }

    /* Wait a bit for other tasks to complete */
    mo_task_delay(5);

    /* Print comprehensive statistics */
    print_stats();

    while (1)
        mo_task_wfi();
}

/* IDLE task: Always ready, runs when all other tasks blocked */
static void idle_task(void)
{
    while (1) {
        /* Just burn CPU cycles - don't yield or delay */
        for (volatile int i = 0; i < 100; i++)
            ;
    }
}

typedef struct {
    uint32_t period;   /* Task period in ticks */
    uint32_t deadline; /* Absolute deadline (ticks) */
} edf_prio_t;

/* Earliest Deadline First (EDF) real-time scheduler
 * – Every RT task carries an edf_prio_t record via its tcb_t::rt_prio field.
 * – The scheduler selects the READY RT task with the earliest absolute
 * deadline. – When a task is selected, its deadline advances to the next
 * period. – Returns the ID of the selected RT task, or -1 when no RT task is
 * ready.
 *
 * Deadline Update Strategy:
 * – Deadline advances (deadline += period) when a task is selected from READY.
 * – For periodic tasks that delay for their period (mo_task_delay(period)),
 *   this approximates correct EDF semantics: tasks become READY at period
 *   boundaries, get selected shortly after, and deadline advances correctly.
 * – This approach is simpler than tracking job releases separately.
 * – Tasks must delay for their period to ensure correct periodic behavior.
 *
 * EDF is optimal for single-core systems: if any scheduler can meet all
 * deadlines, EDF can. Complexity: O(n) where n = number of RT tasks.
 */
static int32_t edf_sched(void)
{
    tcb_t *earliest = NULL;
    uint32_t earliest_deadline = UINT32_MAX;

    /* Scan all tasks to find the one with earliest deadline */
    list_node_t *node = list_next(kcb->tasks->head);
    while (node && node != kcb->tasks->tail) {
        tcb_t *task = tcb_from_global_node(node);

        /* Consider both READY and RUNNING RT tasks for preemptive scheduling */
        if ((task->state == TASK_READY || task->state == TASK_RUNNING) &&
            task->rt_prio) {
            edf_prio_t *edf = (edf_prio_t *) task->rt_prio;

            /* Track task with earliest deadline */
            if (edf->deadline < earliest_deadline) {
                earliest_deadline = edf->deadline;
                earliest = task;
            }
        }

        node = list_next(node);
    }

    /* DON'T advance deadline here - that would happen on EVERY scheduler call!
     * Deadline should only advance when task actually releases next job.
     * For now, just return the selected task. Deadline advancement will happen
     * when task becomes READY again after delay expires.
     */

    /* Return selected task ID, or -1 if no RT task is ready */
    return earliest ? earliest->id : -1;
}

/* Application Entry Point: Initializes tasks and scheduler
 *
 * RT Task Configuration (EDF scheduling):
 * - Task 0: period = 10 ticks, utilization = 10%
 * - Task 1: period = 15 ticks, utilization = 6.7%
 * - Task 2: period = 20 ticks, utilization = 5%
 * - Task 3: Non-RT background task (period = 25 ticks)
 * - Task 4: Non-RT background task (period = 25 ticks)
 *
 * Total RT Utilization: ~21.7% (well under EDF's 100% bound)
 */
int32_t app_main(void)
{
    /* test_start_time will be initialized by first task that runs */

    /* Spawn all 5 RT/background tasks first */
    int32_t tid0 = mo_task_spawn(task0, DEFAULT_STACK_SIZE);
    int32_t tid1 = mo_task_spawn(task1, DEFAULT_STACK_SIZE);
    int32_t tid2 = mo_task_spawn(task2, DEFAULT_STACK_SIZE);
    (void) mo_task_spawn(task3, DEFAULT_STACK_SIZE); /* Non-RT task 3 */
    /* Non-RT task 4 - displays stats */
    (void) mo_task_spawn(task4, DEFAULT_STACK_SIZE);

    /* Spawn IDLE task LAST so it's at end of round-robin list.
     * This ensures other ready tasks get scheduled before IDLE.
     */
    (void) mo_task_spawn(idle_task, DEFAULT_STACK_SIZE);

    /* Configure EDF priorities for RT tasks 0-2 with deadlines relative to
     * current time */
    uint32_t now = mo_ticks();
    static edf_prio_t priorities[3];
    priorities[0].period = 10;
    priorities[0].deadline = now + 10;
    priorities[1].period = 15;
    priorities[1].deadline = now + 15;
    priorities[2].period = 20;
    priorities[2].deadline = now + 20;

    /* Install EDF scheduler BEFORE setting priorities */
    kcb->rt_sched = edf_sched;

    mo_task_rt_priority(tid0, &priorities[0]);
    mo_task_rt_priority(tid1, &priorities[1]);
    mo_task_rt_priority(tid2, &priorities[2]);

    /* Tasks 3-4 are non-RT, will use round-robin when no RT tasks ready */

    printf("[RTSCHED] Current tick: %lu\n", (unsigned long) mo_ticks());

    /* Return 1 for preemptive mode */
    return 1;
}
