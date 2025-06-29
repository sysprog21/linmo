/* Start-up and Interrupt Entry Code for RV32I
 *
 * This file contains the machine-mode reset vector ('_entry') and the common
 * interrupt/exception entry point (_isr). It is placed in the .text.prologue
 * section by the linker script to ensure it is located at the very beginning
 * of the executable image, which is where the CPU begins execution on reset.
 */

#include <types.h>

#include "csr.h"

/* Symbols defined in the linker script */
extern uint32_t _gp, _stack, _end;
extern uint32_t _sbss, _ebss;

#define STACK_SIZE_PER_HART 524288

/* C entry points */
void main(void);
void do_trap(uint32_t cause, uint32_t epc);
void hal_panic(void);

/* Machine-mode entry point ('_entry'). This is the first code executed on
 * reset. It performs essential low-level setup of the processor state,
 * initializes memory, and then jumps to the C-level main function.
 */
__attribute__((naked, section(".text.prologue"))) void _entry(void)
{
    asm volatile(
        /* Initialize Global Pointer (gp) and Stack Pointer (sp). */
        "la     gp, _gp\n"
        "la     sp, _stack\n"
        /* Set up stack for each hart */
        "csrr   t0, mhartid\n"               /* t0 = hartid */
        "la     t1, _stack_top\n"            /* t1 = base address of full stack region (top) */
        "li     t2, %2\n"                    /* t2 = per-hart stack size */
        "mul    t0, t0, t2\n"                /* t0 = hartid * STACK_SIZE_PER_HART */
        "sub    sp, t1, t0\n"                /* sp = _stack_top - hartid * stack_size */

        /* Initialize Thread Pointer (tp). The ABI requires tp to point to
         * a 64-byte aligned memory region for thread-local storage. Here, we
         * point it to the end of the kernel image.
         */
        "la     tp, _end\n"
        "addi   tp, tp, 63\n"
        "andi   tp, tp, -64\n" /* Align to 64 bytes */

        /* Clear the .bss section to zero. */
        "la     a0, _sbss\n"
        "la     a1, _ebss\n"
        "bgeu   a0, a1, .Lbss_done\n"
        ".Lbss_clear_loop:\n"
        "sw     zero, 0(a0)\n"
        "addi   a0, a0, 4\n"
        "bltu   a0, a1, .Lbss_clear_loop\n"
        ".Lbss_done:\n"

        /* Configure machine status register (mstatus).
         * - Set Previous Privilege Mode (MPP) to Machine. This ensures that an
         *   'mret' instruction returns to machine mode.
         * - Interrupts are initially disabled (MIE bit is 0).
         */
        "li     t0, %0\n"
        "csrw   mstatus, t0\n"

        /* Disable all interrupts and clear any pending flags. */
        "csrw   mie, zero\n"     /* Machine Interrupt Enable */
        "csrw   mip, zero\n"     /* Machine Interrupt Pending */
        "csrw   mideleg, zero\n" /* No interrupt delegation to S-mode */
        "csrw   medeleg, zero\n" /* No exception delegation to S-mode */

        /* Park secondary harts (cores). */
        "csrr   t0, mhartid\n"
        "bnez   t0, .Lpark_hart\n"

        /* Set the machine trap vector (mtvec) to point to our ISR. */
        "la     t0, _isr\n"
        "csrw   mtvec, t0\n"

        /* Enable machine-level external interrupts (MIE.MEIE).
         * This allows peripherals like the CLINT timer to raise interrupts.
         * Global interrupts remain disabled by mstatus.MIE until the scheduler
         * is ready.
         */
        "li     t0, %1\n"
        "csrw   mie, t0\n"

        /* Jump to the C-level main function. */
        "call   main\n"

        /* If main() ever returns, it is a fatal error. */
        "call   hal_panic\n"

        ".Lpark_hart:\n"
        "wfi\n"
        "j      .Lpark_hart\n"

        : /* no outputs */
        : "i"(MSTATUS_MPP_MACH), "i"(MIE_MEIE), "i"(STACK_SIZE_PER_HART)
        : "memory");
}

/* Size of the full trap context frame saved on the stack by the ISR.
 * 30 GPRs (x1, x3-x31) + mcause + mepc = 32 registers * 4 bytes = 128 bytes.
 * This provides a 16-byte aligned full context save.
 */
#define ISR_CONTEXT_SIZE 128

/* Low-level Interrupt Service Routine (ISR) trampoline.
 *
 * This is the common entry point for all traps. It performs a FULL context
 * save, creating a complete trap frame on the stack. This makes the C handler
 * robust, as it does not need to preserve any registers itself.
 */
__attribute__((naked, aligned(4))) void _isr(void)
{
    asm volatile(
        /* Allocate stack frame for full context save. */
        "addi   sp, sp, -%0\n"

        /* Save all general-purpose registers except x0 (zero) and x2 (sp).
         * This includes caller-saved and callee-saved registers.
         *
         * Stack Frame Layout (offsets from sp in bytes):
         *   0: ra,   4: gp,   8: tp,  12: t0,  16: t1,  20: t2
         *  24: s0,  28: s1,  32: a0,  36: a1,  40: a2,  44: a3
         *  48: a4,  52: a5,  56: a6,  60: a7,  64: s2,  68: s3
         *  72: s4,  76: s5,  80: s6,  84: s7,  88: s8,  92: s9
         *  96: s10, 100:s11, 104:t3, 108: t4, 112: t5, 116: t6
         * 120: mcause, 124: mepc
         */
        "sw  ra,   0*4(sp)\n"
        "sw  gp,   1*4(sp)\n"
        "sw  tp,   2*4(sp)\n"
        "sw  t0,   3*4(sp)\n"
        "sw  t1,   4*4(sp)\n"
        "sw  t2,   5*4(sp)\n"
        "sw  s0,   6*4(sp)\n"
        "sw  s1,   7*4(sp)\n"
        "sw  a0,   8*4(sp)\n"
        "sw  a1,   9*4(sp)\n"
        "sw  a2,  10*4(sp)\n"
        "sw  a3,  11*4(sp)\n"
        "sw  a4,  12*4(sp)\n"
        "sw  a5,  13*4(sp)\n"
        "sw  a6,  14*4(sp)\n"
        "sw  a7,  15*4(sp)\n"
        "sw  s2,  16*4(sp)\n"
        "sw  s3,  17*4(sp)\n"
        "sw  s4,  18*4(sp)\n"
        "sw  s5,  19*4(sp)\n"
        "sw  s6,  20*4(sp)\n"
        "sw  s7,  21*4(sp)\n"
        "sw  s8,  22*4(sp)\n"
        "sw  s9,  23*4(sp)\n"
        "sw  s10, 24*4(sp)\n"
        "sw  s11, 25*4(sp)\n"
        "sw  t3,  26*4(sp)\n"
        "sw  t4,  27*4(sp)\n"
        "sw  t5,  28*4(sp)\n"
        "sw  t6,  29*4(sp)\n"

        /* Save trap-related CSRs and prepare arguments for do_trap. */
        "csrr   a0, mcause\n" /* Arg 1: cause */
        "csrr   a1, mepc\n"   /* Arg 2: epc */
        "sw     a0,  30*4(sp)\n"
        "sw     a1,  31*4(sp)\n"

        /* Call the high-level C trap handler. */
        "call   do_trap\n"

        /* Restore context. mepc might have been changed by the handler. */
        "lw     a1,  31*4(sp)\n"
        "csrw   mepc, a1\n"
        "lw  ra,   0*4(sp)\n"
        "lw  gp,   1*4(sp)\n"
        "lw  tp,   2*4(sp)\n"
        "lw  t0,   3*4(sp)\n"
        "lw  t1,   4*4(sp)\n"
        "lw  t2,   5*4(sp)\n"
        "lw  s0,   6*4(sp)\n"
        "lw  s1,   7*4(sp)\n"
        "lw  a0,   8*4(sp)\n"
        "lw  a1,   9*4(sp)\n"
        "lw  a2,  10*4(sp)\n"
        "lw  a3,  11*4(sp)\n"
        "lw  a4,  12*4(sp)\n"
        "lw  a5,  13*4(sp)\n"
        "lw  a6,  14*4(sp)\n"
        "lw  a7,  15*4(sp)\n"
        "lw  s2,  16*4(sp)\n"
        "lw  s3,  17*4(sp)\n"
        "lw  s4,  18*4(sp)\n"
        "lw  s5,  19*4(sp)\n"
        "lw  s6,  20*4(sp)\n"
        "lw  s7,  21*4(sp)\n"
        "lw  s8,  22*4(sp)\n"
        "lw  s9,  23*4(sp)\n"
        "lw  s10, 24*4(sp)\n"
        "lw  s11, 25*4(sp)\n"
        "lw  t3,  26*4(sp)\n"
        "lw  t4,  27*4(sp)\n"
        "lw  t5,  28*4(sp)\n"
        "lw  t6,  29*4(sp)\n"

        /* Deallocate stack frame. */
        "addi   sp, sp, %0\n"

        /* Return from trap. */
        "mret\n"
        : /* no outputs */
        : "i"(ISR_CONTEXT_SIZE)
        : "memory");
}
