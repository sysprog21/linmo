#include <hal.h>
#include <lib/libc.h>
#include <sys/task.h>

#include "csr.h"
#include "private/stdio.h"
#include "private/utils.h"

/* Context frame offsets for jmp_buf (as 32-bit word indices).
 *
 * This layout defines the structure of the jmp_buf used by setjmp and longjmp
 * to save and restore a task's context. According to the RISC-V ABI,
 * only the callee-saved registers (s0-s11), stack pointer (sp), global pointer
 * (gp), and thread pointer (tp) need to be preserved across function calls.
 * We also save the return address (ra) to control where execution resumes.
 *
 * This is distinct from the full trap frame saved by the ISR. The jmp_buf is
 * smaller because it only needs to preserve the context from the perspective
 * of a C function call, which is exactly what a context switch is.
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
#define CONTEXT_MCAUSE 16  /* mcause   - Machine Cause CSR (for debugging) */
#define CONTEXT_MEPC 17    /* mepc     - Machine Exception PC CSR */
#define CONTEXT_MSTATUS 18 /* mstatus  - Machine Status CSR */

/* Defines the size of the full trap frame saved by the ISR in 'boot.c'.
 * The _isr routine saves 32 registers (30 GPRs + mcause + mepc), resulting
 * in a 128-byte frame. This space MUST be reserved at the top of every task's
 * stack (as a "red zone") to guarantee that an interrupt, even at peak stack
 * usage, will not corrupt memory outside the task's stack bounds.
 */
#define ISR_STACK_FRAME_SIZE 128

/* NS16550A UART0 - Memory-mapped registers for the QEMU 'virt' machine's serial
 * port.
 */
#define NS16550A_UART0_BASE 0x10000000U
#define NS16550A_UART0_REG(off) \
    (*(volatile uint8_t *) (NS16550A_UART0_BASE + (off)))
#define NS16550A_LSR 0x05 /* Line Status Register: provides status flags. */
/* Transmit Holding Register Empty: if set, ready to send. */
#define NS16550A_LSR_THRE 0x20
/* Transmit Holding Register (write-only): for sending data. */
#define NS16550A_THR 0x00
/* Data Ready: if set, a byte has been received. */
#define NS16550A_LSR_DR 0x01
/* Receive Buffer Register (read-only): for receiving data. */
#define NS16550A_RBR 0x00

/* CLINT (Core Local Interrupter) - Provides machine-level timer and software
 * interrupts.
 */
#define CLINT_BASE 0x02000000U
#define MTIMECMP (*(volatile uint64_t *) (CLINT_BASE + 0x4000u))
#define MTIME (*(volatile uint64_t *) (CLINT_BASE + 0xBFF8u))
/* Accessors for 32-bit halves of the 64-bit CLINT registers. */
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
        if (--timeout == 0)
            return 0; /* Hardware timeout */
    }

    NS16550A_UART0_REG(NS16550A_THR) = (uint8_t) value;
    return value;
}

/* Backend for polling stdin, checks if a character has been received. */
static int __kbhit(void)
{
    /* Check the Data Ready (DR) bit in the Line Status Register. */
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

/* Helper macro to combine high and low 32-bit words into a 64-bit value. */
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

/* Safely read the 64-bit 'mtimecmp' register. */
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
 * To prevent this, we first set the low word to an impassable value (all 1s),
 * then set the high word, and finally set the correct low word. This ensures
 * the full 64-bit compare value becomes active atomically from the timer's
 * view.
 */
static inline void mtimecmp_w(uint64_t val)
{
    /* Disable interrupts during the critical section to ensure atomicity */
    uint32_t old_mie = read_csr(mie);
    write_csr(mie, old_mie & ~MIE_MTIE);

    MTIMECMP_L = 0xFFFFFFFF; /* Set to maximum to prevent spurious interrupt */
    MTIMECMP_H = (uint32_t) (val >> 32); /* Set high word */
    MTIMECMP_L = (uint32_t) val;         /* Set low word to final value */

    /* Re-enable timer interrupts if they were previously enabled */
    write_csr(mie, old_mie);
}

/* Returns number of microseconds since boot by reading the 'mtime' counter. */
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
        /* Prevent compiler from optimizing away the loop. */
        asm volatile("nop");
    }
}

/* Initialization and System Control */

/* UART Line Control Register bits */
#define NS16550A_LCR 0x03
/* Divisor Latch Access Bit: enables writing to baud rate divisor. */
#define NS16550A_LCR_DLAB 0x80
#define NS16550A_DLL 0x00 /* Divisor Latch LSB */
#define NS16550A_DLM 0x01 /* Divisor Latch MSB */
/* Config for 8-bit chars, no parity, 1 stop bit (8N1). */
#define NS16550A_LCR_8BIT 0x03

/* Initializes the UART for serial communication at a given baud rate. */
static void uart_init(uint32_t baud)
{
    uint32_t divisor = F_CPU / (16 * baud);
    if (unlikely(!divisor))
        divisor = 1; /* Ensure non-zero divisor */

    /* Set DLAB to access divisor registers. */
    NS16550A_UART0_REG(NS16550A_LCR) = NS16550A_LCR_DLAB;
    NS16550A_UART0_REG(NS16550A_DLM) = (divisor >> 8) & 0xff;
    NS16550A_UART0_REG(NS16550A_DLL) = divisor & 0xff;
    /* Clear DLAB and set line control to 8N1 mode. */
    NS16550A_UART0_REG(NS16550A_LCR) = NS16550A_LCR_8BIT;
}

/* Performs all essential hardware initialization at boot. */
void hal_hardware_init(void)
{
    uart_init(USART_BAUD);
    /* Set the first timer interrupt. Subsequent interrupts are set in ISR. */
    mtimecmp_w(mtime_r() + (F_CPU / F_TIMER));
    /* Install low-level I/O handlers for the C standard library. */
    _stdout_install(__putchar);
    _stdin_install(__getchar);
    _stdpoll_install(__kbhit);
}

/* Halts the system in an unrecoverable state. */
void hal_panic(void)
{
    _di(); /* Disable all interrupts to prevent further execution. */

    /* Attempt a clean shutdown via QEMU 'virt' machine's shutdown device. */
    *(volatile uint32_t *) 0x100000U = 0x5555U;

    /* If shutdown fails, halt the CPU in a low-power state indefinitely. */
    while (1)
        asm volatile("wfi"); /* Wait For Interrupt */
}

/* Puts the CPU into a low-power state until an interrupt occurs. */
void hal_cpu_idle(void)
{
    asm volatile("wfi");
}

/* Interrupt and Trap Handling */

/* C-level trap handler, called by the '_isr' assembly routine.
 * @cause : The value of the 'mcause' CSR, indicating the reason for the
 * trap.
 * @epc   : The value of the 'mepc' CSR, the PC at the time of the trap.
 */
void do_trap(uint32_t cause, uint32_t epc)
{
    static const char *exc_msg[] = {
        /* For printing helpful debug messages. */
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

    if (MCAUSE_IS_INTERRUPT(cause)) { /* Asynchronous Interrupt */
        uint32_t int_code = MCAUSE_GET_CODE(cause);
        if (int_code == MCAUSE_MTI) { /* Machine Timer Interrupt */
            /* To avoid timer drift, schedule the next interrupt relative to the
             * previous target time, not the current time. This ensures a
             * consistent tick frequency even with interrupt latency.
             */
            mtimecmp_w(mtimecmp_r() + (F_CPU / F_TIMER));
            dispatcher(); /* Invoke the OS scheduler. */
        } else {
            /* All other interrupt sources are unexpected and fatal. */
            printf("[UNHANDLED INTERRUPT] code=%u, cause=%08x, epc=%08x\n",
                   int_code, cause, epc);
            hal_panic();
        }
    } else { /* Synchronous Exception */
        uint32_t code = MCAUSE_GET_CODE(cause);
        const char *reason = "Unknown exception";
        if (code < ARRAY_SIZE(exc_msg) && exc_msg[code])
            reason = exc_msg[code];
        printf("[EXCEPTION] code=%u (%s), epc=%08x, cause=%08x\n", code, reason,
               epc, cause);
        hal_panic();
    }
}

/* Enables the machine-level timer interrupt source. */
void hal_timer_enable(void)
{
    mtimecmp_w(mtime_r() + (F_CPU / F_TIMER));
    write_csr(mie, read_csr(mie) | MIE_MTIE);
}

/* Disables the machine-level timer interrupt source. */
void hal_timer_disable(void)
{
    write_csr(mie, read_csr(mie) & ~MIE_MTIE);
}

/* Hook called by the scheduler after a context switch.
 * Its primary purpose is to enable global interrupts ('mstatus.MIE') only
 * AFTER the first task has been launched. This ensures interrupts are not
 * globally enabled until the OS is fully running in a valid task context.
 */
void hal_interrupt_tick(void)
{
    tcb_t *task = get_task_current(kcb)->data;
    if (unlikely(!task))
        hal_panic(); /* Fatal error - invalid task state */

    /* The task's entry point is still in RA, so this is its very first run. */
    if ((uint32_t) task->entry == task->context[CONTEXT_RA])
        _ei(); /* Enable global interrupts now that we are in a task. */
}

/* Context Switching */

/* Saves the current C execution context into a jmp_buf.
 * Returns 0 when called directly.
 */
int32_t setjmp(jmp_buf env)
{
    if (unlikely(!env))
        return -1; /* Invalid parameter */

    asm volatile(
        /* Save all callee-saved registers as required by the RISC-V ABI. */
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
        /* Save essential pointers and the return address. */
        "sw  gp,  12*4(%0)\n"
        "sw  tp,  13*4(%0)\n"
        "sw  sp,  14*4(%0)\n"
        "sw  ra,  15*4(%0)\n"
        /* Save CSRs for debug and context switching. The mstatus register is
         * reconstructed to preserve the pre-trap MIE state, which is essential
         * for preemptive switching.
         */
        "csrr t0, mcause\n"
        "sw   t0,  16*4(%0)\n"
        "csrr t0, mepc\n"
        "sw   t0,  17*4(%0)\n"
        "csrr t0, mstatus\n" /* Read current mstatus (MIE=0 in trap) */
        "srli t1, t0, 4\n"   /* Shift MPIE (bit 7) to bit 3 pos */
        "andi t1, t1, 8\n"   /* Isolate the bit (MSTATUS_MIE) */
        "li   t2, ~8\n"      /* Create mask to clear old MIE bit */
        "and  t0, t0, t2\n"  /* Clear the current MIE bit */
        "or   t0, t0, t1\n"  /* Set MIE to its pre-trap value (from MPIE) */
        "sw   t0,  18*4(%0)\n"
        /* By convention, the initial call to setjmp returns 0. */
        "li a0, 0\n"
        :
        : "r"(env)
        : "t0", "t1", "t2", "memory", "a0");

    /* 'return' is for compiler analysis only. */
    return 0;
}

/* Restores a context saved by 'setjmp'. Never returns to the caller.
 * Execution resumes at the 'setjmp' call site.
 * @env : Pointer to the saved context (must be valid).
 * @val : The value to be returned by 'setjmp' (coerced to 1 if 0).
 */
__attribute__((noreturn)) void longjmp(jmp_buf env, int32_t val)
{
    if (unlikely(!env))
        hal_panic(); /* Cannot proceed with invalid context */

    if (val == 0)
        val = 1; /* 'setjmp' must return a non-zero value after 'longjmp'. */

    asm volatile(
        /* Restore mstatus FIRST. This ensures the interrupt state is correct
         * before restoring any other registers.
         */
        "lw  t0, 18*4(%0)\n"
        "csrw mstatus, t0\n"

        /* Restore all registers from the provided 'jmp_buf'. */
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
        /* Set the return value (in 'a0') for the 'setjmp' call. */
        "mv  a0,  %1\n"
        /* "Return" to the restored 'ra', effectively jumping to new context. */
        "ret\n"
        :
        : "r"(env), "r"(val)
        : "memory");

    __builtin_unreachable(); /* Tell compiler this point is never reached. */
}

/* Low-level context restore helper. Expects a pointer to a 'jmp_buf' in 'a0'.
 * Restores the GPRs and jumps to the restored return address.
 */
static void __attribute__((naked, used)) __dispatch_init(void)
{
    asm volatile(
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
        "lw  ra,  15*4(a0)\n"
        "ret\n"); /* Jump to the task's entry point. */
}

/* Transfers control from the kernel's main thread to the first task. */
__attribute__((noreturn)) void hal_dispatch_init(jmp_buf env)
{
    if (unlikely(!env))
        hal_panic(); /* Cannot proceed without valid context */

    if (kcb->preemptive)
        hal_timer_enable();
    _ei(); /* Enable global interrupts just before launching the first task. */

    asm volatile(
        "mv  a0, %0\n"           /* Move @env (the task's context) into 'a0'. */
        "call __dispatch_init\n" /* Call the low-level restore routine. */
        :
        : "r"(env)
        : "a0", "memory");
    __builtin_unreachable();
}

/* Builds an initial 'jmp_buf' context for a brand-new task.
 * @ctx : Pointer to the 'jmp_buf' to initialize (must be valid).
 * @sp  : Base address of the task's stack (must be valid).
 * @ss  : Total size of the stack in bytes (must be > ISR_STACK_FRAME_SIZE).
 * @ra  : The task's entry point function, used as the initial return address.
 */
void hal_context_init(jmp_buf *ctx, size_t sp, size_t ss, size_t ra)
{
    if (unlikely(!ctx || !sp || ss < (ISR_STACK_FRAME_SIZE + 64) || !ra))
        hal_panic(); /* Invalid parameters - cannot safely initialize context */

    uintptr_t stack_base = (uintptr_t) sp;
    uintptr_t stack_top;

    /* Reserve a "red zone" for the ISR's full trap frame at top of stack. */
    stack_top = (stack_base + ss - ISR_STACK_FRAME_SIZE);

    /* The RISC-V ABI requires the stack pointer to be 16-byte aligned. */
    stack_top &= ~0xFUL;

    /* Verify stack alignment and bounds */
    if (unlikely(stack_top <= stack_base || (stack_top & 0xF) != 0))
        hal_panic(); /* Stack configuration error */

    memset(ctx, 0, sizeof(*ctx)); /* Zero the context for predictability. */

    /* Set the two essential registers for a new task:
     * - SP is set to the prepared top of the task's stack.
     * - RA is set to the task's entry point.
     * When this context is first restored via 'longjmp', the 'ret' instruction
     * will effectively jump to this entry point, starting the task.
     *
     * The mstatus is also initialized to ensure interrupts are enabled for the
     * new task.
     */
    (*ctx)[CONTEXT_SP] = (uint32_t) stack_top;
    (*ctx)[CONTEXT_RA] = (uint32_t) ra;
    (*ctx)[CONTEXT_MSTATUS] = MSTATUS_MIE | MSTATUS_MPP_MACH;
}
