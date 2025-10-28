/* RISC-V CSR (Control and Status Register) bit definitions.
 *
 * This file centralizes all bitfield definitions for RISC-V CSRs used by the
 * HAL. All definitions follow the RISC-V privileged specification.
 */

#pragma once

/* mstatus Register (Machine Status Register) */

/* Machine Interrupt Enable bit: Controls global interrupt enable/disable in
 * M-mode.
 */
#define MSTATUS_MIE (1U << 3)

/* Previous Interrupt Enable bit: Value of MIE before entering trap. */
#define MSTATUS_MPIE (1U << 7)

/* Previous Privilege Mode bits: Indicates the privilege mode before a trap.
 * 3: Machine Mode, 2: Reserved, 1: Supervisor Mode, 0: User Mode.
 */
#define MSTATUS_MPP_SHIFT 11
#define MSTATUS_MPP_MASK (3U << MSTATUS_MPP_SHIFT)
#define MSTATUS_MPP_USER (0U << MSTATUS_MPP_SHIFT)  /* User mode */
#define MSTATUS_MPP_SUPER (1U << MSTATUS_MPP_SHIFT) /* Supervisor mode */
#define MSTATUS_MPP_MACH (3U << MSTATUS_MPP_SHIFT)  /* Machine mode */

/* Utility macros for mstatus manipulation */
#define MSTATUS_GET_MPP(m) (((m) & MSTATUS_MPP_MASK) >> MSTATUS_MPP_SHIFT)
#define MSTATUS_SET_MPP(m, mode) \
    (((m) & ~MSTATUS_MPP_MASK) | ((mode) << MSTATUS_MPP_SHIFT))

/* mie Register (Machine Interrupt Enable Register) */

/* Machine Software Interrupt Enable: Enables software interrupts in M-mode. */
#define MIE_MSIE (1U << 3)

/* Machine Timer Interrupt Enable: Enables timer interrupts in M-mode. */
#define MIE_MTIE (1U << 7)

/* Machine External Interrupt Enable: Enables external interrupts in M-mode. */
#define MIE_MEIE (1U << 11)

/* Convenience macro for common interrupt enable combinations */
#define MIE_ALL_ENABLED (MIE_MSIE | MIE_MTIE | MIE_MEIE)

/* mip Register (Machine Interrupt Pending Register) */

/* Machine Software Interrupt Pending */
#define MIP_MSIP (1U << 3)

/* Machine Timer Interrupt Pending */
#define MIP_MTIP (1U << 7)

/* Machine External Interrupt Pending */
#define MIP_MEIP (1U << 11)

/* mcause Register (Machine Trap Cause Register)
 *
 * 31    30                           0
 * +-----+-----------------------------+
 * | INT |         Exception Code      |
 * +-----+-----------------------------+
 *
 * Bit 31 (INT): Interrupt flag
 *   1 = This was an interrupt (asynchronous)
 *   0 = This was an exception (synchronous)
 * Bits 30-0: The actual cause code
 *   For interrupts: timer, external, software interrupt types
 *   For exceptions: illegal instruction, page fault, etc.
 */

/* If this bit is set in 'mcause', the trap was an interrupt. */
#define MCAUSE_INT (1U << 31)

/* Masks lower bits of 'mcause' to extract the interrupt or exception code. */
#define MCAUSE_CODE_MASK (0x7FFFFFFF)

/* Utility macros for mcause analysis */
#define MCAUSE_IS_INTERRUPT(cause) ((cause) & MCAUSE_INT)
#define MCAUSE_IS_EXCEPTION(cause) (!MCAUSE_IS_INTERRUPT(cause))
#define MCAUSE_GET_CODE(cause) ((cause) & MCAUSE_CODE_MASK)

/* Standard RISC-V Interrupt Cause Codes (when MCAUSE_INT is set) */

/* Machine Software Interrupt */
#define MCAUSE_MSI 0x3

/* Machine Timer Interrupt - A common interrupt source for scheduling. */
#define MCAUSE_MTI 0x7

/* Machine External Interrupt */
#define MCAUSE_MEI 0xb

/* Standard RISC-V Exception Cause Codes (when MCAUSE_INT is clear) */

/* Instruction address misaligned */
#define MCAUSE_INST_ADDR_MISALIGNED 0x0

/* Instruction access fault */
#define MCAUSE_INST_ACCESS_FAULT 0x1

/* Illegal instruction */
#define MCAUSE_ILLEGAL_INST 0x2

/* Breakpoint */
#define MCAUSE_BREAKPOINT 0x3

/* Load address misaligned */
#define MCAUSE_LOAD_ADDR_MISALIGNED 0x4

/* Load access fault */
#define MCAUSE_LOAD_ACCESS_FAULT 0x5

/* Store/AMO address misaligned */
#define MCAUSE_STORE_ADDR_MISALIGNED 0x6

/* Store/AMO access fault */
#define MCAUSE_STORE_ACCESS_FAULT 0x7

/* Environment call from U-mode */
#define MCAUSE_ECALL_UMODE 0x8

/* Environment call from S-mode */
#define MCAUSE_ECALL_SMODE 0x9

/* Environment call from M-mode */
#define MCAUSE_ECALL_MMODE 0xb

/* Instruction page fault */
#define MCAUSE_INST_PAGE_FAULT 0xc

/* Load page fault */
#define MCAUSE_LOAD_PAGE_FAULT 0xd

/* Store/AMO page fault */
#define MCAUSE_STORE_PAGE_FAULT 0xf

/* mtvec Register (Machine Trap Vector Register) */

/* Trap vector mode bits */
#define MTVEC_MODE_MASK 0x3
#define MTVEC_MODE_DIRECT 0x0   /* All traps go to BASE address */
#define MTVEC_MODE_VECTORED 0x1 /* Interrupts go to BASE + 4*cause */

/* Extract base address from mtvec (clear mode bits) */
#define MTVEC_GET_BASE(mtvec) ((mtvec) & ~MTVEC_MODE_MASK)

/* Set mtvec with base address and mode */
#define MTVEC_SET(base, mode) \
    (((base) & ~MTVEC_MODE_MASK) | ((mode) & MTVEC_MODE_MASK))

/* Safety and Validation Macros */

/* Validate that a privilege mode value is legal */
#define IS_VALID_PRIV_MODE(mode) ((mode) == 0 || (mode) == 1 || (mode) == 3)

/* Check if a cause code represents a valid interrupt */
#define IS_VALID_INTERRUPT_CODE(code) \
    ((code) == MCAUSE_MSI || (code) == MCAUSE_MTI || (code) == MCAUSE_MEI)

/* Check if a cause code represents a standard exception */
#define IS_STANDARD_EXCEPTION_CODE(code) \
    ((code) <= MCAUSE_STORE_PAGE_FAULT && (code) != 0xa && (code) != 0xe)

/* Additional Machine-Mode CSRs */

/* Machine Vendor ID - Read-only identification */
#define CSR_MVENDORID 0xf11

/* Machine Architecture ID - Read-only identification */
#define CSR_MARCHID 0xf12

/* Machine Implementation ID - Read-only identification */
#define CSR_MIMPID 0xf13

/* Hart ID - Read-only hart identifier */
#define CSR_MHARTID 0xf14

/* Machine Scratch Register - For temporary storage during traps */
#define CSR_MSCRATCH 0x340

/* PMP Address Registers (pmpaddr0-pmpaddr15) - 16 regions maximum
 * In TOR (Top-of-Range) mode, these define the upper boundary of each region.
 * The lower boundary is defined by the previous region's upper boundary.
 */
#define CSR_PMPADDR0 0x3b0
#define CSR_PMPADDR1 0x3b1
#define CSR_PMPADDR2 0x3b2
#define CSR_PMPADDR3 0x3b3
#define CSR_PMPADDR4 0x3b4
#define CSR_PMPADDR5 0x3b5
#define CSR_PMPADDR6 0x3b6
#define CSR_PMPADDR7 0x3b7
#define CSR_PMPADDR8 0x3b8
#define CSR_PMPADDR9 0x3b9
#define CSR_PMPADDR10 0x3ba
#define CSR_PMPADDR11 0x3bb
#define CSR_PMPADDR12 0x3bc
#define CSR_PMPADDR13 0x3bd
#define CSR_PMPADDR14 0x3be
#define CSR_PMPADDR15 0x3bf

/* PMP Configuration Registers (pmpcfg0-pmpcfg3)
 * Each configuration register controls 4 PMP regions (on RV32).
 * pmpcfg0 controls pmpaddr0-3, pmpcfg1 controls pmpaddr4-7, etc.
 */
#define CSR_PMPCFG0 0x3a0
#define CSR_PMPCFG1 0x3a1
#define CSR_PMPCFG2 0x3a2
#define CSR_PMPCFG3 0x3a3

/* PMP Configuration Field Bits (8 bits per region within pmpcfg)
 * Layout in each byte of pmpcfg:
 * Bit 7:     L (Lock) - Locks this region until hardware reset
 * Bits 6-5:  Reserved
 * Bits 4-3:  A (Address Matching Mode)
 * Bit 2:     X (Execute permission)
 * Bit 1:     W (Write permission)
 * Bit 0:     R (Read permission)
 */

/* Lock bit: Prevents further modification of this region */
#define PMPCFG_L (1U << 7)

/* Address Matching Mode (bits 3-4)
 * Choose TOR mode for no alignment requirements on region sizes, and support
 * for arbitrary address ranges.
 */
#define PMPCFG_A_SHIFT 3
#define PMPCFG_A_MASK (0x3U << PMPCFG_A_SHIFT)
#define PMPCFG_A_OFF (0x0U << PMPCFG_A_SHIFT) /* Null region (disabled) */
#define PMPCFG_A_TOR (0x1U << PMPCFG_A_SHIFT) /* Top-of-Range mode */

/* Permission bits */
#define PMPCFG_X (1U << 2) /* Execute permission */
#define PMPCFG_W (1U << 1) /* Write permission */
#define PMPCFG_R (1U << 0) /* Read permission */

/* Common permission combinations */
#define PMPCFG_PERM_NONE (0x0U)                          /* No access */
#define PMPCFG_PERM_R (PMPCFG_R)                         /* Read-only */
#define PMPCFG_PERM_RW (PMPCFG_R | PMPCFG_W)             /* Read-Write */
#define PMPCFG_PERM_X (PMPCFG_X)                         /* Execute-only */
#define PMPCFG_PERM_RX (PMPCFG_R | PMPCFG_X)             /* Read-Execute */
#define PMPCFG_PERM_RWX (PMPCFG_R | PMPCFG_W | PMPCFG_X) /* All access */

/* Utility macros for PMP configuration manipulation */

/* Extract PMP address matching mode */
#define PMPCFG_GET_A(cfg) (((cfg) & PMPCFG_A_MASK) >> PMPCFG_A_SHIFT)

/* Extract permission bits from configuration byte */
#define PMPCFG_GET_PERM(cfg) ((cfg) & (PMPCFG_R | PMPCFG_W | PMPCFG_X))

/* Check if region is locked */
#define PMPCFG_IS_LOCKED(cfg) (((cfg) & PMPCFG_L) != 0)

/* Check if region is enabled (address mode is not OFF) */
#define PMPCFG_IS_ENABLED(cfg) (PMPCFG_GET_A(cfg) != PMPCFG_A_OFF)
