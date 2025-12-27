/* RISC-V Kernel Entry Points
 *
 * This file implements architecture-specific entry mechanisms into the kernel,
 * primarily the system call trap interface using the RISC-V ecall instruction.
 *
 * System Call Calling Convention (RISC-V ABI):
 * - a7 (x17): System call number
 * - a0 (x10): Argument 1 / Return value
 * - a1 (x11): Argument 2
 * - a2 (x12): Argument 3
 *
 * The ecall instruction triggers an environment call exception that transfers
 * control to the M-mode exception handler (hal.c), which then dispatches to
 * the appropriate system call implementation via the syscall table.
 */

#include <sys/syscall.h>
#include <types.h>

/* Architecture-specific syscall implementation using ecall trap.
 * This overrides the weak symbol defined in kernel/syscall.c.
 */
int syscall(int num, void *arg1, void *arg2, void *arg3)
{
    register int a0 asm("a0") = (int) arg1;
    register int a1 asm("a1") = (int) arg2;
    register int a2 asm("a2") = (int) arg3;
    register int a7 asm("a7") = num;

    /* Execute ecall instruction to trap into M-mode.
     * The M-mode exception handler will:
     * 1. Save the current task context
     * 2. Dispatch to the syscall handler based on a7
     * 3. Place the return value in a0
     * 4. Restore context and return to user mode via mret
     */
    asm volatile("ecall"
                 : "+r"(a0) /* a0 is both input (arg1) and output (retval) */
                 : "r"(a1), "r"(a2), "r"(a7)
                 : "memory", "cc");

    return a0;
}

/* Stack Pointer Swap for Testing
 *
 * This naked function provides atomic SP swapping for kernel validation tests.
 * Using __attribute__((naked)) ensures the compiler does not generate any
 * prologue/epilogue code that would use the stack, and prevents instruction
 * reordering that could break the swap semantics.
 *
 * Inspired by Linux kernel's __switch_to for context switching.
 */

/* Atomically swap the stack pointer with a new value.
 * @new_sp: New stack pointer value to install (in a0)
 * @return: Previous stack pointer value (in a0)
 */
__attribute__((naked)) uint32_t __switch_sp(uint32_t new_sp)
{
    asm volatile(
        "mv   t0, sp    \n" /* Save current SP to temporary */
        "mv   sp, a0    \n" /* Install new SP from argument */
        "mv   a0, t0    \n" /* Return old SP in a0 */
        "ret            \n");
}
