/* Software Timer Subsystem Self-Test
 *
 * It creates several timers with different periods and modes to verify
 * their correct operation.
 */
#include <linmo.h>

/* Helper function to print the current system uptime. */
void print_time()
{
    /* mo_uptime() returns a 64-bit value to prevent rollover. */
    uint64_t time_ms = mo_uptime();
    uint32_t secs = time_ms / 1000;
    uint32_t msecs = time_ms % 1000;

    printf("%lu.%03lu", (unsigned long) secs, (unsigned long) msecs);
}

/* Generic timer callback.
 * We can use a single callback for multiple timers by passing a unique
 * identifier as the argument.
 */
void *timer_callback(void *arg)
{
    /* The argument is the timer number, cast from a void pointer. */
    int timer_num = (int) (size_t) arg;

    printf("TIMER %d (", timer_num);
    print_time();
    printf(")\n");

    return NULL;
}

/* An idle task is necessary to ensure there is always at least one task in the
 * TASK_READY state, preventing the scheduler from halting.
 * It yields the CPU in a power-efficient manner.
 */
void idle_task(void)
{
    while (1)
        mo_task_wfi();
}

/* Application entry point */
int32_t app_main(void)
{
    printf("Initializing software timer test...\n");

    /* Create three timers with different periods.
     * The second argument to mo_timer_create is the period in milliseconds.
     * The third argument is passed directly to the callback.
     */
    mo_timer_create(timer_callback, 1000, (void *) 1);
    mo_timer_create(timer_callback, 3000, (void *) 2);
    mo_timer_create(timer_callback, 500, (void *) 3);
    mo_timer_create(timer_callback, 100, (void *) 4);

    /* Start all created timers in auto-reload mode.
     * Note: In this simple case, the IDs will be 0x6000, 0x6001, and 0x6002.
     */
    mo_timer_start(0x6000, TIMER_AUTORELOAD);
    mo_timer_start(0x6001, TIMER_ONESHOT);
    mo_timer_start(0x6002, TIMER_AUTORELOAD);
    mo_timer_start(0x6003, TIMER_AUTORELOAD);

    /* Timer destroy to confirm functions workable; now only timer 3 will run
     * and timer 2 only one shot */
    mo_timer_destroy(0x6000);
    mo_timer_cancel(0x6003);
    /* Destroyed timer can't be resumed */
    mo_timer_start(0x6000, TIMER_AUTORELOAD);

    /* Spawn a single idle task to keep the kernel running. */
    mo_task_spawn(idle_task, DEFAULT_STACK_SIZE);

    /* preemptive mode */
    return 1;
}
