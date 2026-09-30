# Report — Pinforic jam (game disk loading): Phosphoric not at fault

**Date**: 2026-08-20
**Status**: **Closed — no Phosphoric fix required** (bug on the Pinforic side, confirmed by an Oricutron cross-check)
**Components**: Microdisc/WD1793 (`src/storage/disk.c`), `--control` protocol (`load-disk`, `keys`), `--fdc-trace` / `--trace-ring` traces

## Context

Demonstration of the **Pinforic** Infocom interpreter (*two-disk* design: program
disk `pinforic.dsk`, then game disk). Scenario:

1. Boot `pinforic.dsk` (Sedoric V3.0) → run `INFOCOM` → Pinforic splash screen
   « Insert game disk & press a key ».
2. Hot swap of drive A: `pinforic.dsk` → `zork1.dsk`.
3. Key press → Pinforic reads the game → **the CPU jams** (illegal opcode `$02`),
   PC ≈ `$30F0` (varies between runs), emulation stops.

Initial hypothesis (team): the hot swap would leave the FDC state inconsistent, or Pinforic
would read without a **RESTORE**, assuming the head is on track 0.

## Approach & Phosphoric tools used

- **`--control`**: `load-disk A zork1.dsk` (deterministic hot swap), `keys`, `read`, `regs`,
  `break`, `peek disk`.
- **`FDC_TRACE=1`** (`--fdc-trace`): exact sequence of WD1793 commands.
- **`--trace-ring 50`**: last 50 instructions before the hang (ideal for this kind of jump).

## Evidence (measured)

### 1. The FDC reads correctly — "no RESTORE" / "FDC state" hypotheses refuted

`peek disk` before/after `load-disk`: state unchanged (`c_track=0A`, `cur_off=0100`) — **faithful
to the hardware** (a real swap also leaves the track register as it is; the program has to
issue a RESTORE). And the FDC trace after the key press shows that **Pinforic DOES issue the RESTORE**:

```
[FDC] seek target=0 (c_track=10 track_reg=10 data=00)   ← RESTORE track 0
[FDC] READ c_track=0 sector=1 side=0 ok
[FDC] READ c_track=1 sector=1..9 side=0/1 ok            ← sequential read
[FDC] READ c_track=2 sector=1..9 side=0/1 ok
```

**All reads `ok`, 0 `NOT_FOUND`.** The hot swap and the FDC positioning are correct.

### 2. The jam is an IRQ vectored into loaded data (`--trace-ring`)

```
203D  STA $58              ← last legitimate Pinforic code (page $20)
30EB  RLA ($91),Y          ← SP F8→F5: 3 bytes pushed = hardware IRQ
30ED  NOP $A0,X            ← bytes 33 91 54 A0 02 = game DATA, not code
30EF  JAM ($02)            ← halt
```

Between `$203D` and `$30EB`, **SP decreases by 3 (PC+P)** = the signature of a **hardware IRQ**.
The CPU services the interrupt and **vectors to `$30EB`, which is data loaded from
`zork1.dsk`** → garbage is executed → JAM. The IRQ vector/handler has therefore been **overwritten by
the game data** (buffer overrun). Since the IRQ lands on a variable cycle → **the jam PC is not
deterministic** (`$30EB`/`$30EF`/`$30F0`).

## Cross-check — Oricutron 1.2.0

Same `pinforic.dsk` + `zork1.dsk` pair, driven in Oricutron (F1 menu → *Insert disk 0* →
`zork1.dsk`, then a key):

| Emulator | Result when loading `zork1.dsk` |
|---|---|
| **Phosphoric** | JAM — IRQ into loaded data, `PC≈$30F0`, opcode `$02` |
| **Oricutron**  | **Identical crash** — CPU escaped to `$0002`, executes `$FF` (illegal); debugger opens automatically on JAM |

Identical frozen garbage screen, same failure mode. The exact address varies
(`$30F0` / `$0002` / `$FFFF`) — the signature of a **corrupted pointer / IRQ into garbage**.

## Root cause

**Pinforic bug**: when loading `zork1.dsk`, Pinforic writes the game data to the
wrong place (a **format/layout** mismatch between this `INFOCOM.COM` /
`PINFORIC.BIN` binary and the supplied `zork1.dsk` image — diverging versions/sector placement),
**overwriting the interrupt vector/handler**. At the first IRQ (VIA/Microdisc), the CPU
jumps into garbage → JAM.

## Conclusion

**Phosphoric is not at fault.** The WD1793 FDC reads the hot-swapped disk correctly
(RESTORE issued by the program, sectors `ok`, 0 `NOT_FOUND`), and **two independent
emulators timed at cycle level (Phosphoric, Oricutron) crash in the same way** →
the defect is 100 % on the Pinforic side (placement of the game data). **No emulator fix required.**

The `--fdc-trace` and above all `--trace-ring` tools isolated the cause in a single run.

## Repro

```bash
# Phosphoric (control):
oric1-emu --control -m atmos -r roms/basic11b.rom --disk-rom roms/microdis.rom \
          --fdc-timing fast -d pinforic.dsk --type-keys "9000000:INFOCOM\n"
#   > continue ; (at the splash) load-disk A zork1.dsk ; keys \n
#   Diagnostics: FDC_TRACE=1 …  +  --trace /tmp/ring.log --trace-ring 50

# Oricutron:
oricutron -m atmos -k microdisc -w -d pinforic.dsk
#   INFOCOM ⏎ ; F1 → Insert disk 0 → zork1.dsk ; (splash) any key ; F2 = monitor
```
