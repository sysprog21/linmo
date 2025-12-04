/*
 * Test suite for the RR-cursor based scheduler implementation.
 *
 * This suite verifies the consistency of the O(1) RR-cursor based scheduler
 * data structures, including bitmap bit positions, ready queues, and RR-cursor
 * It exercises all task states (TASK_READY, TASK_BLOCKED,
 * TASK_SUSPENDED, and TASK_RUNNING) and also verifies correct handling
 * of task cancellation.
 *
 * The semaphore and mutex operations are also included in this unit tests file,
 * verifying the alignment of the new scheduler design.
 */

#include <linmo.h>

#include "private/error.h"

#define TEST_ASSERT(condition, description)    \
    do {                                       \
        if (condition) {                       \
            printf("PASS: %s\n", description); \
            tests_passed++;                    \
        } else {                               \
            printf("FAIL: %s\n", description); \
            tests_failed++;                    \
        }                                      \
    } while (0)

/* Test results tracking */
static int tests_passed = 0;
static int tests_failed = 0;

/* Tasks and resources */
int test_controller;
sem_t *sem;
mutex_t mutex;
cond_t cond;

void task_normal(void)
{
    for (;;) {
        mo_task_wfi();
    }
}

/* Suspend self task */
void task_suspend(void)
{
    mo_task_suspend(mo_task_id());
    while (1)
        mo_task_wfi();
}

/* Delay and block self task */
void task_delay(void)
{
    mo_task_delay(1);
    mo_task_resume(test_controller);
    mo_task_yield();
    while (1)
        mo_task_wfi();
}

/* Mutex lock task; try to obtain lock */
void task_mutex(void)
{
    mo_mutex_lock(&mutex);
    mo_mutex_unlock(&mutex);
    while (1) {
        mo_task_wfi();
    }
}

/* Mutex timed lock task; try to obtain lock before timeout */
void task_mutex_timedlock(void)
{
    uint32_t ticks = 10;
    TEST_ASSERT(mo_mutex_timedlock(&mutex, ticks) == ERR_TIMEOUT,
                " Mutex timeout unlock successful ");
    mo_task_resume(test_controller);
    while (1) {
        mo_task_wfi();
    }
}

/* Mutex condition task; try to obtain lock before timeout */
void task_mutex_cond(void)
{
    /* Acquire mutex */
    mo_mutex_lock(&mutex);

    /* Condition wait; enter condition waiter list and release mutex */
    mo_cond_wait(&cond, &mutex);

    /* Condition release, obtain lock again */
    mo_mutex_unlock(&mutex);

    while (1) {
        mo_task_wfi();
    }
}

/* Mutex condition timed lock task; try to obtain lock before timeout */
void task_mutex_cond_timewait(void)
{
    /* Acquire mutex */
    mo_mutex_lock(&mutex);

    /* Condition wait; enter condition waiter list and release mutex */
    uint32_t ticks = 10;
    TEST_ASSERT(mo_cond_timedwait(&cond, &mutex, ticks) == ERR_TIMEOUT,
                " Mutex condition timeout unlock successful ");

    /* Condition release, obtain lock again */
    mo_mutex_unlock(&mutex);
    mo_task_resume(test_controller);

    while (1) {
        mo_task_wfi();
    }
}

/* Semaphore task; try to obtain lock */
void task_sem(void)
{
    mo_sem_wait(sem);
    while (1)
        mo_task_wfi();
}

/* Idle taskk */
void task_idle(void)
{
    while (1) {
        mo_task_wfi();
    }
}

/* Helpers for verification */

/* Bitmap check */
static bool bit_in_bitmap(int prio)
{
    return ((kcb->ready_bitmap & (1U << prio))) ? true : false;
}

/* Task count check, list length approach */
static int32_t task_cnt_in_sched(int prio)
{
    return kcb->ready_queues[prio]->length;
}

/* Compare all list node id in the ready queue */
static bool task_in_rq(int task_id, int prio)
{
    list_node_t *node = kcb->ready_queues[prio]->head->next;
    while (node != kcb->ready_queues[prio]->tail) {
        if (((tcb_t *) (node->data))->id == task_id)
            return true;
        node = node->next;
    }
    return false;
}

/* Test priority bitmap consistency across task lifecycle transitions:
 * basic task creation, priority migration, and cancellation.
 */
void test_bitmap(void)
{
    printf("\n=== Testing Priority Bitmap Consistency ===\n");

    /* task count = 1 after spawn → bitmap bit should be set */
    int task_id = mo_task_spawn(task_normal, DEFAULT_STACK_SIZE);
    TEST_ASSERT(bit_in_bitmap(4) == true && task_cnt_in_sched(4) == 1,
                "Bitmap sets bit when a same-priority task is spawned");

    /* migrate task to a different priority queue → bitmap updates bits */
    mo_task_priority(task_id, TASK_PRIO_HIGH);
    TEST_ASSERT(bit_in_bitmap(2) == true && bit_in_bitmap(4) == false &&
                    task_cnt_in_sched(2) == 1 && task_cnt_in_sched(4) == 0,
                "Bitmap updates bits correctly after priority migration");

    /* cancel task → ready queue becomes empty, bitmap bit should clear */
    mo_task_cancel(task_id);
    TEST_ASSERT(bit_in_bitmap(2) == false && task_cnt_in_sched(2) == 0,
                "Bitmap clears bit when the migrated task is cancelled");
}

/* Test RR cursor consistency across task lifecycle transitions and
 * task-count changes within a single priority queue.
 *
 * Cursor invariants for a ready queue:
 * - task count == 0: cursor points to NULL
 * - task count == 1: cursor points to the only task node
 * - task count > 1: cursor points to a task node that differs from
 *                   the running task
 *
 * Scenarios:
 * - Running task creates and cancels a same-priority task
 * - Running task creates and cancels tasks in a different priority
 *   queue
 */
void test_cursor(void)
{
    printf("\n=== Testing Cursor Consistency ===\n");

    /* --- Test1: Running task creates a same-priority task and cancels it ---
     */

    /* task count = 1, cursor should point to the only task node
     * (controller, TASK_RUNNING)
     */
    TEST_ASSERT(((tcb_t *) (kcb->rr_cursors[0]->data))->id == test_controller &&
                    task_cnt_in_sched(0) == 1,
                " Task count 1: Cursor points to the only task node");

    /* task count from 1 -> 2, cursor points to a task node different
     * from the running task
     */
    int task_id = mo_task_spawn(task_normal, DEFAULT_STACK_SIZE);
    mo_task_priority(task_id, TASK_PRIO_CRIT);
    TEST_ASSERT(((tcb_t *) (kcb->rr_cursors[0]->data))->id == task_id &&
                    task_cnt_in_sched(0) == 2,
                " Task count 1->2: Cursor points to the new task node which "
                "originally points to the running task ");

    /* cancel the cursor(new) task, task count from 2 -> 1; cursor should move
     * back to the next task(controller)
     */
    mo_task_cancel(task_id);
    TEST_ASSERT(((tcb_t *) (kcb->rr_cursors[0]->data))->id == test_controller &&
                    task_cnt_in_sched(0) == 1,
                " Task count 2->1: Cursor points to next task (controller) "
                "which points to the removed node ");


    /* --- Test2: Running task creates different-priority task and cancels it
     * --- */

    /* task count = 0 */
    TEST_ASSERT(kcb->rr_cursors[4] == NULL && task_cnt_in_sched(4) == 0,
                "Task count 0: Cursor is NULL when the ready queue is empty");

    /* task count from 0 -> 1 */
    int task1_id = mo_task_spawn(task_normal, DEFAULT_STACK_SIZE);
    TEST_ASSERT(
        ((tcb_t *) (kcb->rr_cursors[4]->data))->id == task1_id &&
            task_cnt_in_sched(4) == 1,
        "Task count 0->1: Cursor initialized and points to the new task");

    /* task count from 1 -> 2, cursor does not need to advance */
    int task2_id = mo_task_spawn(task_normal, DEFAULT_STACK_SIZE);
    TEST_ASSERT(((tcb_t *) (kcb->rr_cursors[4]->data))->id == task1_id &&
                    task_cnt_in_sched(4) == 2,
                "Task count 1->2: Cursor is maintained when cursor not same as "
                "the running task ");

    /* task count from 2 -> 1, cancel the cursor task */
    mo_task_cancel(task1_id);
    TEST_ASSERT(
        ((tcb_t *) (kcb->rr_cursors[4]->data))->id == task2_id &&
            task_cnt_in_sched(4) == 1,
        "Task count 2->1: Cursor is advanced when cancelled cursor task ");

    /* task count from 1 -> 0 */
    mo_task_cancel(task2_id);
    TEST_ASSERT(kcb->rr_cursors[4] == NULL && task_cnt_in_sched(4) == 0,
                "Task count 1->0: Cursor is NULL when the ready queue becomes "
                "empty again");
}

/* Task state transition APIs test, including APIs in task.c, semaphore.c and
 * mutex.c */

/* Test ready queue consistency with state transition APIs - normal case */
void test_normal_state_transition(void)
{
    printf("\n=== Testing APIs normal task state transition ===\n");

    /* --- Test1: State transition from TASK_READY  ---*/

    /* TASK_STOPPED to TASK_READY */
    int suspend_task = mo_task_spawn(task_suspend, DEFAULT_STACK_SIZE);
    TEST_ASSERT(task_in_rq(suspend_task, 4) && task_cnt_in_sched(4) == 1,
                " Enqueue successfully: TASK_STOPPED -> TASK_READY");

    /* TASK_READY to TASK_SUSPEND */
    mo_task_suspend(suspend_task);
    TEST_ASSERT(!task_in_rq(suspend_task, 4) && task_cnt_in_sched(4) == 0,
                " Dequeue successfully: TASK_READY -> TASK_SUSPEND");

    /* TASK_SUSPEND to TASK_READY */
    mo_task_resume(suspend_task);
    TEST_ASSERT(task_in_rq(suspend_task, 4) && task_cnt_in_sched(4) == 1,
                " Enqueue successfully: TASK_SUSPEND -> TASK_READY");

    /* --- Test2: State transition from TASK_RUNNING  ---*/

    /* When suspend task is executing (TASK_RUNNING), it will suspend itself
     * (TASK_SUSPENDED) */
    mo_task_priority(suspend_task, TASK_PRIO_CRIT);
    mo_task_yield();

    /* Suspended task should not in the ready queue */
    TEST_ASSERT(!task_in_rq(suspend_task, 0) && task_cnt_in_sched(0) == 1,
                " Dequeue successfully: TASK_RUNNING -> TASK_SUSPEND");

    /* Resume suspended task, it will be enqueued again */
    mo_task_resume(suspend_task);
    TEST_ASSERT(task_in_rq(suspend_task, 0) && task_cnt_in_sched(0) == 2,
                " Enqueue successfully: TASK_SUSPEND -> TASK_READY");

    mo_task_cancel(suspend_task);

    /* --- Test3: Delay task test (TASK_RUNNING) -> (TASK_BLOCKED) ---*/

    int delay_id = mo_task_spawn(task_delay, DEFAULT_STACK_SIZE);
    mo_task_priority(delay_id, TASK_PRIO_CRIT);

    /* Yield to block task, block itself and return to the controller */
    mo_task_yield();
    TEST_ASSERT(!task_in_rq(delay_id, 0) && task_cnt_in_sched(0) == 1,
                " Dequeue successfully: TASK_RUNNING -> TASK_BLOCKED (delay)");

    /* Suspend controller, the delay task will resume controller after delay
     * task wakeup from TASK_BLOCK */
    mo_task_suspend(test_controller);
    TEST_ASSERT(task_cnt_in_sched(0) == 2,
                " Enqueue successfully: TASK_BLOCKED (delay) -> TASK_READY");

    mo_task_cancel(delay_id);
}

/* Test ready queue consistency with state transition APIs - semaphore case */
void test_sem_block_state_transition(void)
{
    printf("\n=== Testing Semaphore ===\n");

    sem = mo_sem_create(1, 1);
    mo_sem_wait(sem);

    /* Create semaphore task and yield controller to it; semaphore task state
     * from TASK_RUNNING to TASK_BLOCKED (wait resource) */
    int sem_id = mo_task_spawn(task_sem, DEFAULT_STACK_SIZE);
    mo_task_priority(sem_id, TASK_PRIO_CRIT);
    mo_task_yield();
    TEST_ASSERT(
        task_cnt_in_sched(0) == 1 && mo_sem_waiting_count(sem) == 1,
        " Semaphore task dequeue successfully when no semaphore resource ");

    /* Controller release a resource, the semaphore task state from TASK_BLOCKED
     * to TASK_READY */
    mo_sem_signal(sem);
    mo_task_yield();
    TEST_ASSERT(
        task_cnt_in_sched(0) == 2 && mo_sem_waiting_count(sem) == 0,
        " Semaphore task enqueue successfully when resource available ");
    mo_sem_destroy(sem);
    mo_task_cancel(sem_id);
}

void test_mutex(void)
{
    printf("\n=== Testing Mutex ===\n");

    /* --- Test1: Mutex lock and unlock ---  */

    /* Create a mutex lock task */
    mo_mutex_init(&mutex);
    int mutex_id = mo_task_spawn(task_mutex, DEFAULT_STACK_SIZE);
    mo_task_priority(mutex_id, TASK_PRIO_CRIT);

    /* Controller acquire mutex lock, yield to mutex task that block itself. The
     * mutex task will try to acquire the mutex lock which has been acquired by
     * the controller; block itself */
    mo_mutex_lock(&mutex);
    mo_task_yield();

    /* Mutex task block itself, return to the controller task */
    TEST_ASSERT(
        task_cnt_in_sched(0) == 1 && mo_mutex_waiting_count(&mutex) == 1,
        " Mutex task dequeue successfully when mutex lock is not available ");

    /* Controller release the mutex lock, mutex task will re-enqueue */
    mo_mutex_unlock(&mutex);
    mo_task_yield();
    TEST_ASSERT(
        task_cnt_in_sched(0) == 2 && mo_mutex_waiting_count(&mutex) == 0,
        " Mutex task enqueue successfully when mutex released by the "
        "controller");
    mo_task_cancel(mutex_id);

    /* --- Test2: Mutex lock timeout ---  */

    mo_mutex_lock(&mutex);
    mutex_id = mo_task_spawn(task_mutex_timedlock, DEFAULT_STACK_SIZE);
    mo_task_priority(mutex_id, TASK_PRIO_CRIT);

    mo_task_yield();
    /* Mutex task block itself, the ready queue task count reduced, mutex lock
     * waiting count added */
    TEST_ASSERT(
        task_cnt_in_sched(0) == 1 && mo_mutex_waiting_count(&mutex) == 1,
        " Timed mutex task dequeue successfully when mutex lock is not "
        "available ");
    /* Controller suspend, timed mutex lock will wakeup controller when timeout
     * happen  */
    mo_task_suspend(mo_task_id());
    mo_mutex_unlock(&mutex);

    TEST_ASSERT(
        task_cnt_in_sched(0) == 2 && mo_mutex_waiting_count(&mutex) == 0,
        " Timed mutex task enqueue successfully when timeout ");
    mo_task_cancel(mutex_id);
}

void test_mutex_cond(void)
{
    printf("\n=== Testing Mutex Condition ===\n");

    /*--- Test 1: Mutex condition wait ---*/
    mo_cond_init(&cond);
    /* Spawn condition wait task */
    int c_wait1 = mo_task_spawn(task_mutex_cond, DEFAULT_STACK_SIZE);
    int c_wait2 = mo_task_spawn(task_mutex_cond, DEFAULT_STACK_SIZE);
    int c_wait3 = mo_task_spawn(task_mutex_cond, DEFAULT_STACK_SIZE);
    mo_task_priority(c_wait1, TASK_PRIO_CRIT);
    mo_task_priority(c_wait2, TASK_PRIO_CRIT);
    mo_task_priority(c_wait3, TASK_PRIO_CRIT);
    mo_task_yield(); /* Yield to condition wait task*/

    /* Check condition wait task is in the waiting list and removed from the
     * ready queue. */
    TEST_ASSERT(task_cnt_in_sched(0) == 1 && mo_cond_waiting_count(&cond) == 3,
                " Condition wait dequeue successfully ");

    mo_cond_signal(&cond);
    mo_task_yield();

    /* Check condition wait task enqueued by signal. */
    TEST_ASSERT(task_cnt_in_sched(0) == 2 && mo_cond_waiting_count(&cond) == 2,
                " Condition wait enqueue successfully by signal ");

    /* Broadcast all condition tasks */
    mo_cond_broadcast(&cond);
    TEST_ASSERT(task_cnt_in_sched(0) == 4 && mo_cond_waiting_count(&cond) == 0,
                " Condition wait enqueue successfully by broadcast ");

    mo_task_cancel(c_wait1);
    mo_task_cancel(c_wait2);
    mo_task_cancel(c_wait3);

    /*--- Test 2: Mutex condition timed wait ---*/
    int c_t_wait1 = mo_task_spawn(task_mutex_cond_timewait, DEFAULT_STACK_SIZE);
    mo_task_priority(c_t_wait1, TASK_PRIO_CRIT);
    mo_task_yield();

    TEST_ASSERT(task_cnt_in_sched(0) == 1 && mo_cond_waiting_count(&cond) == 1,
                " Condition timed wait dequeue successfully ");

    /* Suspend controller task, waiting for condition timed task wake up by
     * timeout and resume controller task*/
    mo_task_suspend(test_controller);

    /* Check waked up task enqueue */
    TEST_ASSERT(task_cnt_in_sched(0) == 2 && mo_cond_waiting_count(&cond) == 0,
                " Condition timed wait enqueue successfully by timeout ");
    mo_task_cancel(c_t_wait1);
}

/* Print test results */
void print_test_results(void)
{
    printf("\n=== Test Results ===\n");
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("Total tests: %d\n", tests_passed + tests_failed);

    if (tests_failed == 0) {
        printf("All tests PASSED!\n");
    } else {
        printf("Some tests FAILED!\n");
    }
}

void schedule_test_task(void)
{
    printf("Starting RR-cursor based scheduler test suits...\n");

    mo_logger_flush();

    test_bitmap();
    test_cursor();
    test_normal_state_transition();
    test_sem_block_state_transition();
    test_mutex();
    test_mutex_cond();

    print_test_results();
    printf("RR-cursor based scheduler tests completed successfully.\n");

    mo_logger_async_resume();
    /* Test complete - go into low-activity mode */
    while (1)
        mo_task_wfi();
}

int32_t app_main(void)
{
    int idle_id = mo_task_spawn(task_idle, DEFAULT_STACK_SIZE);
    mo_task_priority(idle_id, TASK_PRIO_IDLE);

    test_controller = mo_task_spawn(schedule_test_task, DEFAULT_STACK_SIZE);
    mo_task_priority(test_controller, TASK_PRIO_CRIT);
    /* preemptive scheduling */
    return 1;
}
