#include <linmo.h>

/* U-mode Validation Task
 *
 * Integrates two tests into a single task flow to ensure sequential execution:
 * 1. Phase 1: Mechanism Check - Verify syscalls work.
 * 2. Phase 2: Security Check - Verify privileged instructions trigger a trap.
 */
void umode_validation_task(void)
{
    /* --- Phase 1: Mechanism Check (Syscalls) --- */
    umode_printf("[umode] Phase 1: Testing Syscall Mechanism\n");

    /* Test 1: sys_tid() - Simplest read-only syscall. */
    int my_tid = sys_tid();
    if (my_tid > 0) {
        umode_printf("[umode] PASS: sys_tid() returned %d\n", my_tid);
    } else {
        umode_printf("[umode] FAIL: sys_tid() failed (ret=%d)\n", my_tid);
    }

    /* Test 2: sys_uptime() - Verify value transmission is correct. */
    int uptime = sys_uptime();
    if (uptime >= 0) {
        umode_printf("[umode] PASS: sys_uptime() returned %d\n", uptime);
    } else {
        umode_printf("[umode] FAIL: sys_uptime() failed (ret=%d)\n", uptime);
    }

    /* Note: Skipping sys_tadd for now, as kernel user pointer checks might
     * block function pointers in the .text segment, avoiding distraction.
     */

    /* --- Phase 2: Security Check (Privileged Access) --- */
    umode_printf("[umode] ========================================\n");
    umode_printf("[umode] Phase 2: Testing Security Isolation\n");
    umode_printf(
        "[umode] Action: Attempting to read 'mstatus' CSR from U-mode.\n");
    umode_printf("[umode] Expect: Kernel Panic with 'Illegal instruction'.\n");
    umode_printf("[umode] ========================================\n");

    /* CRITICAL: Delay before suicide to ensure logs are flushed from
     * buffer to UART.
     */
    sys_tdelay(10);

    /* Privileged Instruction Trigger */
    uint32_t mstatus;
    asm volatile("csrr %0, mstatus" : "=r"(mstatus));

    /* If execution reaches here, U-mode isolation failed (still has
     * privileges).
     */
    umode_printf(
        "[umode] FAIL: Privileged instruction executed! (mstatus=0x%lx)\n",
        (long) mstatus);

    /* Spin loop to prevent further execution. */
    while (1)
        sys_tyield();
}

int32_t app_main(void)
{
    umode_printf("[Kernel] Spawning U-mode validation task...\n");

    /* app_main is called from kernel context during bootstrap.
     * Use mo_task_spawn_user to create the validation task in user mode.
     * This ensures privilege isolation is properly tested.
     */
    mo_task_spawn_user(umode_validation_task, DEFAULT_STACK_SIZE);

    /* Return 1 to enable preemptive scheduler */
    return 1;
}
