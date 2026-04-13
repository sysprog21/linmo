#include <linmo.h>
#include <sys/debug_trace.h>
#include <sys/logger.h>

#if CONFIG_DEBUG_TRACE
typedef struct {
    uint16_t worker_a;
    uint16_t worker_b;
    uint16_t verifier;
} trace_sched_ids_t;

static int tests_passed;
static int tests_failed;
static volatile bool scenario_done;
static volatile bool worker_a_ready;
static volatile bool worker_b_ready;
static volatile bool worker_b_suspended;
static volatile bool worker_b_resumed;
static volatile bool worker_b_destroyed;
static trace_sched_ids_t ids;

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

static int find_event_by_type(uint8_t type,
                              uint32_t p1,
                              uint32_t p2,
                              uint32_t start_index)
{
    uint32_t count = debug_trace_count();

    for (uint32_t i = start_index; i < count; i++) {
        debug_event_t event;

        if (debug_trace_get(i, &event) < 0)
            break;

        if (event.event_type == type && event.param1 == p1 &&
            event.param2 == p2)
            return (int) i;
    }

    return -1;
}

static int find_event_by_type_any(uint8_t type, uint32_t p1)
{
    uint32_t count = debug_trace_count();

    for (uint32_t i = 0; i < count; i++) {
        debug_event_t event;

        if (debug_trace_get(i, &event) < 0)
            break;

        if (event.event_type == type && event.param1 == p1)
            return (int) i;
    }

    return -1;
}

static bool assert_event(const char *name, bool ok)
{
    TEST_ASSERT(ok, name);
    return ok;
}

static void flood_yield_events(int iterations)
{
    for (int i = 0; i < iterations; i++)
        mo_task_yield();
}

static void assert_monotonic_timestamps(void)
{
    debug_event_t prev;
    debug_event_t curr;
    uint32_t count = debug_trace_count();

    if (count < 2) {
        TEST_ASSERT(true, "trace timestamps are monotonic");
        return;
    }

    for (uint32_t i = 1; i < count; i++) {
        if (debug_trace_get(i - 1, &prev) < 0 ||
            debug_trace_get(i, &curr) < 0) {
            TEST_ASSERT(false, "trace timestamps are monotonic");
            return;
        }

        if (prev.timestamp > curr.timestamp) {
            TEST_ASSERT(false, "trace timestamps are monotonic");
            return;
        }
    }

    TEST_ASSERT(true, "trace timestamps are monotonic");
}

static void worker_a(void)
{
    worker_a_ready = true;
    while (!worker_b_ready)
        mo_task_wfi();
    mo_task_yield();
    mo_task_delay(2);
    mo_task_suspend(ids.worker_b);
    worker_b_suspended = true;
    scenario_done = true;

    while (!worker_b_destroyed)
        mo_task_wfi();

    for (;;)
        mo_task_wfi();
}

static void worker_b(void)
{
    worker_b_ready = true;
    while (!worker_a_ready)
        mo_task_wfi();

    while (!worker_b_suspended)
        mo_task_wfi();

    while (!worker_b_resumed)
        mo_task_wfi();

    while (!worker_b_destroyed)
        mo_task_wfi();

    for (;;)
        mo_task_wfi();
}

static void verifier(void)
{
    uint32_t trace_count;
    uint32_t trace_overwrites;
    uint32_t total_events;

    while (!scenario_done)
        mo_task_wfi();

    if (mo_task_resume(ids.worker_b) == 0)
        worker_b_resumed = true;

    if (mo_task_cancel(ids.worker_b) == 0)
        worker_b_destroyed = true;

    int create_a = find_event_by_type(EVENT_TASK_CREATE, ids.worker_a, 4U, 0);
    int create_b = find_event_by_type(EVENT_TASK_CREATE, ids.worker_b, 4U, 0);
    int yield_a = find_event_by_type(EVENT_TASK_YIELD, ids.worker_a, 0U, 0);
    int delay_a = find_event_by_type(EVENT_TASK_DELAY, 2U, 0U, 0);
    int suspend_b =
        find_event_by_type(EVENT_TASK_SUSPEND, ids.worker_b, 0U, 0U);
    int resume_b = find_event_by_type(EVENT_TASK_RESUME, ids.worker_b, 0U, 0);
    int destroy_b = find_event_by_type_any(EVENT_TASK_DESTROY, ids.worker_b);

    assert_event("CREATE worker_a prio 4", create_a >= 0);
    assert_event("CREATE worker_b prio 4", create_b >= 0);
    assert_event("YIELD worker_a", yield_a >= 0);
    assert_event("DELAY worker_a ticks 2", delay_a >= 0);
    assert_event("SUSPEND worker_b", suspend_b >= 0);
    assert_event("RESUME worker_b", resume_b >= 0);
    assert_event("DESTROY worker_b", destroy_b >= 0);
    assert_event("CREATE events precede worker_a yield",
                 create_a >= 0 && create_b >= 0 && yield_a >= 0 &&
                     create_a < yield_a && create_b < yield_a);
    assert_event("worker_a yield precedes delay",
                 yield_a >= 0 && delay_a >= 0 && yield_a < delay_a);
    assert_event("worker_a delay precedes worker_b suspend",
                 delay_a >= 0 && suspend_b >= 0 && delay_a < suspend_b);
    assert_event("worker_b suspend precedes resume",
                 suspend_b >= 0 && resume_b >= 0 && suspend_b < resume_b);
    assert_event("worker_b resume precedes destroy",
                 resume_b >= 0 && destroy_b >= 0 && resume_b < destroy_b);

    bool found_switch = false;
    for (uint32_t i = 0; i < debug_trace_count(); i++) {
        debug_event_t event;

        if (debug_trace_get(i, &event) < 0)
            break;

        if (event.event_type == EVENT_TASK_SWITCH &&
            ((event.param1 == ids.worker_a && event.param2 == ids.worker_b) ||
             (event.param1 == ids.worker_b && event.param2 == ids.worker_a))) {
            assert_event("SWITCH between worker_a and worker_b", true);
            found_switch = true;
            break;
        }
    }
    if (!found_switch)
        assert_event("SWITCH between worker_a and worker_b", false);

    flood_yield_events(DEBUG_EVENT_BUFFER_SIZE + 8);
    TEST_ASSERT(debug_trace_overwrites() > 0,
                "trace reports overwrite after wraparound flood");
    assert_monotonic_timestamps();

    mo_logger_flush();
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("Overall: %s\n", tests_failed == 0 ? "PASS" : "FAIL");
    if (tests_failed == 0) {
        trace_count = debug_trace_count();
        trace_overwrites = debug_trace_overwrites();
        total_events = trace_count + trace_overwrites;

        printf("Trace totals: count=%u overwrites=%u total_events=%u\n",
               trace_count, trace_overwrites, total_events);
    }
    if (tests_failed != 0)
        debug_dump_events();

    *(volatile uint32_t *) 0x100000U = 0x5555U;
    for (;;)
        mo_task_wfi();
}

int32_t app_main(void)
{
    debug_clear_events();
    tests_passed = 0;
    tests_failed = 0;
    scenario_done = false;
    worker_a_ready = false;
    worker_b_ready = false;
    worker_b_suspended = false;
    worker_b_resumed = false;
    worker_b_destroyed = false;

    ids.worker_a = mo_task_spawn(worker_a, DEFAULT_STACK_SIZE);
    ids.worker_b = mo_task_spawn(worker_b, DEFAULT_STACK_SIZE);
    ids.verifier = mo_task_spawn(verifier, DEFAULT_STACK_SIZE);

    mo_task_priority(ids.worker_a, TASK_PRIO_NORMAL);
    mo_task_priority(ids.worker_b, TASK_PRIO_NORMAL);
    mo_task_priority(ids.verifier, TASK_PRIO_IDLE);

    return 1;
}
#else
int32_t app_main(void)
{
    debug_clear_events();
    mo_logger_flush();
    printf("Tracing disabled build: PASS\n");
    *(volatile uint32_t *) 0x100000U = 0x5555U;
    return 0;
}
#endif
