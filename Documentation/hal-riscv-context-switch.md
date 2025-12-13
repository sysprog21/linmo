# HAL: Context Switching for RISC-V

## Context Switching
Context switching is essential to Linmo's preemptive multitasking kernel,
facilitating smooth task transitions.
In Linmo, context switching is implemented through a clean separation of concerns architecture that combines the portability of standard C library functions with the performance requirements of real-time systems.
This approach provides both `setjmp` and `longjmp` functions following standard C library semantics for application use,
and dedicated HAL routines (`hal_context_save` and `hal_context_restore`) for optimized kernel scheduling.

## Separation of Concerns Architecture
The context switching implementation follows a clean layered approach that separates execution context management from processor state management:

### Standard C Library Layer
Portable, standards-compliant context switching for applications
- `setjmp` - Saves execution context only (elements 0-15)
- `longjmp` - Restores execution context only
- Semantics: Pure C library behavior, no processor state management
- Use Cases: Exception handling, coroutines, application-level control flow
- Performance: Standard overhead, optimized for portability

### HAL Context Switching Layer
Context switching for kernel scheduling
- `hal_context_save` - Saves execution context AND processor state
- `hal_context_restore` - Restores complete task state
- Semantics: System-level optimization with interrupt state management
- Use Cases: Preemptive scheduling, cooperative task switching
- Performance: Optimized for minimal overhead

### Unified Context Buffer
Both layers use the same `jmp_buf` structure but access different portions:

```c
typedef uint32_t jmp_buf[17];

/* Layout:
 * [0-11]:  s0-s11 (callee-saved registers) - both layers
 * [12]:    gp (global pointer) - both layers
 * [13]:    tp (thread pointer) - both layers
 * [14]:    sp (stack pointer) - both layers
 * [15]:    ra (return address) - both layers
 * [16]:    mstatus (processor state) - HAL layer only
 */
```

## Context Switch Process

### 1. Save Current Task State
The `hal_context_save` function captures complete task state including both execution context and processor state.
The function saves all callee-saved registers as required by the RISC-V ABI,
plus essential pointers (gp, tp, sp, ra).
For processor state, it saves `mstatus` as-is, preserving the exact processor state at the time of the context switch.
The RISC-V hardware automatically manages the `mstatus.MIE` and `mstatus.MPIE` stack during trap entry and `MRET`,
ensuring correct interrupt state restoration per the RISC-V Privileged Specification.

### 2. Select Next Task
The scheduler, invoked via `dispatcher()` during machine timer interrupts,
uses a priority-based round-robin algorithm or user-defined scheduler to select the next ready task.
The scheduling logic evaluates task priorities and readiness states to ensure optimal system responsiveness.

### 3. Restore Next Task State
The `hal_context_restore` function performs complete state restoration with processor state restored first to establish correct execution environment:

```c
lw  t0, 16*4(%0)        // Load saved mstatus from jmp_buf[16]
csrw mstatus, t0        // Restore processor state FIRST
// ... then restore all execution context registers
```

This ordering ensures that interrupt state and privilege mode are correctly established before resuming task execution.

## Processor State Management

### Hardware-Managed Interrupt State
The HAL context switching routines follow the RISC-V Privileged Specification for interrupt state management,
allowing hardware to automatically manage the interrupt enable stack:

During Trap Entry (Hardware Automatic per RISC-V Spec §3.1.6.1):
- `mstatus.MPIE ← mstatus.MIE` (preserve interrupt enable state)
- `mstatus.MIE ← 0` (disable interrupts during trap handling)
- `mstatus.MPP ← current_privilege` (preserve privilege mode)

During MRET (Hardware Automatic):
- `mstatus.MIE ← mstatus.MPIE` (restore interrupt enable state)
- `mstatus.MPIE ← 1` (reset to default enabled)
- `privilege ← mstatus.MPP` (return to saved privilege)

HAL Context Switch Behavior:
- `hal_context_save` saves `mstatus` exactly as observed (no manual bit manipulation)
- `hal_context_restore` restores `mstatus` exactly as saved
- Hardware manages the `MIE`/`MPIE` stack automatically during nested traps
- This ensures spec-compliant behavior and correct interrupt state across all scenarios

State Preservation:
- Each task maintains its own complete `mstatus` value
- Context switches preserve privilege mode (Machine mode for kernel tasks)
- Nested interrupts are handled correctly by hardware's automatic state stacking

### Task Initialization
Task initialization differs between cooperative and preemptive modes due to
their distinct context management approaches.

In cooperative mode, tasks use lightweight context structures for voluntary
yielding. New tasks are initialized with execution context only:

```c
void hal_context_init(jmp_buf *ctx, size_t sp, size_t ss, size_t ra)
{
    /* Set execution context */
    (*ctx)[CONTEXT_SP] = (uint32_t) stack_top;  // Stack pointer
    (*ctx)[CONTEXT_RA] = (uint32_t) ra;         // Entry point
    /* Set processor state */
    (*ctx)[CONTEXT_MSTATUS] = MSTATUS_MIE | MSTATUS_MPP_MACH;
}
```

This lightweight approach uses standard calling conventions where tasks
return control through normal function returns.

Preemptive mode requires interrupt frame structures to support trap-based
context switching and privilege mode transitions. Task initialization builds
a complete interrupt service routine frame:

```c
void *hal_build_initial_frame(void *stack_top,
                              void (*task_entry)(void),
                              int user_mode)
{
    /* Place frame in stack with initial reserve below for proper startup */
    uint32_t *frame = (uint32_t *) ((uint8_t *) stack_top - 256 -
                                    ISR_STACK_FRAME_SIZE);

    /* Initialize all general purpose registers to zero */
    for (int i = 0; i < 36; i++)
        frame[i] = 0;

    /* Compute thread pointer: aligned to 64 bytes from _end */
    uint32_t tp_val = ((uint32_t) &_end + 63) & ~63U;

    /* Set essential pointers */
    frame[FRAME_GP] = (uint32_t) &_gp;  /* Global pointer */
    frame[FRAME_TP] = tp_val;            /* Thread pointer */

    /* Configure processor state for task entry:
     * - MPIE=1: Interrupts will enable when task starts
     * - MPP: Target privilege level (user or machine mode)
     * - MIE=0: Keep interrupts disabled during frame restoration
     */
    uint32_t mstatus_val =
        MSTATUS_MPIE | (user_mode ? MSTATUS_MPP_USER : MSTATUS_MPP_MACH);
    frame[FRAME_MSTATUS] = mstatus_val;

    /* Set entry point */
    frame[FRAME_EPC] = (uint32_t) task_entry;

    return frame;  /* Return frame base as initial stack pointer */
}
```

The interrupt frame layout reserves space for all register state, control
registers, and alignment padding. When the scheduler first dispatches this
task, the trap return mechanism restores the frame and transfers control to
the entry point with the configured privilege level.

Key differences from cooperative mode include full register state allocation
rather than minimal callee-saved registers, trap return semantics rather than
function return, support for privilege level transitions through MPP
configuration, and proper interrupt state initialization through MPIE bit.

## Implementation Details

### Kernel Integration
The kernel scheduler uses the context switching routine:

```c
/* Preemptive context switching */
void dispatch(void)
{
    /* Save current task with processor state */
    if (hal_context_save(current_task->context) != 0)
        return;  /* Restored from context switch */
 
    /* ... scheduling logic ... */

    /* Restore next task with processor state*/
    hal_context_restore(next_task->context, 1);
}
```

## Best Practices

### Architecture Principles
- Layer Separation: Keep application and system contexts separate
- Standard Compliance: Use standard functions for portable code
- Performance Optimization: Use HAL functions for time-critical system code
- State Management: Let HAL functions handle processor state automatically

### Development Guidelines
- Application Code: Always use `setjmp` and `longjmp` for exception handling
- System Code: Always use `hal_context_save` and `hal_context_restore` for scheduling
- Mixed Use: Both can operate on the same `jmp_buf` without interference
- Testing: Verify interrupt state preservation across context switches

### Interrupt State Machinery
RISC-V Interrupt Behavior During Traps:
1. Hardware automatically clears `mstatus.MIE` on trap entry
2. Previous interrupt state saved in `mstatus.MPIE`
3. Privilege level preserved in `mstatus.MPP`
4. HAL functions reconstruct original interrupt state for task resumption

State Reconstruction Logic:
```
Original MIE state = Current MPIE bit
Reconstructed mstatus = (current_mstatus & ~MIE) | (MPIE >> 4)
```

This ensures tasks resume with their original interrupt enable state rather than the disabled state from trap entry.

### Task State Lifecycle
New Task Creation:
1. `hal_context_init` sets up initial execution context
2. Stack pointer positioned with ISR frame reservation
3. Return address points to task entry function
4. Processor state initialized with interrupts enabled

First Task Launch:

**Cooperative Mode**:
1. `hal_dispatch_init` receives lightweight context structure
2. Global interrupts enabled just before task execution
3. Control transfers to first task through standard function call
4. Task begins execution and voluntarily yields control

**Preemptive Mode**:
1. `hal_dispatch_init` receives interrupt frame pointer
2. Timer interrupt enabled for periodic preemption
3. Dispatcher loads frame and executes trap return instruction
4. Hardware restores registers and transitions to configured privilege level
5. Task begins execution and can be preempted by timer

Context Switch Cycle:
1. Timer interrupt triggers scheduler entry
2. `hal_context_save` preserves complete current task state
3. Scheduler selects next ready task based on priority
4. `hal_context_restore` resumes selected task execution
5. Task continues from its previous suspension point
