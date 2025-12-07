#include <hal.h>
#include <lib/libc.h>
#include <sys/logger.h>
#include <sys/task.h>

#include "private/error.h"

/* C-level entry point for the kernel.
 *
 * This function is called from the boot code ('_entry'). It is responsible for
 * initializing essential hardware and the memory heap, calling the application
 * main routine to create tasks, and finally starting the scheduler.
 *
 * Under normal operation, this function never returns.
 */
int32_t main(void)
{
    /* Initialize hardware abstraction layer and memory heap. */
    hal_hardware_init();

    printf("Linmo kernel is starting...\n");

    mo_heap_init((void *) &_heap_start, (size_t) &_heap_size);
    printf("Heap initialized, %u bytes available\n",
           (unsigned int) (size_t) &_heap_size);

    /* Seed PRNG with hardware entropy for stack canary randomization.
     * Combine cycle counter (mcycle) and us timer for unpredictability. This
     * prevents fixed canary values across boots.
     */
    uint32_t entropy = read_csr(mcycle) ^ (uint32_t) _read_us();
    srand(entropy);

    /* Initialize deferred logging system.
     * Must be done after heap init but before app_main() to ensure
     * application tasks can use thread-safe printf.
     * Note: Early printf calls (above) use direct output fallback.
     */
    if (mo_logger_init() != 0) {
        printf("Warning: Logger initialization failed, using direct output\n");
    } else {
        printf("Logger initialized\n");
    }

    /* Call the application's main entry point to create initial tasks. */
    kcb->preemptive = (bool) app_main();
    printf("Scheduler mode: %s\n",
           kcb->preemptive ? "Preemptive" : "Cooperative");

    /* Verify that the application created at least one task.
     * If 'kcb->task_current' is still NULL, it means mo_task_spawn was never
     * successfully called.
     */
    if (!kcb->task_current)
        panic(ERR_NO_TASKS);

    /* Save the kernel's context. This is a formality to establish a base
     * execution context before launching the first real task.
     */
    setjmp(kcb->context);

    /* Launch the first task.
     * 'kcb->task_current' was set by the first call to mo_task_spawn.
     * This function transfers control and does not return.
     */
    tcb_t *first_task = tcb_from_global_node(kcb->task_current);
    if (!first_task)
        panic(ERR_NO_TASKS);

    /* Mark scheduler as started - enables timer IRQ in NOSCHED_LEAVE.
     * Must be set before hal_dispatch_init() which enables preemption.
     */
    scheduler_started = true;

    /* In preemptive mode, tasks are managed via ISR frames (sp).
     * In cooperative mode, tasks are managed via jmp_buf (context).
     */
    void *ctx = kcb->preemptive ? first_task->sp : first_task->context;
    hal_dispatch_init(ctx);

    /* This line should be unreachable. */
    panic(ERR_UNKNOWN);
    return 0;
}
