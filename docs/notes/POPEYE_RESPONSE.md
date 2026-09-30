# Popeye Issue #1 — Fast-load overwritten by the RAM test

**Date**: 2026-03-04
**Status**: Fixed (v1.14.1-alpha)

## Reported problem

Fast-load mode (`-f`) injects the TAP data into RAM **before** `cpu_reset()`.
The BASIC 1.0 ROM then runs a full RAM test ($FA1F-$FA45, ~2.5M cycles) which
overwrites the whole $0000-$BFFF area, destroying the injected code.

## Root cause

The immediate injection through `memory_write()` in the CLI parsing loop happens
before emulation starts. The `cpu_reset()` at the beginning of `emulator_run()`
resets the PC and the ROM runs its RAM test, which sweeps all of low memory.

## Fix applied

**Approach: deferred injection based on a cycle threshold**

1. **`emulator.h`**: 4 fields added to `emulator_t`:
   - `fastload_buf`: buffer holding the TAP data
   - `fastload_addr`: destination address
   - `fastload_size`: size in bytes
   - `fastload_pending`: pending-injection flag

2. **`main.c`** (fast-load section): instead of injecting immediately via
   `memory_write()`, the data are stored in the dedicated buffer.

3. **`main.c`** (`emulator_run()` loop): after each frame, if
   `total_executed > 3_000_000` and `fastload_pending == true`, the data
   are injected into RAM and the buffer is freed. The 3M-cycle threshold
   guarantees that the ROM's RAM test (~2.5M cycles) has finished.

4. **Cleanup**: the buffer is freed in `emulator_cleanup()` in case of
   early shutdown.

## Tests added

- `test_deferred_fastload_fields`: checks the initial values
- `test_deferred_fastload_buffer`: checks buffering and injection
- `test_deferred_fastload_survives_ram_clear`: the buffer survives a RAM clear

## Verification

```bash
# Build and tests
make tests    # 283 tests, 100% pass

# Manual test
./oric1-emu -r roms/basic10.rom -t prog.tap -f
# Expected log: "Deferred fast-load: injected N bytes at $XXXX-$YYYY (after ZZZZ cycles)"
```

## Version

- **v1.14.1-alpha** — Sprint 32 bugfix
- Commit: fix: deferred fast-load to survive ROM RAM test (Popeye #1)
