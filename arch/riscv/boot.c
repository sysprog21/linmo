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

/* C entry points */
void main(void);
void do_trap(uint32_t cause, uint32_t epc, uint32_t isr_sp);
void hal_panic(void);

/* Machine-mode entry point ('_entry'). This is the first code executed on
 * reset. It performs essential low-level setup of the processor state,
 * initializes memory, and then jumps to the C-level main function.
 */
__attribute__((naked, section(".text.prologue"))) void _entry(void)
{
    asm volatile(
        /* Initialize Global Pointer (gp) and Stack Pointer (sp) */
        "la     gp, _gp\n"
        "la     sp, _stack\n"

        /* Initialize Thread Pointer (tp). The ABI requires tp to point to
         * a 64-byte aligned memory region for thread-local storage. Here, we
         * point it to the end of the kernel image and ensure proper alignment.
         */
        "la     tp, _end\n"
        "addi   tp, tp, 63\n"
        "andi   tp, tp, -64\n" /* Align to 64 bytes */

        /* Clear the .bss section to zero */
        "la     a0, _sbss\n"
        "la     a1, _ebss\n"
        "bgeu   a0, a1, .Lbss_done\n"
        ".Lbss_clear_loop:\n"
        "sw     zero, 0(a0)\n"
        "addi   a0, a0, 4\n"
        "bltu   a0, a1, .Lbss_clear_loop\n"
        ".Lbss_done:\n"

        /* Configure machine status register (mstatus)
         * - Set Previous Privilege Mode (MPP) to Machine mode. This ensures
         *   that an 'mret' instruction returns to machine mode.
         * - Machine Interrupt Enable (MIE) is initially disabled.
         */
        "li     t0, %0\n"
        "csrw   mstatus, t0\n"

        /* Disable all interrupts and clear any pending flags */
        "csrw   mie, zero\n"     /* Machine Interrupt Enable */
        "csrw   mip, zero\n"     /* Machine Interrupt Pending */
        "csrw   mideleg, zero\n" /* No interrupt delegation to S-mode */
        "csrw   medeleg, zero\n" /* No exception delegation to S-mode */

        /* Park secondary harts (cores) - only hart 0 continues */
        "csrr   t0, mhartid\n"
        "bnez   t0, .Lpark_hart\n"

        /* Set the machine trap vector (mtvec) to point to our ISR */
        "la     t0, _isr\n"
        "csrw   mtvec, t0\n"

        /* Enable machine-level external interrupts (MIE.MEIE).
         * This allows peripherals like the UART to raise interrupts.
         * Global interrupts remain disabled by mstatus.MIE until the scheduler
         * is ready.
         */
        "li     t0, %1\n"
        "csrw   mie, t0\n"

        /* Jump to the C-level main function */
        "call   main\n"

        /* If main() ever returns, it is a fatal error */
        "call   hal_panic\n"

        ".Lpark_hart:\n"
        "wfi\n"
        "j      .Lpark_hart\n"

        : /* no outputs */
        : "i"(MSTATUS_MPP_MACH), "i"(MIE_MEIE)
        : "memory");
}

/* ISR trap frame layout (144 bytes = 36 words).
 * [0-29]: GPRs (ra, gp, tp, t0-t6, s0-s11, a0-a7)
 * [30]: mcause
 * [31]: mepc
 * [32]: mstatus
 * [33]: SP (user SP in U-mode, original SP in M-mode)
 */
#define ISR_CONTEXT_SIZE 144

/* Low-level ISR common entry for all traps with full context save */
__attribute__((naked, aligned(4))) void _isr(void)
{
    asm volatile(
        /* Blind swap with mscratch for kernel stack isolation.
         * Convention: M-mode (mscratch=0, SP=kernel), U-mode (mscratch=kernel,
         * SP=user). After swap: if SP != 0 came from U-mode, else M-mode.
         */
        "csrrw  sp, mscratch, sp\n"
        "bnez   sp, .Lumode_entry\n"

        /* Undo swap and continue for M-mode */
        "csrrw  sp, mscratch, sp\n"

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
         * 120: mcause, 124: mepc, 128: mstatus, 132: SP
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

        /* Save original SP before frame allocation */
        "addi   t0, sp, %0\n"
        "sw     t0, 33*4(sp)\n"

        /* Save machine CSRs (mcause, mepc, mstatus) */
        "csrr   a0, mcause\n"
        "csrr   a1, mepc\n"
        "csrr   a2, mstatus\n"
        "sw     a0,  30*4(sp)\n"
        "sw     a1,  31*4(sp)\n"
        "sw     a2,  32*4(sp)\n"

        /* Call trap handler with frame pointer */
        "mv     a2, sp\n"
        "call   do_trap\n"
        "mv     sp, a0\n"

        /* Load mstatus and extract MPP to determine M-mode or U-mode return
           path */
        "lw     t0, 32*4(sp)\n"
        "csrw   mstatus, t0\n"

        "srli   t1, t0, 11\n"
        "andi   t1, t1, 0x3\n"
        "beqz   t1, .Lrestore_umode\n"

        /* M-mode restore */
        ".Lrestore_mmode:\n"
        "csrw   mscratch, zero\n"

        "lw     t1, 31*4(sp)\n" /* Restore mepc */
        "csrw   mepc, t1\n"

        /* Restore all GPRs */
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

        /* Restore SP from frame[33] */
        "lw  sp,  33*4(sp)\n"

        /* Return from trap */
        "mret\n"

        /* U-mode entry receives kernel stack in SP and user SP in mscratch */
        ".Lumode_entry:\n"
        "addi   sp, sp, -%0\n"

        /* Save t6 first to preserve it before using it as scratch */
        "sw     t6, 29*4(sp)\n"

        /* Retrieve user SP from mscratch into t6 and save it */
        "csrr   t6, mscratch\n"
        "sw     t6, 33*4(sp)\n"

        /* Save remaining GPRs */
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
        /* t6 already saved */

        /* Save CSRs */
        "csrr   a0, mcause\n"
        "csrr   a1, mepc\n"
        "csrr   a2, mstatus\n"
        "sw     a0,  30*4(sp)\n"
        "sw     a1,  31*4(sp)\n"
        "sw     a2,  32*4(sp)\n"

        "mv     a2, sp\n" /* a2 = ISR frame pointer */
        "call   do_trap\n"
        "mv     sp, a0\n"

        /* Check MPP in mstatus to determine return path */
        "lw     t0, 32*4(sp)\n"
        "csrw   mstatus, t0\n"

        "srli   t1, t0, 11\n"
        "andi   t1, t1, 0x3\n"
        "bnez   t1, .Lrestore_mmode\n"

        /* Setup mscratch for U-mode restore to prepare for next trap */
        ".Lrestore_umode:\n"
        "la     t1, _stack\n"
        "csrw   mscratch, t1\n"

        "lw     t1, 31*4(sp)\n"
        "csrw   mepc, t1\n"

        /* Restore all GPRs */
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

        /* Restore user SP from frame[33] */
        "lw  sp,  33*4(sp)\n"

        /* Return from trap */
        "mret\n"
        : /* no outputs */
        : "i"(ISR_CONTEXT_SIZE)
        : "memory");
}
