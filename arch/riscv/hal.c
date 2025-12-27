#include <hal.h>
#include <lib/libc.h>
#include <sys/task.h>

#include "csr.h"
#include "private/stdio.h"
#include "private/utils.h"

/* Context frame offsets for jmp_buf (as 32-bit word indices).
 *
 * This layout defines the structure of the jmp_buf. The first 16 elements
 * contain standard execution context as required by the RISC-V ABI for
 * function calls. Element 16 contains processor state for HAL context
 * switching routines.
 *
 * Standard C library setjmp/longjmp use only elements 0-15.
 * HAL context switching routines use all elements 0-16.
 */
#define CONTEXT_S0 0       /* s0 (x8)  - Callee-saved register */
#define CONTEXT_S1 1       /* s1 (x9)  - Callee-saved register */
#define CONTEXT_S2 2       /* s2 (x18) - Callee-saved register */
#define CONTEXT_S3 3       /* s3 (x19) - Callee-saved register */
#define CONTEXT_S4 4       /* s4 (x20) - Callee-saved register */
#define CONTEXT_S5 5       /* s5 (x21) - Callee-saved register */
#define CONTEXT_S6 6       /* s6 (x22) - Callee-saved register */
#define CONTEXT_S7 7       /* s7 (x23) - Callee-saved register */
#define CONTEXT_S8 8       /* s8 (x24) - Callee-saved register */
#define CONTEXT_S9 9       /* s9 (x25) - Callee-saved register */
#define CONTEXT_S10 10     /* s10(x26) - Callee-saved register */
#define CONTEXT_S11 11     /* s11(x27) - Callee-saved register */
#define CONTEXT_GP 12      /* gp (x3)  - Global Pointer */
#define CONTEXT_TP 13      /* tp (x4)  - Thread Pointer */
#define CONTEXT_SP 14      /* sp (x2)  - Stack Pointer */
#define CONTEXT_RA 15      /* ra (x1)  - Return Address / Program Counter */
#define CONTEXT_MSTATUS 16 /* Machine Status CSR */

/* Defines the size of the full trap frame saved by the ISR in 'boot.c'.
 * The _isr routine saves 33 words (30 GPRs + mcause + mepc + mstatus),
 * resulting in a 144-byte frame with alignment padding. This space MUST be
 * reserved at the top of every task's stack (as a "red zone") to guarantee
 * that an interrupt, even at peak stack usage, will not corrupt memory
 * outside the task's stack bounds.
 */
#define ISR_STACK_FRAME_SIZE 144

/* ISR frame register indices (as 32-bit word offsets from isr_sp).
 * This layout matches the stack frame created by _isr in boot.c.
 * Indices are in word offsets (divide byte offset by 4).
 */
enum {
    FRAME_RA = 0,       /* x1  - Return Address */
    FRAME_GP = 1,       /* x3  - Global Pointer */
    FRAME_TP = 2,       /* x4  - Thread Pointer */
    FRAME_T0 = 3,       /* x5  - Temporary register 0 */
    FRAME_T1 = 4,       /* x6  - Temporary register 1 */
    FRAME_T2 = 5,       /* x7  - Temporary register 2 */
    FRAME_S0 = 6,       /* x8  - Saved register 0 / Frame Pointer */
    FRAME_S1 = 7,       /* x9  - Saved register 1 */
    FRAME_A0 = 8,       /* x10 - Argument/Return 0 */
    FRAME_A1 = 9,       /* x11 - Argument/Return 1 */
    FRAME_A2 = 10,      /* x12 - Argument 2 */
    FRAME_A3 = 11,      /* x13 - Argument 3 */
    FRAME_A4 = 12,      /* x14 - Argument 4 */
    FRAME_A5 = 13,      /* x15 - Argument 5 */
    FRAME_A6 = 14,      /* x16 - Argument 6 */
    FRAME_A7 = 15,      /* x17 - Argument 7 / Syscall Number */
    FRAME_S2 = 16,      /* x18 - Saved register 2 */
    FRAME_S3 = 17,      /* x19 - Saved register 3 */
    FRAME_S4 = 18,      /* x20 - Saved register 4 */
    FRAME_S5 = 19,      /* x21 - Saved register 5 */
    FRAME_S6 = 20,      /* x22 - Saved register 6 */
    FRAME_S7 = 21,      /* x23 - Saved register 7 */
    FRAME_S8 = 22,      /* x24 - Saved register 8 */
    FRAME_S9 = 23,      /* x25 - Saved register 9 */
    FRAME_S10 = 24,     /* x26 - Saved register 10 */
    FRAME_S11 = 25,     /* x27 - Saved register 11 */
    FRAME_T3 = 26,      /* x28 - Temporary register 3 */
    FRAME_T4 = 27,      /* x29 - Temporary register 4 */
    FRAME_T5 = 28,      /* x30 - Temporary register 5 */
    FRAME_T6 = 29,      /* x31 - Temporary register 6 */
    FRAME_MCAUSE = 30,  /* Machine Cause CSR */
    FRAME_EPC = 31,     /* Machine Exception PC (mepc) */
    FRAME_MSTATUS = 32, /* Machine Status CSR */
    FRAME_SP = 33       /* Stack Pointer (saved for restore) */
};

/* Global variable to hold the new stack pointer for pending context switch.
 * When a context switch is needed, hal_switch_stack() saves the current SP
 * and stores the new SP here. The ISR epilogue then uses this value.
 * NULL means no context switch is pending, use current SP.
 */
static void *pending_switch_sp = NULL;

/* Global variable to hold the ISR frame SP for the current trap.
 * Set at the start of do_trap() so hal_switch_stack() can save the correct
 * SP to the previous task (the ISR frame SP, not the current function's SP).
 */
static uint32_t current_isr_frame_sp = 0;

/* NS16550A UART0 - Memory-mapped registers for the QEMU 'virt' machine's serial
 * port.
 */
#define NS16550A_UART0_BASE 0x10000000U
#define NS16550A_UART0_REG(off) \
    (*(volatile uint8_t *) (NS16550A_UART0_BASE + (off)))

/* UART register offsets */
#define NS16550A_THR 0x00 /* Transmit Holding Register (write-only) */
#define NS16550A_RBR 0x00 /* Receive Buffer Register (read-only) */
#define NS16550A_DLL 0x00 /* Divisor Latch LSB (when DLAB=1) */
#define NS16550A_DLM 0x01 /* Divisor Latch MSB (when DLAB=1) */
#define NS16550A_LCR 0x03 /* Line Control Register */
#define NS16550A_LSR 0x05 /* Line Status Register */

/* Line Status Register bits */
#define NS16550A_LSR_DR 0x01 /* Data Ready: byte received */
/* Transmit Holding Register Empty: ready to send */
#define NS16550A_LSR_THRE 0x20

/* Line Control Register bits */
#define NS16550A_LCR_8BIT 0x03 /* 8-bit chars, no parity, 1 stop bit (8N1) */
#define NS16550A_LCR_DLAB 0x80 /* Divisor Latch Access Bit */

/* CLINT (Core Local Interrupter) - Provides machine-level timer and software
 * interrupts.
 */
#define CLINT_BASE 0x02000000U
#define MTIMECMP (*(volatile uint64_t *) (CLINT_BASE + 0x4000u))
#define MTIME (*(volatile uint64_t *) (CLINT_BASE + 0xBFF8u))

/* Accessors for 32-bit halves of the 64-bit CLINT registers */
#define MTIMECMP_L (*(volatile uint32_t *) (CLINT_BASE + 0x4000u))
#define MTIMECMP_H (*(volatile uint32_t *) (CLINT_BASE + 0x4004u))
#define MTIME_L (*(volatile uint32_t *) (CLINT_BASE + 0xBFF8u))
#define MTIME_H (*(volatile uint32_t *) (CLINT_BASE + 0xBFFCu))

/* Low-Level I/O and Delay */

/* Backend for 'putchar', writes a single character to the UART. */
static int __putchar(int value)
{
    /* Spin (busy-wait) until the UART's transmit buffer is ready for a new
     * character.
     */
    volatile uint32_t timeout = 0x100000; /* Reasonable timeout limit */
    while (!(NS16550A_UART0_REG(NS16550A_LSR) & NS16550A_LSR_THRE)) {
        if (unlikely(--timeout == 0))
            return 0; /* Hardware timeout */
    }

    NS16550A_UART0_REG(NS16550A_THR) = (uint8_t) value;
    return value;
}

/* Backend for polling stdin, checks if a character has been received. */
static int __kbhit(void)
{
    /* Check the Data Ready (DR) bit in the Line Status Register */
    return (NS16550A_UART0_REG(NS16550A_LSR) & NS16550A_LSR_DR) ? 1 : 0;
}

/* Backend for 'getchar', reads a single character from the UART. */
static int __getchar(void)
{
    /* Block (busy-wait) until a character is available, then read and return
     * it. No timeout here as this is expected to block.
     */
    while (!__kbhit())
        ;
    return (int) NS16550A_UART0_REG(NS16550A_RBR);
}

/* Helper macro to combine high and low 32-bit words into a 64-bit value */
#define CT64(hi, lo) (((uint64_t) (hi) << 32) | (lo))

/* Safely read the 64-bit 'mtime' register on a 32-bit RV32 architecture.
 * A race condition can occur where the lower 32 bits roll over while reading
 * the upper 32 bits. This loop ensures a consistent read by retrying if the
 * high word changes during the operation.
 */
static inline uint64_t mtime_r(void)
{
    uint32_t hi, lo;
    do {
        hi = MTIME_H;
        lo = MTIME_L;
    } while (hi != MTIME_H); /* If 'hi' changed, a rollover occurred. Retry. */
    return CT64(hi, lo);
}

/* Safely read the 64-bit 'mtimecmp' register */
static inline uint64_t mtimecmp_r(void)
{
    uint32_t hi, lo;
    do {
        hi = MTIMECMP_H;
        lo = MTIMECMP_L;
    } while (hi != MTIMECMP_H);
    return CT64(hi, lo);
}

/* Safely write to the 64-bit 'mtimecmp' register on a 32-bit architecture.
 * A direct write of 'lo' then 'hi' could trigger a spurious interrupt if the
 * timer happens to cross the new 'lo' value before 'hi' is updated.
 * To prevent this, the implementation first sets the low word to an impassable
 * value (all 1s), then sets the high word, and finally sets the correct low
 * word. This ensures the full 64-bit compare value becomes active atomically
 * from the timer's perspective.
 */
static inline void mtimecmp_w(uint64_t val)
{
    /* Disable timer interrupts during the critical section */
    uint32_t old_mie = read_csr(mie);
    write_csr(mie, old_mie & ~MIE_MTIE);

    MTIMECMP_L = 0xFFFFFFFF; /* Set to maximum to prevent spurious interrupt */
    MTIMECMP_H = (uint32_t) (val >> 32); /* Set high word */
    MTIMECMP_L = (uint32_t) val;         /* Set low word to final value */

    /* Re-enable timer interrupts if they were previously enabled */
    write_csr(mie, old_mie);
}

/* Returns number of microseconds since boot by reading the 'mtime' counter */
uint64_t _read_us(void)
{
    /* Ensure F_CPU is defined and non-zero to prevent division by zero */
    _Static_assert(F_CPU > 0, "F_CPU must be defined and greater than 0");
    return mtime_r() / (F_CPU / 1000000);
}

/* Provides a blocking, busy-wait delay. This function monopolizes the CPU.
 * It should ONLY be used during early system initialization before the
 * scheduler has started, or for very short, critical delays. In task code, use
 * mo_task_delay() instead to yield the CPU.
 */
void delay_ms(uint32_t msec)
{
    if (!msec)
        return;

    /* Check for potential overflow in calculation */
    const uint64_t max_msec = UINT64_MAX / (F_CPU / 1000);
    if (msec > max_msec) {
        /* Cap delay to maximum safe value */
        msec = (uint32_t) max_msec;
    }

    uint64_t end_time = mtime_r() + ((uint64_t) msec * (F_CPU / 1000));
    while (mtime_r() < end_time) {
        /* Prevent compiler from optimizing away the loop */
        asm volatile("nop");
    }
}

/* Initialization and System Control */

/* Initializes the UART for serial communication at a given baud rate */
static void uart_init(uint32_t baud)
{
    uint32_t divisor = F_CPU / (16 * baud);
    if (unlikely(!divisor))
        divisor = 1; /* Ensure non-zero divisor */

    /* Set DLAB to access divisor registers */
    NS16550A_UART0_REG(NS16550A_LCR) = NS16550A_LCR_DLAB;
    NS16550A_UART0_REG(NS16550A_DLM) = (divisor >> 8) & 0xff;
    NS16550A_UART0_REG(NS16550A_DLL) = divisor & 0xff;
    /* Clear DLAB and set line control to 8N1 mode */
    NS16550A_UART0_REG(NS16550A_LCR) = NS16550A_LCR_8BIT;
}

/* Performs all essential hardware initialization at boot */
void hal_hardware_init(void)
{
    uart_init(USART_BAUD);
    /* Set the first timer interrupt. Subsequent interrupts are set in ISR */
    mtimecmp_w(mtime_r() + (F_CPU / F_TIMER));
    /* Install low-level I/O handlers for the C standard library */
    _stdout_install(__putchar);
    _stdin_install(__getchar);
    _stdpoll_install(__kbhit);

    /* Grant U-mode access to all memory for validation purposes.
     * By default, RISC-V PMP denies all access to U-mode, which would cause
     * instruction access faults immediately upon task switch. This minimal
     * setup allows U-mode tasks to execute and serves as a placeholder until
     * the full PMP driver is integrated.
     */
    uint32_t pmpaddr = -1UL; /* Cover entire address space */
    uint8_t pmpcfg = 0x0F;   /* TOR, R, W, X enabled */

    asm volatile(
        "csrw pmpaddr0, %0\n"
        "csrw pmpcfg0, %1\n"
        :
        : "r"(pmpaddr), "r"(pmpcfg));
}

/* Halts the system in an unrecoverable state */
void hal_panic(void)
{
    _di(); /* Disable all interrupts to prevent further execution */

    /* Attempt a clean shutdown via QEMU 'virt' machine's shutdown device */
    *(volatile uint32_t *) 0x100000U = 0x5555U;

    /* If shutdown fails, halt the CPU in a low-power state indefinitely */
    while (1)
        asm volatile("wfi"); /* Wait For Interrupt */
}

/* Puts the CPU into a low-power state until an interrupt occurs */
void hal_cpu_idle(void)
{
    asm volatile("wfi");
}

/* Interrupt and Trap Handling */

/* Direct UART output for trap context (avoids printf deadlock) */
extern int _putchar(int c);
static void trap_puts(const char *s)
{
    while (*s)
        _putchar(*s++);
}

/* Exception message table per RISC-V Privileged Spec */
static const char *exc_msg[] = {
    [0] = "Instruction address misaligned",
    [1] = "Instruction access fault",
    [2] = "Illegal instruction",
    [3] = "Breakpoint",
    [4] = "Load address misaligned",
    [5] = "Load access fault",
    [6] = "Store/AMO address misaligned",
    [7] = "Store/AMO access fault",
    [8] = "Environment call from U-mode",
    [9] = "Environment call from S-mode",
    [10] = "Reserved",
    [11] = "Environment call from M-mode",
    [12] = "Instruction page fault",
    [13] = "Load page fault",
    [14] = "Reserved",
    [15] = "Store/AMO page fault",
};

/* C-level trap handler, called by the '_isr' assembly routine.
 * @cause : The value of the 'mcause' CSR, indicating the reason for the trap.
 * @epc   : The value of the 'mepc' CSR, the PC at the time of the trap.
 * @isr_sp: The stack pointer pointing to the ISR frame.
 *
 * Returns The SP to use for restoring context (same or new task's frame).
 */
uint32_t do_trap(uint32_t cause, uint32_t epc, uint32_t isr_sp)
{
    /* Reset pending switch at start of every trap */
    pending_switch_sp = NULL;

    /* Store ISR frame SP so hal_switch_stack() can save it to prev task */
    current_isr_frame_sp = isr_sp;

    if (MCAUSE_IS_INTERRUPT(cause)) { /* Asynchronous Interrupt */
        uint32_t int_code = MCAUSE_GET_CODE(cause);
        if (int_code == MCAUSE_MTI) { /* Machine Timer Interrupt */
            /* To avoid timer drift, schedule the next interrupt relative to the
             * previous target time, not the current time. This ensures a
             * consistent tick frequency even with interrupt latency.
             */
            mtimecmp_w(mtimecmp_r() + (F_CPU / F_TIMER));
            /* Invoke scheduler - parameter 1 = from timer, increment ticks */
            dispatcher(1);
        } else {
            /* All other interrupt sources are unexpected and fatal */
            hal_panic();
        }
    } else { /* Synchronous Exception */
        uint32_t code = MCAUSE_GET_CODE(cause);

        /* Handle ecall from U-mode - system calls */
        if (code == MCAUSE_ECALL_UMODE) {
            /* Advance mepc past the ecall instruction (4 bytes) */
            uint32_t new_epc = epc + 4;
            write_csr(mepc, new_epc);

            /* Extract syscall arguments from ISR frame */
            uint32_t *f = (uint32_t *) isr_sp;

            int syscall_num = f[FRAME_A7];
            void *arg1 = (void *) f[FRAME_A0];
            void *arg2 = (void *) f[FRAME_A1];
            void *arg3 = (void *) f[FRAME_A2];

            /* Dispatch to syscall implementation via direct table lookup.
             * Must use do_syscall here instead of syscall() to avoid recursive
             * traps, as the user-space syscall() may be overridden with ecall.
             */
            extern int do_syscall(int num, void *arg1, void *arg2, void *arg3);
            int retval = do_syscall(syscall_num, arg1, arg2, arg3);

            /* Store return value and updated PC */
            f[FRAME_A0] = (uint32_t) retval;
            f[FRAME_EPC] = new_epc;

            return isr_sp;
        }

        /* Handle ecall from M-mode - used for yielding in preemptive mode */
        if (code == MCAUSE_ECALL_MMODE) {
            /* Advance mepc past the ecall instruction (4 bytes) */
            uint32_t new_epc = epc + 4;
            write_csr(mepc, new_epc);

            /* Also update mepc in the ISR frame on the stack!
             * The ISR epilogue will restore mepc from the frame. If we don't
             * update the frame, mret will jump back to the ecall instruction!
             */
            uint32_t *f = (uint32_t *) isr_sp;
            f[FRAME_EPC] = new_epc;

            /* Invoke dispatcher for context switch - parameter 0 = from ecall,
             * don't increment ticks.
             */
            dispatcher(0);

            /* Return the SP to use - new task's frame or current frame */
            return pending_switch_sp ? (uint32_t) pending_switch_sp : isr_sp;
        }

        /* Print exception info via direct UART (safe in trap context) */
        trap_puts("[EXCEPTION] ");
        if (code < ARRAY_SIZE(exc_msg) && exc_msg[code])
            trap_puts(exc_msg[code]);
        else
            trap_puts("Unknown");
        trap_puts(" epc=0x");
        for (int i = 28; i >= 0; i -= 4) {
            uint32_t nibble = (epc >> i) & 0xF;
            _putchar(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
        }

        trap_puts("\r\n");

        hal_panic();
    }

    /* Return the SP to use for context restore - new task's frame or current */
    return pending_switch_sp ? (uint32_t) pending_switch_sp : isr_sp;
}

/* Enables the machine-level timer interrupt source */
void hal_timer_enable(void)
{
    uint64_t now = mtime_r();
    uint64_t target = now + (F_CPU / F_TIMER);
    mtimecmp_w(target);
    write_csr(mie, read_csr(mie) | MIE_MTIE);
}

/* Disables the machine-level timer interrupt source */
void hal_timer_disable(void)
{
    write_csr(mie, read_csr(mie) & ~MIE_MTIE);
}

/* Enable timer interrupt bit only - does NOT reset mtimecmp.
 * Use this for NOSCHED_LEAVE to avoid pushing the interrupt deadline forward.
 */
void hal_timer_irq_enable(void)
{
    write_csr(mie, read_csr(mie) | MIE_MTIE);
}

/* Disable timer interrupt bit only - does NOT touch mtimecmp.
 * Use this for NOSCHED_ENTER to temporarily disable preemption.
 */
void hal_timer_irq_disable(void)
{
    write_csr(mie, read_csr(mie) & ~MIE_MTIE);
}

/* Linker script symbols - needed for task initialization */
extern uint32_t _gp, _end;

/* Build initial ISR frame on task stack for preemptive mode.
 * Returns the stack pointer that points to the frame.
 * When ISR restores from this frame, it will jump to task_entry.
 *
 * CRITICAL: ISR deallocates the frame before mret (sp += 128).
 * We place the frame such that after deallocation, SP is at a safe location.
 *
 * ISR Stack Frame Layout (must match boot.c _isr):
 *   0: ra,   4: gp,   8: tp,  12: t0, ... 116: t6
 * 120: mcause, 124: mepc
 */
void *hal_build_initial_frame(void *stack_top,
                              void (*task_entry)(void),
                              int user_mode)
{
#define INITIAL_STACK_RESERVE \
    256 /* Reserve space below stack_top for task startup */

    /* Place frame deeper in stack so after ISR deallocates (sp += 128),
     * SP will be at (stack_top - INITIAL_STACK_RESERVE), not at stack_top.
     */
    uint32_t *frame =
        (uint32_t *) ((uint8_t *) stack_top - INITIAL_STACK_RESERVE -
                      ISR_STACK_FRAME_SIZE);

    /* Zero out entire frame */
    for (int i = 0; i < 36; i++) {
        frame[i] = 0;
    }

    /* Compute tp value same as boot.c: aligned to 64 bytes from _end */
    uint32_t tp_val = ((uint32_t) &_end + 63) & ~63U;

    /* Initialize critical registers for proper task startup */
    frame[FRAME_GP] = (uint32_t) &_gp; /* gp - global pointer */
    frame[FRAME_TP] = tp_val;          /* tp - thread pointer */

    /* Initialize mstatus for new task:
     * - MPIE=1: mret will copy this to MIE, enabling interrupts after task
     * starts
     * - MPP: Set privilege level (U-mode or M-mode)
     * - MIE=0: Keep interrupts disabled during frame restoration
     */
    uint32_t mstatus_val =
        MSTATUS_MPIE | (user_mode ? MSTATUS_MPP_USER : MSTATUS_MPP_MACH);
    frame[FRAME_MSTATUS] = mstatus_val;

    frame[FRAME_EPC] = (uint32_t) task_entry; /* mepc - entry point */
    /* SP value for when ISR returns. Must match the deallocation in
     * __dispatch_init_isr which adds ISR_STACK_FRAME_SIZE to frame pointer.
     */
    frame[FRAME_SP] = (uint32_t) ((uint8_t *) frame + ISR_STACK_FRAME_SIZE);

    return (void *) frame;
}

/* Context Switching */

/* Saves execution context only (elements 0-15).
 * Returns 0 when called directly.
 */
int32_t setjmp(jmp_buf env)
{
    if (unlikely(!env))
        return -1; /* Invalid parameter */

    asm volatile(
        /* Save all callee-saved registers as required by the RISC-V ABI */
        "sw  s0,   0*4(%0)\n"
        "sw  s1,   1*4(%0)\n"
        "sw  s2,   2*4(%0)\n"
        "sw  s3,   3*4(%0)\n"
        "sw  s4,   4*4(%0)\n"
        "sw  s5,   5*4(%0)\n"
        "sw  s6,   6*4(%0)\n"
        "sw  s7,   7*4(%0)\n"
        "sw  s8,   8*4(%0)\n"
        "sw  s9,   9*4(%0)\n"
        "sw  s10, 10*4(%0)\n"
        "sw  s11, 11*4(%0)\n"
        /* Save essential pointers and the return address */
        "sw  gp,  12*4(%0)\n"
        "sw  tp,  13*4(%0)\n"
        "sw  sp,  14*4(%0)\n"
        "sw  ra,  15*4(%0)\n"
        /* By convention, the initial call to setjmp returns 0 */
        "li a0, 0\n"
        :
        : "r"(env)
        : "memory", "a0");

    return 0;
}

/* Restores execution context only (elements 0-15).
 * Never returns to the caller. Execution resumes at the 'setjmp' call site.
 * @env : Pointer to the saved context (must be valid).
 * @val : The value to be returned by 'setjmp' (coerced to 1 if 0).
 */
__attribute__((noreturn)) void longjmp(jmp_buf env, int32_t val)
{
    if (unlikely(!env))
        hal_panic(); /* Cannot proceed with invalid context */

    /* 'setjmp' must return a non-zero value after 'longjmp' */
    if (val == 0)
        val = 1;

    asm volatile(
        /* Restore all registers from the provided 'jmp_buf' */
        "lw  s0,   0*4(%0)\n"
        "lw  s1,   1*4(%0)\n"
        "lw  s2,   2*4(%0)\n"
        "lw  s3,   3*4(%0)\n"
        "lw  s4,   4*4(%0)\n"
        "lw  s5,   5*4(%0)\n"
        "lw  s6,   6*4(%0)\n"
        "lw  s7,   7*4(%0)\n"
        "lw  s8,   8*4(%0)\n"
        "lw  s9,   9*4(%0)\n"
        "lw  s10, 10*4(%0)\n"
        "lw  s11, 11*4(%0)\n"
        "lw  gp,  12*4(%0)\n"
        "lw  tp,  13*4(%0)\n"
        "lw  sp,  14*4(%0)\n"
        "lw  ra,  15*4(%0)\n"
        /* Set the return value (in 'a0') for the 'setjmp' call */
        "mv  a0,  %1\n"
        /* "Return" to the restored 'ra', effectively jumping to new context */
        "ret\n"
        :
        : "r"(env), "r"(val)
        : "memory");

    __builtin_unreachable(); /* Tell compiler this point is never reached */
}

/* Saves execution context AND processor state.
 * This is the context switching routine used by the CPU scheduler.
 * Returns 0 when called directly, non-zero when restored.
 */
int32_t hal_context_save(jmp_buf env)
{
    if (unlikely(!env))
        return -1; /* Invalid parameter */

    asm volatile(
        /* Save all callee-saved registers as required by the RISC-V ABI */
        "sw  s0,   0*4(%0)\n"
        "sw  s1,   1*4(%0)\n"
        "sw  s2,   2*4(%0)\n"
        "sw  s3,   3*4(%0)\n"
        "sw  s4,   4*4(%0)\n"
        "sw  s5,   5*4(%0)\n"
        "sw  s6,   6*4(%0)\n"
        "sw  s7,   7*4(%0)\n"
        "sw  s8,   8*4(%0)\n"
        "sw  s9,   9*4(%0)\n"
        "sw  s10, 10*4(%0)\n"
        "sw  s11, 11*4(%0)\n"
        /* Save essential pointers and the return address */
        "sw  gp,  12*4(%0)\n"
        "sw  tp,  13*4(%0)\n"
        "sw  sp,  14*4(%0)\n"
        "sw  ra,  15*4(%0)\n"
        /* Save mstatus as-is. According to RISC-V spec section 3.1.6.1,
         * mstatus.mie and mstatus.mpie are automatically managed by hardware:
         * - On trap entry: mpie <- mie, mie <- 0
         * - On MRET: mie <- mpie, mpie <- 1
         * The implementation saves the current state without manual bit
         * manipulation, allowing hardware to correctly manage the interrupt
         * enable stack.
         */
        "csrr t0, mstatus\n"
        "sw   t0, 16*4(%0)\n"
        /* By convention, the initial call returns 0 */
        "li a0, 0\n"
        :
        : "r"(env)
        : "t0", "t1", "t2", "memory", "a0");

    return 0;
}

/* Restores execution context AND processor state.
 * This is the fast context switching routine used by the scheduler.
 * Never returns to the caller.
 * @env : Pointer to the saved context (must be valid).
 * @val : The value to be returned by 'hal_context_save' (coerced to 1 if 0).
 */
__attribute__((noreturn)) void hal_context_restore(jmp_buf env, int32_t val)
{
    if (unlikely(!env))
        hal_panic(); /* Cannot proceed with invalid context */

    /* Validate RA is in text section (simple sanity check) */
    uint32_t ra = env[15]; /* CONTEXT_RA = 15 */
    if (ra < 0x80000000 || ra > 0x80010000) {
        trap_puts("[CTX_ERR] Bad RA=0x");
        for (int i = 28; i >= 0; i -= 4) {
            uint32_t nibble = (ra >> i) & 0xF;
            _putchar(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
        }
        trap_puts("\r\n");
        hal_panic();
    }

    if (val == 0)
        val = 1; /* Must return a non-zero value after restore */

    asm volatile(
        /* Restore mstatus FIRST to ensure correct processor state */
        "lw  t0, 16*4(%0)\n"
        "csrw mstatus, t0\n"
        /* Restore all registers from the provided 'jmp_buf' */
        "lw  s0,   0*4(%0)\n"
        "lw  s1,   1*4(%0)\n"
        "lw  s2,   2*4(%0)\n"
        "lw  s3,   3*4(%0)\n"
        "lw  s4,   4*4(%0)\n"
        "lw  s5,   5*4(%0)\n"
        "lw  s6,   6*4(%0)\n"
        "lw  s7,   7*4(%0)\n"
        "lw  s8,   8*4(%0)\n"
        "lw  s9,   9*4(%0)\n"
        "lw  s10, 10*4(%0)\n"
        "lw  s11, 11*4(%0)\n"
        "lw  gp,  12*4(%0)\n"
        "lw  tp,  13*4(%0)\n"
        "lw  sp,  14*4(%0)\n"
        "lw  ra,  15*4(%0)\n"
        /* Set the return value (in 'a0') */
        "mv  a0,  %1\n"
        /* "Return" to the restored 'ra', effectively jumping to new context */
        "ret\n"
        :
        : "r"(env), "r"(val)
        : "memory");

    __builtin_unreachable(); /* Tell compiler this point is never reached */
}

/* Stack pointer switching for preemptive context switch.
 * Saves current SP to *old_sp and loads new SP from new_sp.
 * Called by dispatcher when switching tasks in preemptive mode.
 * After this returns, ISR will restore registers from the new stack.
 *
 * @old_sp: Pointer to location where current SP should be saved
 * @new_sp: New stack pointer to switch to
 */
void hal_switch_stack(void **old_sp, void *new_sp)
{
    /* Save the ISR frame SP (NOT current SP which is deep in call stack!)
     * to prev task. DO NOT change SP here - that would corrupt the C call
     * stack! Instead, store new_sp in pending_switch_sp for ISR epilogue.
     */
    *old_sp = (void *) current_isr_frame_sp;

    /* Set pending switch - ISR epilogue will use this SP for restore */
    pending_switch_sp = new_sp;
}

/* Enable interrupts on first run of a task.
 * Checks if task's return address still points to entry (meaning it hasn't
 * run yet), and if so, enables global interrupts.
 */
void hal_interrupt_tick(void)
{
    tcb_t *task = kcb->task_current->data;
    if (unlikely(!task))
        hal_panic();

    /* The task's entry point is still in RA, so this is its very first run */
    if ((uint32_t) task->entry == task->context[CONTEXT_RA])
        _ei();
}

/* Low-level context restore helper. Expects a pointer to a 'jmp_buf' in 'a0'.
 * Restores the GPRs, mstatus, and jumps to the restored return address.
 *
 * This function must restore mstatus from the context to be
 * consistent with hal_context_restore(). The first task context is initialized
 * with MSTATUS_MIE | MSTATUS_MPP_MACH by hal_context_init(), which enables
 * interrupts. Failing to restore this value would create an inconsistency
 * where the first task inherits the kernel's mstatus instead of its own.
 */
static void __attribute__((naked, used)) __dispatch_init(void)
{
    asm volatile(
        /* Restore mstatus FIRST to ensure correct processor state.
         * This is critical for interrupt enable state (MSTATUS_MIE).
         * Context was initialized with MIE=1 by hal_context_init().
         */
        "lw  t0, 16*4(a0)\n"
        "csrw mstatus, t0\n"
        /* Now restore all general-purpose registers */
        "lw  s0,   0*4(a0)\n"
        "lw  s1,   1*4(a0)\n"
        "lw  s2,   2*4(a0)\n"
        "lw  s3,   3*4(a0)\n"
        "lw  s4,   4*4(a0)\n"
        "lw  s5,   5*4(a0)\n"
        "lw  s6,   6*4(a0)\n"
        "lw  s7,   7*4(a0)\n"
        "lw  s8,   8*4(a0)\n"
        "lw  s9,   9*4(a0)\n"
        "lw  s10, 10*4(a0)\n"
        "lw  s11, 11*4(a0)\n"
        "lw  gp,  12*4(a0)\n"
        "lw  tp,  13*4(a0)\n"
        "lw  sp,  14*4(a0)\n"
        "lw  t0,  15*4(a0)\n"
        "csrw mepc, t0\n" /* Load task entry point into mepc */
        "mret\n");        /* Jump to the task's entry point */
}

/* Low-level routine to restore context from ISR frame and jump to task.
 * This is used in preemptive mode where tasks are managed via ISR frames.
 */
static void __attribute__((naked, used)) __dispatch_init_isr(void)
{
    asm volatile(
        /* a0 contains the ISR frame pointer (sp value) */
        "mv     sp, a0\n"

        /* Restore mstatus from frame[32] */
        "lw     t0, 32*4(sp)\n"
        "csrw   mstatus, t0\n"

        /* Initialize mscratch based on MPP field in mstatus.
         * For M-mode set mscratch to zero, for U-mode set to kernel stack.
         * ISR uses this to detect privilege mode via blind swap.
         */
        "srli   t2, t0, 11\n"
        "andi   t2, t2, 0x3\n"
        "bnez   t2, .Ldispatch_mmode\n"

        /* U-mode path */
        "la     t2, _stack\n"
        "csrw   mscratch, t2\n"
        "j      .Ldispatch_done\n"

        /* M-mode path */
        ".Ldispatch_mmode:\n"
        "csrw   mscratch, zero\n"
        ".Ldispatch_done:\n"

        /* Restore mepc */
        "lw     t1, 31*4(sp)\n"
        "csrw   mepc, t1\n"

        /* Restore all general-purpose registers */
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

        /* Deallocate stack frame */
        "addi   sp, sp, %0\n"

        /* Return from trap - jump to task entry point */
        "mret\n"
        :
        : "i"(ISR_STACK_FRAME_SIZE)
        : "memory");
}

/* Transfers control from the kernel's main thread to the first task.
 * In preemptive mode, ctx should be the ISR frame pointer (void *sp).
 * In cooperative mode, ctx should be the jmp_buf context.
 */
__attribute__((noreturn)) void hal_dispatch_init(void *ctx)
{
    if (unlikely(!ctx))
        hal_panic(); /* Cannot proceed without valid context */

    if (kcb->preemptive) {
        /* Preemptive mode: ctx is ISR frame pointer, restore from it.
         * Enable timer before jumping to task. Global interrupts will be
         * enabled by mret based on MPIE bit in restored mstatus.
         */
        /* Save ctx before hal_timer_enable modifies registers */
        void *saved_ctx = ctx;

        hal_timer_enable();

        /* Restore ISR frame pointer and call dispatch */
        asm volatile(
            "mv    a0, %0\n"             /* Load ISR frame pointer into a0 */
            "call __dispatch_init_isr\n" /* Restore from ISR frame */
            :
            : "r"(saved_ctx)
            : "a0", "memory");
    } else {
        /* Cooperative mode: ctx is jmp_buf, use standard dispatch */
        _ei(); /* Enable global interrupts */

        asm volatile(
            "mv  a0, %0\n" /* Move @env (the task's context) into 'a0' */
            "call __dispatch_init\n" /* Call the low-level restore routine */
            :
            : "r"(ctx)
            : "a0", "memory");
    }
    __builtin_unreachable();
}

/* Builds an initial 'jmp_buf' context for a brand-new task.
 * @ctx       : Pointer to the 'jmp_buf' to initialize (must be valid).
 * @sp        : Base address of the task's stack (must be valid).
 * @ss        : Total size of the stack in bytes (must be >
 * ISR_STACK_FRAME_SIZE).
 * @ra        : The task's entry point function, used as the initial return
 * address.
 * @user_mode : Non-zero to initialize for user mode, zero for machine mode.
 */
void hal_context_init(jmp_buf *ctx,
                      size_t sp,
                      size_t ss,
                      size_t ra,
                      int user_mode)
{
    if (unlikely(!ctx || !sp || ss < (ISR_STACK_FRAME_SIZE + 64) || !ra))
        hal_panic(); /* Invalid parameters - cannot safely initialize context */

    uintptr_t stack_base = (uintptr_t) sp;
    uintptr_t stack_top;

    /* Reserve a "red zone" for the ISR's full trap frame at top of stack */
    stack_top = (stack_base + ss - ISR_STACK_FRAME_SIZE);

    /* The RISC-V ABI requires the stack pointer to be 16-byte aligned */
    stack_top &= ~0xFUL;

    /* Verify stack alignment and bounds */
    if (unlikely(stack_top <= stack_base || (stack_top & 0xF) != 0))
        hal_panic(); /* Stack configuration error */

    /* Zero the context for predictability */
    memset(ctx, 0, sizeof(*ctx));

    /* Compute tp value same as boot.c: aligned to 64 bytes from _end */
    uint32_t tp_val = ((uint32_t) &_end + 63) & ~63U;

    /* Set global pointer and thread pointer for proper task execution.
     * These are critical for accessing global variables and TLS.
     */
    (*ctx)[CONTEXT_GP] = (uint32_t) &_gp;
    (*ctx)[CONTEXT_TP] = tp_val;

    /* Set the essential registers for a new task:
     * - SP is set to the prepared top of the task's stack.
     * - RA is set to the task's entry point.
     * - mstatus is set to enable interrupts and configure privilege mode.
     *
     * When this context is first restored, the ret instruction will effectively
     * jump to this entry point, starting the task.
     */
    (*ctx)[CONTEXT_SP] = (uint32_t) stack_top;
    (*ctx)[CONTEXT_RA] = (uint32_t) ra;
    /* Note: CONTEXT_MSTATUS not used in cooperative mode (setjmp/longjmp),
     * but set it for consistency with ISR frame initialization */
    (*ctx)[CONTEXT_MSTATUS] =
        MSTATUS_MPIE | (user_mode ? MSTATUS_MPP_USER : MSTATUS_MPP_MACH);
}
