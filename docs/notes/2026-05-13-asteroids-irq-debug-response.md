# Phosphoric team reply — Asteroids T1 IRQ user vector debugging

**From**: Phosphoric team (bmarty)
**To**: Oric‑1 Asteroids team
**Date**: 2026-05-13
**Subject**: reply to `phosphoric-irq-debug-request.md` — diagnosis of the
IRQ bug + technical answers + feature requests
**Phosphoric**: `v1.16.13-alpha` (ULA-accurate scanline-by-scanline rendering)

---

## TL;DR

**The bug has been found**: your IRQ handler assumes that the ROM has already done
`PHA / TXA PHA / TYA PHA` before handing over. **This assumption is
wrong** on Oric‑1 BASIC 1.0 AND on Atmos BASIC 1.1 when you patch
`$0228` (or `$0244`).

The 6502 CPU **only pushes PC and P** on IRQ. The PHA/PLA of the A/X/Y
registers is done *inside* the ROM handler (at `$EC0C` on Oric‑1, at
`$EE22` on Atmos). By patching `$0228`/`$0244`, you **bypass the ROM
entirely** — so no PHA has happened before your handler
runs.

When your handler does `PLA / TAY / PLA / TAX / PLA / RTI`, it pops:
1. `PLA` → pulls the P flags (and puts them in A — lost)
2. `TAY` → Y = P
3. `PLA` → pulls PC_low (into A)
4. `TAX` → X = PC_low
5. `PLA` → pulls PC_high (into A) — **but the following RTI expects P + PC_low + PC_high on the stack!**
6. `RTI` → pops **garbage** as P, **the real PC** as low, **the real SP** as high

The final PC is **completely wrong**, hence the hang in a random area
of the Asteroids code.

## Fix (3 asm lines)

```asm
_irq_handler:
        pha                       ; ← ADDED: push A
        txa                       ; ← ADDED
        pha                       ; ← ADDED: push X
        tya                       ; ← ADDED
        pha                       ; ← ADDED: push Y
        lda  VIA_IFR
        and  #$40
        beq  @not_t1
        lda  VIA_T1CL
        jsr  _sound_tick
        inc  _frame_cnt
@not_t1:
        pla                       ; pop Y
        tay
        pla                       ; pop X
        tax
        pla                       ; pop A
        rti
```

The contract is: **your handler is the FIRST code executed on IRQ**
when you patch `$0228` / `$0244`. It is up to you to save the registers
manually.

---

## Detailed answers

### Q1 — Oric‑1 BASIC 1.0 IRQ user vector dispatch

> *« la ROM BASIC 1.0 fait-elle bien un `JMP ($0228)` (indirect) ou un
> `JMP $0228` (direct) ? »*
> (*Does the BASIC 1.0 ROM really do a `JMP ($0228)` (indirect) or a
> `JMP $0228` (direct)?*)

**Neither — it is the 6502 CPU itself that dispatches directly
to `$0228`** through its hardware interrupt vector.

Exact mechanism:

1. On IRQ, the CPU reads the address stored at `$FFFE` / `$FFFF` (in ROM).
2. For the Oric‑1 BASIC 1.0 ROM: `$FFFE = $28`, `$FFFF = $02` → the CPU jumps
   to **`$0228`** (little-endian read).
3. The CPU **executes the code at `$0228`** (like a normal instruction,
   not an indirect JMP).
4. At boot, the ROM writes the 3 bytes `4C 03 EC` (= JMP
   `$EC03`) into RAM at `$0228`, which chains to the ROM handler at `$EC03`. Checked by hand:
   ```
   Phosphoric savestate Oric-1 boot $0228-022D: 4C 03 EC 4C 30 F4
   ```
5. The ROM handler at `$EC03` is `JMP $ED09` (itself a trampoline), which
   eventually reaches `$EC0C` where `PHA / TXA PHA / TYA PHA` is executed.

**Your `4C XX YY` at `$0228` is correct** as an installation method.
It is exactly what the ROM itself does at boot.

**Same for Atmos**: `$FFFE/F` → `$0244`. The ROM writes `4C 22 EE` at `$0244`
(JMP `$EE22`). Checked:
```
Phosphoric savestate Atmos boot $0244-024F: 4C 22 EE 4C B2 F8 40 ...
                                                         ^^ $024A=$40 (RTI)
```

Note the `$40` at `$024A` — this is the **user chain hook** (see Q2 option B).

### Q2 — Register save/restore by the ROM before `$0228`

> *« Mon handler suppose que la ROM IRQ a déjà fait PHA/TXA-PHA/TYA-PHA
> avant de céder la main à `$0228`. »*
> (*My handler assumes that the IRQ ROM has already done PHA/TXA-PHA/TYA-PHA
> before handing over to `$0228`.*)

**Wrong.** The contract depends on **where** you intercept:

**Option A — Replace the IRQ ROM entirely (your current choice)**

You patch `$0228` / `$0244`. The CPU dispatches directly to you, **the
ROM is not executed**. No PHA. You must save/restore A/X/Y
yourself.

**Option B — Chain after the ROM (recommended for compatibility)**

On Atmos, the ROM handler `$EE22` ends with `JMP $024A`. At `$024A` the ROM
places `40` (= RTI). By replacing `$024A` with `4C XX YY` (JMP `_user_handler`),
your handler runs **after** the ROM, with A already pushed/popped (the ROM
does `PHA` at `$EE22` then `PLA` at `$EE30` before chaining — so on
entry to `$024A` the registers are **clean**, as before the IRQ).

Atmos `$EE22` disassembly:
```
$EE22: 48              PHA            ; push A
$EE23: AD 0D 03        LDA $030D      ; VIA_IFR
$EE26: 29 40           AND #$40       ; T1 ?
$EE28: F0 06           BEQ $EE30      ; no → skip
$EE2A: 8D 0D 03        STA $030D      ; clear T1 flag
$EE2D: 20 34 EE        JSR $EE34      ; ROM IRQ sub-handler (PHA X,Y inside)
$EE30: 68              PLA            ; pop A
$EE31: 4C 4A 02        JMP $024A      ; ← user chain hook
```

So on Atmos, **patching `$024A` instead of `$0244`** gives you:
- T1 IRQ already cleared by the ROM
- A intact (PHA/PLA already done)
- X / Y untouched (the ROM saves/restores them *inside `$EE34`* only if
  it uses them)

On Oric‑1, the equivalent chain hook is not obvious in the BASIC
1.0 ROM (the `$EC03 → $ED09` handler does not expose a documented RAM
user vector). The simplest approach on Oric‑1 remains Option A with a correct PHA.

**For Oric‑1 + Atmos portability, recommendation**: Option A with
a clean PHA in your handler. It is portable, simple, 3 lines.

### Q3 — Multi-source IRQ behaviour

> *« La ROM utilise-t-elle elle-même des sources IRQ VIA (T2 ? CA1 ?)
> pour son scan clavier ou son clock interne ? »*
> (*Does the ROM itself use VIA IRQ sources (T2? CA1?)
> for its keyboard scan or its internal clock?*)

Yes — **and it is probably the second source of your hang**.

At boot, the Atmos ROM typically enables:
- **T1 IRQ**: 100 Hz for the internal clock and keyboard scan (`IER bit 6 = 1`)
- Possibly CA2/CA1 for printer ACK handling

When you do `LDA #$C0 STA $030E`, you ENABLE T1 but do **not touch
the other bits** already enabled by the ROM (bit 7 = 1 in `$C0` is the
master set/clear flag, bit 6 = T1 enable). The other sources remain
active.

If a **CB1 IRQ** is still active (e.g. during a CLOAD or when idle), and
a CB1 edge arrives, your handler runs, sees IFR & $40 = 0 (not T1),
jumps to `@not_t1` without clearing → CB1 stays set → the IRQ re-fires immediately →
**infinite IRQ loop**.

**Robust fix** in `_irq_install`:
```asm
        sei
        lda  #$7F                 ; DISABLE all VIA IRQ sources first
        sta  $030E                ; (bit 7=0 = clear, bits 0-6=1 = clear all)
        lda  #$7F                 ; clear all IFR flags
        sta  $030D                ; (write 1s clears flags)
        ; ... your $0228/$0244 patch ...
        lda  #$C0                 ; ENABLE T1 only (bit 7=1 set, bit 6=1 T1)
        sta  $030E
        cli
        rts
```

If a non-T1 IRQ fires anyway (for example because of a ROM
side effect), your handler can safely clear all sources with:
```asm
        lda  #$7F
        sta  $030D                ; clear ALL IFR flags
```
before the RTI.

### Q4 — Phosphoric tooling for IRQ debugging

Excellent requests — all relevant. Proposed roadmap:

#### 4.1 — `--trace-irq FILE` (high priority, ~3 h of dev)

Log every serviced IRQ: cycle, entry PC (= value read from
$FFFE/F), source (IFR snapshot BEFORE clearing), return PC (after RTI).

Implementation: hook in `cpu_step` when it detects a pending IRQ,
emitting a line in the format:
```
<cycle> IRQ entry: PC_pre=$XXXX → PC_post=$0228 IFR=XX IER=XX
<cycle> IRQ exit:  PC_post=$XXXX (via RTI)
```

Planned sprint: `34o` or `35a`.

#### 4.2 — `--dump-ram-at C:FILE` (high priority, ~1 h of dev)

Dump 64 K of RAM at a given cycle, raw binary format. Variant:
`--dump-ram-at C:ADDR:LEN:FILE` for a specific range (e.g. zero page
`00:00FF`).

Ideal for checking `frame_cnt` at different instants. Also allows
tracking `$0228`, `$0244`, `$024A`.

Planned sprint: `34o`.

#### 4.3 — Cycle-based breakpoint (medium priority, ~2 h)

Extension of `--break ADDR`: `--break ADDR --break-after-cycle C`.
Stops at the first `PC=ADDR` after `cycle >= C`.

#### 4.4 — Interactive debugger: ZP / RAM dump commands (low priority)

The `--debug` debugger already has `mem ADDR LEN` (see `src/debugger.c`). For
ZP: a `zp` → `mem 0000 100` alias is proposed.

#### Priorities

I can deliver **4.1 + 4.2** within the week if you confirm it is
useful (before 4.3 + 4.4).

---

## Reproducibility

Bug reproduced locally with `oric1-emu v1.16.13-alpha`:
- Without the IRQ sprint: asteroids runs (IFR polling)
- With the IRQ sprint + a `PLA / TAY / PLA / TAX / PLA / RTI` handler
  without the entry PHA: hang with PC in the asteroids code area, as
  observed on your side

## Hardware sources

Oric ROM IRQ dispatch verified by:
- Disassembly of Atmos `basic11b.rom` at `$EE22` (Phosphoric commit `6c9d30b`,
  Python script in the 2026-05-13 debugging session)
- Disassembly of Oric‑1 `basic10.rom` at `$EC03` → `$ED09` → `$EC0C`
- Reading the savestate at `$0228`/`$0244` after boot, confirming the JMP in RAM
- Cross-referenced with Oricutron `via.c` and conventions documented
  on [defence-force.org](https://forum.defence-force.org/)

## Contacts

- Phosphoric: `/home/bmarty/Oric1` (commit `6c9d30b`, `EMU_VERSION
  1.16.13-alpha`)
- Asteroids: `/home/bmarty/Oric asteroids` (commit `8af36cd`)
- bmarty <bmarty@mailo.com>

---

**Summary**: add `PHA / TXA / PHA / TYA / PHA` at the start of
`_irq_handler` and `LDA #$7F STA $030E` at the start of `_irq_install`
(disable all VIA IRQ sources before enabling T1). These 2 deltas should
get the IRQ sprint through. For the tooling, tell me whether you want to
prioritise `--trace-irq` or `--dump-ram-at`.
