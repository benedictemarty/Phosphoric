# Post-LOCI debug gaps audit (sprints 34y → 34c)

**Date**: 2026-06-07
**Reference**: 1.16.66-alpha (commit `6f31840`)
**Author**: senior staff audit
**Comparison baseline**: 5 gaps closed 34s→34w (symbols, paginated disasm, conditional breakpoints, memory edit, ncurses TUI with 6 panes)

---

## TL;DR

The massive addition of LOCI (~2,780 LOC, 4 TUs) and the Microdisc/ACIA/PSG consolidation have **no counterpart on the debugger side**: 0 REPL commands for `loci`, `fdc`, `acia`, `tape`, `mcp40`. The disassembler resolves symbols on `PC` but **not on operands** (a usability regression of the feature delivered in 34s). The CPU trace (`--trace`) also ignores symbols. No "rewind" capability and no screen tracker. 1 already-known symbol parser bug (`0x` prefix, Format A) confirmed by smoke test; no other parser bug detected across 6 lines of cross tests.

---

## Section 1 — Identified gaps

### P0-A: Disasm/trace do not resolve operand symbols
- **File:line**: `src/cpu/cpu6502.c:163-165` (cpu_disassemble), `src/utils/trace.c` (consumer)
- **Scenario**: symbol table loaded with `$E7AE CLOAD_HANDLER`. Command `d E000 5`:
  ```
  $E002: 20 E2 00  JSR $00E2          ; 6 cyc
  ```
  No symbol resolved on the JSR/JMP/branch target. Only the label of the current line (`addr` itself) is printed via `show_disassembly` → `symbol_lookup(addr)` (debugger.c:327). Same for `--trace`: output `00000002  F891  9A  TXS …` without a name. Consequence: the "symbols" feature delivered in 34s is only half usable — the whole point of loading a `.lab` is to read `JSR <CLOAD>`.
- **Effort**: S — change `addr_mode_fmt` to accept an optional `(uint16_t)->const char*` callback, or a wrapper in `show_disassembly` that post-processes the string (regex `$XXXX` → name).

### P0-B: Zero LOCI introspection although 2,780 LOC were added
- **File:line**: `src/debugger.c:464-465` (help) — only `via`, `psg`. No `loci` command.
- **Scenario**: `--loci --loci-flash /tmp` active. The debugger cannot show: current op register (`$03A0`), status (`$03B1`), file handle table (10 entries in `loci_fs.c`), SD-IMG mounts, last errno error, current read/write position. When the Sedoric V4.0 stage 1 boot fails, one has to re-read the `--verbose` logs instead of a snapshot. Critical gap for the maintenance of the whole 34a* sprint series.
- **Effort**: M — `loci_dump_state(loci_t*, FILE*)` + `loci` handler in the REPL (~120 LOC).

### P1-C: No `fdc` / `disk` command for the Microdisc WD1793
- **File:line**: `src/io/microdisc.c` (191 LOC), no exposed `microdisc_get_state` API; `src/debugger.c` does not include microdisc.h.
- **Scenario**: a DSK boot hangs on a `SEEK` command. Impossible to read the WD1793 track/sector/status, current drive, DRQ/INTRQ live. One has to rerun with `--trace` and grep.
- **Effort**: S — `printf` of the 8 FDC registers + `md->status` + mount info for the 4 drives, ~50 LOC.

### P1-D: No `acia` command for the 6551
- **File:line**: `src/io/acia6551.c` (571 LOC), no debugger hook.
- **Scenario**: `--serial modem:host:port --serial-irq-on-rdrf`. To diagnose a TX/RX stall (Minitel, V23), one needs `--serial-trace FILE`, which is post-mortem. No live snapshot of the status/control/cmd registers nor of the TX/RX FIFOs.
- **Effort**: S — display of the 4 registers + FIFO state + RTS/DCD signals, ~40 LOC.

### P1-E: No cassette monitor (position in bytes/seconds)
- **File:line**: `src/io/cassette.c` (no match for grep `position|tape_pos|tap_pos` → the internal state is not exposed).
- **Scenario**: a long tape CSAVE in progress. No visibility on the current byte written, elapsed time, next header. Oricutron exposes a drive bar.
- **Effort**: XS — add the counter already present in cassette_t + a `tape` command.

### P2-F: No rewind / reverse single-step
- **File:line**: N/A — savestate.c offers no in-memory ring-buffer API.
- **Scenario**: after a step over that goes off the rails (JSR to an IRQ that clobbers the stack), impossible to go back 1 instruction. Standard in Mesen, Bizhawk, and historically absent from Oricutron but requested on the forums.
- **Effort**: M — in-memory ring buffer of ~32 compressed savestates (RLE on RAM), `u` (undo) opcode in the REPL.

### P2-G: No screen tracker (cycle ↔ raster line)
- **File:line**: N/A — `--screenshot-at C:FILE` allows a snapshot but not a "break when raster = 100" breakpoint.
- **Scenario**: debugging a HIRES effect that crashes on a specific line. We want `break raster 142`. Today we use tape-key + screenshot.
- **Effort**: M — extension of the breakpoint engine with a RASTER type, hook in the frame loop.

### Latent bugs NOT found in the smoke test
I tested Format A (`$XXXX NAME`), Format B (`NAME = $XXXX`), Format C (`al C XXXX .NAME`), Format D (`NAME EQU $XXXX`), names with `.` and `_`, hex without prefix. All work. Only the `0xXXXX` prefix bug in Format A (already identified outside the audit) remains open. No other silent regression detected in the 5 features 34s→34w.

---

## Section 2 — Sprint 34d recommendations (by value/effort ratio)

| # | Task | Value | Effort | Ratio |
|---|-------|--------|--------|-------|
| 1 | **P0-A**: symbol resolution on disasm + trace operands | Very high — revives the 34s feature for its main use | S | **★★★★★** |
| 2 | **P0-B**: `loci` REPL command (dump current op + handles + mounts + errno) | High — covers 2,780 LOC with no debugging today, upcoming LOCI sprints benefit | M | **★★★★☆** |
| 3 | **P1-C**: `fdc` REPL command (WD1793 registers + drives) | High — recurring Sedoric/DSK boot investigations | S | **★★★★☆** |
| 4 | P1-D: `acia` command | Medium — useful for the serial sprints but less frequent use | S | ★★★☆☆ |
| 5 | P1-E: `tape` command | Medium | XS | ★★★☆☆ |
| 6 | P2-F/G: rewind + raster break | High but complex | M+M | ★★☆☆☆ |

**Top 3 recommendations for sprint 34d**: P0-A + P0-B + P1-C (≈ S+M+S, ~250 LOC, immediate tangible gain for the LOCI/DSK debugging sessions).

---

## Section 3 — What is OK (no action)

- **Symbol parser**: robust on the 5 cases tested (except the known `0x` bug, out of scope).
- **Memory edit `m addr = V1 V2 ...`**: present (`debugger.c:600+`), works.
- **Conditional breakpoints** (`b addr if EXPR`): present (`debugger.c:670+`).
- **Paginated disasm** (`d +` / `d -` history): present.
- **6-pane TUI**: not regressed (`TUI=1` build OK).
- **`--trace-irq`**: implemented (main.c:2315-2330).
- **VIA / PSG REPL dump**: present and correct.
- **`.sym` / `.lab` / `.sym65` formats**: load OK, just not used on the disasm side (gap P0-A).
