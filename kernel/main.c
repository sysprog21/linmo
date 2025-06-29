#include <hal.h>
#include <spinlock.h>
#include <lib/libc.h>
#include <sys/task.h>

#include "private/error.h"

static void idle_task(void)
{
    while (1)
        mo_task_wfi();
}

static volatile bool finish = false;
static spinlock_t finish_lock = SPINLOCK_INITIALIZER;

/* C-level entry point for the kernel.
 *
 * This function is called from the boot code ('_entry'). It is responsible for
 * initializing essential hardware and the memory heap, calling the application
 * main routine to create tasks, and finally starting the scheduler.
 *
 * Under normal operation, this function never returns.
 */
int32_t main(int32_t hartid)
{
    /* Initialize hardware abstraction layer and memory heap. */
    hal_hardware_init();

    if (hartid == 0) {
        printf("Linmo kernel is starting...\n");

        mo_heap_init((void *) &_heap_start, (size_t) &_heap_size);
        printf("Heap initialized, %u bytes available\n",
            (unsigned int) (size_t) &_heap_size);

        /* Call the application's main entry point to create initial tasks. */
        kcb->preemptive = (bool) app_main();
        printf("Scheduler mode: %s\n",
            kcb->preemptive ? "Preemptive" : "Cooperative");

        spin_lock(&finish_lock);
        finish = true;
        spin_unlock(&finish_lock);
    }

    /* Make sure hardware initialize before running the first task. */
    while (1) {
        spin_lock(&finish_lock);
        if (finish)
            break;
        spin_unlock(&finish_lock);
    }
    spin_unlock(&finish_lock);

    mo_task_spawn(idle_task, DEFAULT_STACK_SIZE);

    /* Verify that the application created at least one task.
     * If 'get_task_current()' is still NULL, it means mo_task_spawn was never
     * successfully called.
     */
    if (!get_task_current())
        panic(ERR_NO_TASKS);

    /* Save the kernel's context. This is a formality to establish a base
     * execution context before launching the first real task.
     */
    setjmp(kcb->context);

    spin_lock(&finish_lock);

    /* Launch the first task.
     * 'get_task_current()' was set by the first call to mo_task_spawn.
     * This function transfers control and does not return.
     */

    tcb_t *first_task = get_task_current()->data;
    if (!first_task)
        panic(ERR_NO_TASKS);

    spin_unlock(&finish_lock);

    hal_dispatch_init(first_task->context);

    /* This line should be unreachable. */
    panic(ERR_UNKNOWN);
    return 0;
}
