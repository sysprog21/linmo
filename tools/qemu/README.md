# QEMU Trace Profiling

This directory contains a QEMU TCG plugin used to profile the Linmo tracing hot path without modifying kernel behavior.

## Scope

The current plugin targets `debug_trace_event()` and reports:

- `insn_count`
- `store_count`

These numbers are intended for QEMU-based relative analysis only. They are not hardware-cycle measurements.

## Files

- `debug_trace_profiler.c`
  - QEMU TCG plugin that counts guest instructions and stores within a target PC range
- `Makefile`
  - builds `debug_trace_profiler.so`

## Prerequisites

- QEMU system emulator with plugin support
- `qemu-plugin.h`
- `cc`
- `pkg-config`
- `glib-2.0` development headers visible to `pkg-config`

In the current environment, the plugin header is provided through:

```bash
QEMU_PLUGIN_INC=/path/to/qemu/include
```

## Build

From the Linmo repo root, export the environment first:

```bash
export CROSS_COMPILE=<your-riscv-tool-prefix>
export QEMU_PLUGIN_INC=/path/to/qemu/include
export QEMU_BIN=$(command -v qemu-system-riscv32)
export TRACE_PLUGIN_SO="$(pwd)/tools/qemu/debug_trace_profiler.so"
```

Then build the plugin:

```bash
make -C tools/qemu clean
make -C tools/qemu QEMU_PLUGIN_INC="$QEMU_PLUGIN_INC"
```

This produces:

```text
tools/qemu/debug_trace_profiler.so
```

## Reproduction

### 1. Build the target workload

For the bounded scheduler-tracing workload:

```bash
make trace_sched CROSS_COMPILE="$CROSS_COMPILE"
```

If your toolchain binaries are not already on `PATH`, export that before running the commands above.

### 2. Resolve the `debug_trace_event()` address range

```bash
"${CROSS_COMPILE}nm" -S --defined-only build/image.elf | \
    rg ' debug_trace_event$'
```

Example output:

```text
80004a54 000000f0 T debug_trace_event
```

Interpret that as:

- `start = 0x80004a54`
- `size = 0x000000f0`
- `end = 0x80004b44`

### 3. Run QEMU with `icount` and the plugin

```bash
"$QEMU_BIN" \
    -machine virt -nographic -bios none \
    -kernel build/image.elf \
    -icount shift=0,align=off,sleep=off \
    -plugin "$TRACE_PLUGIN_SO",start=0x80004a54,end=0x80004b44
```

Expected bounded workload output includes:

```text
Overall: PASS
Trace totals: count=256 overwrites=1090 total_events=1346
debug_trace_profiler: start=0x80004a54 end=0x80004b44 insn_count=62795 store_count=17511
```

## Interpreting the result

The plugin itself reports only:

- PC range
- total instruction count within that range
- total store count within that range

To compute per-event values, use the workload diagnostics:

```text
total_events = debug_trace_count() + debug_trace_overwrites()
```

With the example above:

- `total_events = 1346`
- `instructions/event = 62795 / 1346 = 46.653`
- `stores/event = 17511 / 1346 = 13.010`

## Notes

- `QEMU_BIN` should resolve to a QEMU binary with plugin support.
- `TRACE_PLUGIN_SO` should remain an absolute path.
- The plugin is intended for deterministic QEMU-based profiling with `-icount`.
- This tooling characterizes the tracing hot path; it should not be used as proof of hardware-cycle timing.
