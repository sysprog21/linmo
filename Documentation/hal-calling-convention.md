# HAL: RISC-V Calling Convention

## Overview
Linmo kernel strictly adheres to the RISC-V calling convention for RV32I architecture.
This document describes how the standard calling convention is implemented and extended within the Linmo kernel context,
particularly for context switching, interrupt handling, and task management.

## C Datatypes and Alignment (RV32I)

| C type        | Description              | Bytes | Alignment |
| ------------- | ------------------------ | ----- | --------- |
| `char`        | Character value/byte     | 1     | 1         |
| `short`       | Short integer            | 2     | 2         |
| `int`         | Integer                  | 4     | 4         |
| `long`        | Long integer             | 4     | 4         |
| `long long`   | Long long integer        | 8     | 8         |
| `void*`       | Pointer                  | 4     | 4         |
| `float`       | Single-precision float   | 4     | 4         |
| `double`      | Double-precision float   | 8     | 8         |

Note: Linmo currently targets RV32I (integer-only) and does not use floating-point instructions.

## Register Usage in Linmo

### Standard Register Classifications

#### Caller-Saved Registers (Volatile)
These registers may be modified by function calls and must be saved by the caller if needed across calls:

| Register | ABI Name | Description                      | Linmo Usage |
| -------- | -------- | -------------------------------- | ----------- |
| x1       | `ra`     | Return address                   | Function returns, ISR context |
| x5–7     | `t0–2`   | Temporaries                      | Scratch registers |
| x10–11   | `a0–1`   | Function arguments/return values | Syscall args/returns |
| x12–17   | `a2–7`   | Function arguments               | Extended syscall args |
| x28–31   | `t3–6`   | Temporaries                      | Scratch registers |

#### Callee-Saved Registers (Non-Volatile)
These registers must be preserved across function calls:

| Register | ABI Name | Description                  | Linmo Usage |
| -------- | -------- | ---------------------------- | ----------- |
| x2       | `sp`     | Stack pointer                | Task stack management |
| x8       | `s0/fp`  | Saved register/frame pointer | General purpose |
| x9       | `s1`     | Saved register               | General purpose |
| x18–27   | `s2–11`  | Saved registers              | General purpose |

#### Special Registers
| Register | ABI Name | Description     | Linmo Usage |
| -------- | -------- | --------------- | ----------- |
| x0       | `zero`   | Hard-wired zero | Constant zero |
| x3       | `gp`     | Global pointer  | Kernel globals access |
| x4       | `tp`     | Thread pointer  | Thread-local storage |

## Context Switching in Linmo

### Task Context Structure (`jmp_buf`)

Linmo's `jmp_buf` stores the minimal context required for task switching:

```c
typedef uint32_t jmp_buf[17];
```

Layout (32-bit word indices):
```
[0-11]:  s0-s11    (Callee-saved registers)
[12]:    gp        (Global pointer) 
[13]:    tp        (Thread pointer)
[14]:    sp        (Stack pointer)
[15]:    ra        (Return address)
[16]:    mstatus   (Machine status CSR)
```

### Why Only Callee-Saved Registers?

The RISC-V calling convention guarantees that:
- Callee-saved registers (`s0-s11`, `sp`) are preserved across function calls
- Caller-saved registers (`t0-t6`, `a0-a7`, `ra`) may be modified by callees

Since task switches occur at well-defined points (yield, block, preemption), we only need to save callee-saved registers plus essential control state. Caller-saved registers are either:
- Already saved by the compiler before function calls
- Not significant at task switch boundaries

### Context Switching Functions

#### Standard C Library Functions
```c
int32_t setjmp(jmp_buf env);         /* Save execution context only */
void longjmp(jmp_buf env, int32_t val); /* Restore execution context only */
```
- Use elements [0-15] of `jmp_buf`
- Standard C semantics for non-local jumps
- No processor state management

#### HAL Context Switching Functions  
```c
int32_t hal_context_save(jmp_buf env);           /* Save context + processor state */
void hal_context_restore(jmp_buf env, int32_t val); /* Restore context + processor state */
```
- Use all elements [0-16] of `jmp_buf`
- Include `mstatus` for interrupt state preservation
- Used by the kernel scheduler for task switching

## Interrupt Service Routine (ISR) Context

### Full Context Preservation

The ISR in `boot.c` performs a complete context save of all registers:

```
Stack Frame Layout (144 bytes, 36 words × 4 bytes, offsets from sp):
  0: ra,   4: gp,   8: tp,  12: t0,  16: t1,  20: t2
 24: s0,  28: s1,  32: a0,  36: a1,  40: a2,  44: a3
 48: a4,  52: a5,  56: a6,  60: a7,  64: s2,  68: s3
 72: s4,  76: s5,  80: s6,  84: s7,  88: s8,  92: s9
 96: s10, 100:s11, 104:t3, 108: t4, 112: t5, 116: t6
120: mcause, 124: mepc, 128: mstatus, 132: sp (for restore)
136-143: padding (8 bytes for 16-byte alignment)
```

Why full context save in ISR?
- ISRs can preempt tasks at any instruction boundary
- Caller-saved registers may contain live values not yet spilled by compiler
- Ensures ISR can call any C function without corrupting task state
- Provides complete transparency to interrupted code

### ISR Stack Requirements

Each task stack must reserve space for the ISR frame:
```c
#define ISR_STACK_FRAME_SIZE 144  /* 36 words × 4 bytes, 16-byte aligned */
```

This "red zone" is reserved at the top of every task stack to guarantee ISR safety.

## Function Calling in Linmo

### Kernel Function Calls

Standard RISC-V calling convention applies:

```c
/* Example: mo_task_spawn(entry, stack_size) */
/* a0 = entry, a1 = stack_size, return value in a0 */
int32_t result = mo_task_spawn(task_function, 2048);
```

### System Call Interface

Linmo provides system calls through the RISC-V trap mechanism for privilege
boundary crossing. User mode tasks invoke system calls using the environment
call instruction, which triggers a synchronous exception handled by the kernel.

System call convention:
- Arguments passed in `a0-a7` registers before trap
- System call number in `a7` register
- Trap handler preserves all registers except return value
- Return value delivered in `a0` register after trap return
- Standard RISC-V calling convention maintained across privilege boundary

The trap-based interface allows user mode tasks to safely access kernel
services without requiring privileged instruction execution. The kernel
validates all parameters and mediates access to protected resources.

### Task Entry Points

When a new task starts:
```c
void task_function(void) {
    /* ra contains this function address */
    /* sp points to task's stack (16-byte aligned) */
    /* gp points to kernel globals */
    /* tp points to thread-local storage area */
    
    /* Task code here */
}
```

## Stack Management

### Stack Layout

Each task has its own stack with this layout:

```
High Address
+------------------+ <- stack_base + stack_size
| ISR Red Zone     | <- 144 bytes reserved for ISR
| (144 bytes)      |
+------------------+ <- Initial SP (16-byte aligned)
|                  |
| Task Stack       | <- Grows downward
| (Dynamic)        |
|                  |
+------------------+ <- stack_base
Low Address
```

### Stack Alignment
- 16-byte alignment: Required by RISC-V ABI for stack pointer
- 4-byte alignment: Minimum for all memory accesses on RV32I
- Stack grows downward (towards lower addresses)

### Stack Protection
When `CONFIG_STACK_PROTECTION` is enabled:
```c
#define STACK_CANARY 0x33333333U

/* Canaries placed at stack boundaries */
*(uint32_t *)stack_base = STACK_CANARY;                    /* Low guard */
*(uint32_t *)(stack_base + stack_size - 4) = STACK_CANARY; /* High guard */
```

## Assembly Function Interface

### Calling Assembly from C
```c
/* C declaration */
extern void assembly_function(uint32_t arg1, uint32_t arg2);

/* Assembly implementation must follow RISC-V ABI */
```

### Calling C from Assembly
```assembly
.globl assembly_calls_c
assembly_calls_c:
    # Arguments already in a0, a1 per calling convention
    call    c_function          # Standard call
    # Return value now in a0
    ret
```

### Naked Functions
For low-level kernel functions that manage their own prologue/epilogue:

```c
__attribute__((naked)) void _isr(void) {
    asm volatile(
        /* Manual register save/restore */
        "addi sp, sp, -128\n"
        /* ... save registers ... */
        "call do_trap\n"
        /* ... restore registers ... */
        "addi sp, sp, 128\n"
        "mret\n"
    );
}
```

## Performance Considerations

### Register Pressure
- Callee-saved registers: `s0-s11` (12 registers) - preserved across calls
- Caller-saved registers: `t0-t6`, `a0-a7`, `ra` (15 registers) - may be used freely

The abundance of caller-saved registers in RISC-V reduces spill pressure compared to architectures like x86.

### Context Switch Cost
Minimal context (jmp_buf):
- 17 × 32-bit loads/stores = 68 bytes
- Essential for cooperative scheduling

Full context (ISR):
- 33 × 32-bit loads/stores = 144 bytes (includes padding for alignment)
- Required for preemptive interrupts

### Function Call Overhead
Standard RISC-V function call:
```assembly
call    function    # 1 instruction (may expand to 2)
# ... function body ...
ret                 # 1 instruction
```

Minimal overhead due to dedicated return address register (`ra`).

## Debugging and Stack Traces

### Stack Frame Walking
With frame pointer enabled (`-fno-omit-frame-pointer`):
```c
void print_stack_trace(void) {
    uint32_t *fp = (uint32_t *)read_csr_s0();  /* s0 = frame pointer */
    
    while (fp) {
        uint32_t ra = *(fp - 1);     /* Return address */
        uint32_t prev_fp = *(fp - 2); /* Previous frame pointer */
        
        printf("PC: 0x%08x\n", ra);
        fp = (uint32_t *)prev_fp;
    }
}
```

### Register Dump in Panic
```c
void panic_dump_context(void) {
    /* ISR context is available on stack during trap handling */
    printf("ra=0x%08x sp=0x%08x gp=0x%08x tp=0x%08x\n", 
           saved_ra, saved_sp, saved_gp, saved_tp);
    /* ... dump all saved registers ... */
}
```

## Compliance and Validation

### ABI Compliance Checks
- GCC validation: Use `-mabi=ilp32` for RV32I
- Stack alignment: Verified at task creation and context switches  
- Register preservation: Validated by context switching tests
- Calling convention: Ensured by compiler and manual assembly review

### Testing
```c
/* Validate calling convention compliance */
void test_calling_convention(void) {
    /* Call functions with various argument patterns */
    test_no_args();
    test_scalar_args(1, 2, 3, 4, 5, 6, 7, 8, 9);  /* > 8 args use stack */
    test_return_values();
    test_callee_saved_preservation();
}
```

## References
- [RISC-V Calling Convention Specification](https://riscv.org/wp-content/uploads/2024/12/riscv-calling.pdf)
