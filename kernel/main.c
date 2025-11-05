#include <hal.h>
#include <lib/libc.h>
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

    /* Initialize the first current task as idle sentinel node.
     * This ensures a valid entry point before any real task runs.
     */
    idle_task_init();
    kcb->task_current = kcb->harts->task_idle;

    /* Call the application's main entry point to create initial tasks. */
    kcb->preemptive = (bool) app_main();
    printf("Scheduler mode: %s\n",
           kcb->preemptive ? "Preemptive" : "Cooperative");

    /* Save the kernel's context. This is a formality to establish a base
     * execution context before launching the first real task.
     */
    setjmp(kcb->context);

    /* Launch the first task (idle task), then scheduler will select highest
     * priority task. This function transfers control and does not return.
     */
    tcb_t *idle = kcb->task_current->data;
    idle->state = TASK_RUNNING;

    hal_dispatch_init(idle->context);

    /* This line should be unreachable. */
    panic(ERR_UNKNOWN);
    return 0;
}
