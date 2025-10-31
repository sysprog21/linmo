#pragma once

#include <types.h>

/* Symbols from the linker script, defining memory boundaries */
extern uint32_t _stext, _etext; /* Start/end of the .text section */
extern uint32_t _sidata;        /* Start address for .data initialization */
extern uint32_t _sdata, _edata; /* Start/end address for .data section */
extern uint32_t _sbss, _ebss;   /* Start/end address for .bss section */
extern uint32_t _end;           /* End of kernel image */
extern uint32_t _heap_start, _heap_end;     /* Start/end of the HEAP memory */
extern uint32_t _heap_size;                 /* Size of HEAP memory */
extern uint32_t _stack_bottom, _stack_top;  /* Bottom/top of the STACK memory */

/* Read a RISC-V Control and Status Register (CSR).
 * @reg : The symbolic name of the CSR (e.g., mstatus).
 */
#define read_csr(reg)                                 \
    ({                                                \
        uint32_t __tmp;                               \
        asm volatile("csrr %0, " #reg : "=r"(__tmp)); \
        __tmp;                                        \
    })

/* Write a value to a RISC-V CSR.
 * @reg : The symbolic name of the CSR.
 * @val : The 32-bit value to write.
 */
#define write_csr(reg, val) ({ asm volatile("csrw " #reg ", %0" ::"rK"(val)); })

/* Globally enable or disable machine-level interrupts by setting mstatus.MIE.
 * @enable : Non-zero to enable, zero to disable.
 * Returns the previous state of the interrupt enable bit (1 if enabled, 0 if
 * disabled).
 */
static inline int32_t hal_interrupt_set(int32_t enable)
{
    uint32_t mstatus_val = read_csr(mstatus);
    uint32_t mie_bit = (1U << 3); /* MSTATUS_MIE bit position */

    if (enable) {
        write_csr(mstatus, mstatus_val | mie_bit);
    } else {
        write_csr(mstatus, mstatus_val & ~mie_bit);
    }
    return (int32_t) ((mstatus_val >> 3) & 1);
}

/* Convenience macros for interrupt control */
#define _di() hal_interrupt_set(0) /* Disable global interrupts */
#define _ei() hal_interrupt_set(1) /* Enable global interrupts */

/* Context buffer for task switching. Contains execution context and space
 * for processor state management. The standard C library functions use only
 * the execution context portion, while HAL context switching routines manage
 * the complete context including processor state.
 *
 * Memory layout (17 x 32-bit words):
 * [0-11]:  s0-s11 (callee-saved registers)
 * [12]:    gp (global pointer)
 * [13]:    tp (thread pointer)
 * [14]:    sp (stack pointer)
 * [15]:    ra (return address)
 * [16]:    mstatus (processor state)
 */
typedef uint32_t jmp_buf[17];

/* Standard C library context-switching primitives.
 * These follow standard C semantics and handle only execution context
 * (elements 0-15 of jmp_buf). Processor state is not managed.
 */
int32_t setjmp(jmp_buf env);
void longjmp(jmp_buf env, int32_t val);

/* HAL context switching routines for complete context management */
int32_t hal_context_save(jmp_buf env);
void hal_context_restore(jmp_buf env, int32_t val);
void hal_dispatch_init(jmp_buf env);

/* Provides a blocking, busy-wait delay.
 * This function monopolizes the CPU and should only be used for very short
 * delays or in pre-scheduling initialization code.
 * @msec : The number of milliseconds to wait.
 */
void delay_ms(uint32_t msec);

/* Reads the system's high-resolution timer.
 * Returns the number of microseconds since boot.
 */
uint64_t _read_us(void);

/* Hardware Abstraction Layer (HAL) initialization and control functions */
void hal_hardware_init(void);
void hal_timer_enable(void);
void hal_timer_disable(void);
void hal_interrupt_tick(void);

/* Initializes the context structure for a new task.
 * @ctx : Pointer to jmp_buf to initialize (must be non-NULL).
 * @sp  : Base address of the task's stack (must be valid).
 * @ss  : Total size of the stack in bytes (must be >= MIN_STACK_SIZE).
 * @ra  : The task's entry point function (must be non-NULL).
 */
void hal_context_init(jmp_buf *ctx, size_t sp, size_t ss, size_t ra);

/* Halts the CPU in an unrecoverable error state, shutting down if possible */
void hal_panic(void);

/* Puts the CPU into a low-power wait-for-interrupt state */
void hal_cpu_idle(void);

/* Default stack size for new tasks if not otherwise specified */
#define DEFAULT_STACK_SIZE 4096

/* Physical Memory Protection (PMP) region limit constants */
#define PMP_MAX_REGIONS 16     /* RISC-V supports 16 PMP regions */
#define PMP_TOR_PAIRS 8        /* In TOR mode, 16 regions = 8 pairs (uses 2 addrs each) */
#define MIN_PMP_REGION_SIZE 4  /* Minimum addressable size in TOR mode (4 bytes) */
