# Technical note — Asteroids Oric‑1: VSync via CB1 ≠ real hardware

**Initial date**: 2026-05-13
**Revision v2**: 2026-05-13 (primary sources added, Option B withdrawn)
**From**: Phosphoric team (bmarty)
**To**: Asteroids Oric‑1 team
**Subject**: `frame_wait()` relies on a CB1 = VSync wiring that does not exist on the stock Oric

---

## TL;DR

Your `frame_wait()` (`src/game.c:289-300` v1) polls `IFR bit 4` (CB1),
assuming the pin is pulsed every frame by the ULA.
**Checked against primary Oric sources (oric.free.fr, defence-force.org,
twilighte.oric.org, etc. — see §9): on the stock Oric, CB1 = tape signal
input**, *not* VSync. Phosphoric ≤ 1.16.10 emulated by default a
non‑conformant convention (equivalent to Oricutron's opt‑in `vsynchack`
option) which masked the bug. Since **Phosphoric 1.16.11** (commit
`f98f828`, 2026-05-13), Phosphoric follows the factory wiring and the
program **loops forever**, as in Oricutron WIP `f79d5d4` (default
`vsynchack=OFF`).

Action required on the Asteroids side: replace the CB1 sync with **the VIA's
Timer 1 in continuous mode** (the only portable option for real Oric + all emulators).
The "memory-mapped ULA bit read option" I had suggested in v1
**does not exist** on the stock Oric — see the corrected §3 and §5.

---

## 1. Observed symptom

Oricutron `f79d5d4` capture after loading `asteroids.tap`:

```
PC=0D8E   AD 0D 03   LDA $030D    ; VIA_IFR
0D91      29 10      AND #$10     ; isolate bit 4 (CB1)
0D93      F0 F9      BEQ $0D8E    ; loop while flag = 0
```

PC oscillates between `$0D8E` and `$0D9F` (two instances of the same `frame_wait()` at
different places in the code). The "ASTEROIDS / PRESS SPACE" title screen
stays frozen, no polish animation, no keyscan.

Reproduced identically on Phosphoric 1.16.11:

```
oric1-emu -r basic10.rom -t asteroids.tap -f -n -c 10000000
→ Final CPU state: A:40 X:00 Y:0E SP:FB P:..-..... PC:0D8E
```

## 2. Offending code

`src/game.c:126-130`:

```c
/* Phase 9 — ULA VSync sync via CB1.
 * On the Oric-1, CB1 is connected to the ULA VSync signal (50 Hz PAL).
 * IFR bit 4 = CB1 flag, set on transition. At 25 Hz = 2 VSyncs per frame. */
#define VSYNC_FLAG       0x10        /* IFR bit 4 = CB1 */
#define VSYNCS_PER_FRAME 2           /* 50 Hz / 2 = 25 Hz */
```

`src/game.c:289-300`:

```c
static void frame_wait(void)
{
    unsigned char i;
    for (i = 0; i < VSYNCS_PER_FRAME; i++) {
        while (!(VIA_IFR & VSYNC_FLAG)) { }
        VIA_IFR = VSYNC_FLAG;        /* clear by writing the bit */
    }
}
```

The comment "CB1 is connected to the ULA VSync signal" is **incorrect
for the Oric hardware**.

## 3. Oric‑1 / Atmos hardware reality (primary sources)

On the stock Oric, **the VIA 6522's CB1 pin = tape signal input**,
**not** VSync. Direct quotations from authoritative sources of the Oric
community:

> *« The CB1 pin on the 6522 is the tape signal input, with a 1K resistor and
> 2.2nF capacitor connected between 5V and the CB1 pin. »*
> — [Hardware Programming on the Oric (oric.free.fr)](http://oric.free.fr/programming.html)

> *« The cassette circuitry connects to the CB1 line on the 6522. When this
> goes from low to high the CB1 flag is set in the interrupt flag register of
> the 6522. »*
> — [VIA — twilighte.oric.org](http://twilighte.oric.org/twinew/via.htm)

> *« A 1-bit digitised version of the tape's audio stream is fed as an input
> to the CB1 pin of the Oric's VIA. The ROM configures the 6522 to raise an
> interrupt upon a low-to-high transition of CB1. It then measures the amount
> of time between those interrupts using the 6522's second timer. »*
> — [Defence Force Wiki — Oric tape encoding](https://wiki.defence-force.org/doku.php?id=oric:hardware:tape_encoding)

> *« Reading the tape requires measuring the time interval between edges on
> the CB1 pin (with a VIA's timer of course). »*
> — [Connecting a Commodore tape drive to the Oric‑1 (Marko Mäkelä)](https://www.ktverkko.fi/~msmakela/8bit/c2n-oric/index.en.html)

**Consequences**:

- When idle (no tape being read), CB1 stays high. No
  edge, no `IFR.CB1` is set, and `frame_wait()` loops forever.
- During a `CLOAD`, CB1 toggles at the tape bit rate
  (~2400 Hz Standard / ~1200 Hz Slow), **never at 50 Hz**. So even during
  a load, the polling would return garbage.

### So what about the ULA VSync?

With the factory wiring, **VSync is not exposed to the CPU through the VIA**. It
only comes out on the SYNC pin of the RGB connector, for the monitor.
The option we thought was the clean one — "read a memory-mapped ULA bit for
VSync" — **does not exist on standard hardware**. Oric programs
that want a frame rate rely on:

- the VIA's **Timer 1** in continuous mode (stable 20 ms PAL rate)
- or counting 6502 cycles by hand

There is also a **DIY hardware modification** documented by the
community: rewire the SYNC pin of the RGB connector to the TAPE input,
so that VSync arrives on CB1. That is the mod Oricutron emulates
with its `vsynchack` option (OFF by default) — not the factory wiring. See
the Oricutron code `tape.c:1414-1423` (`if (oric->vsynchack)`) and the
comment `ula.c:295-312`, which illustrates the waveform of that mod.

The current asteroids code only worked on emulators with this
"hack" enabled (Phosphoric ≤ 1.16.10 simulated it by default, which was
not conformant with the stock Oric).

## 4. Why Phosphoric ≤ 1.16.10 "worked"

As a historical simplification, Phosphoric pulsed CB1 falling/rising every
frame (`src/main.c:857-866`):

```c
if (!vsync_triggered && frame_cycles >= VSYNC_CYCLE) {
    via_set_cb1(&emu->via, false);  /* falling */
    vsync_triggered = true;
}
...
if (vsync_triggered) {
    via_set_cb1(&emu->via, true);   /* rising */
}
```

With `PCR bit 4 = 1` (the Oric ROM configuration), the rising edge of CB1
set `IFR bit 4 = 1`, and `frame_wait()` saw its condition satisfied.

**This behaviour did not conform to the hardware.** It was removed in
1.16.11 to make Phosphoric usable as a hardware reference during
development.

## 5. Proposed solutions (in order of quality)

### Option A — VIA Timer 1 in continuous mode (recommended, portable everywhere)

Program T1 for 20,000 µs (= 20,000 cycles at 1 MHz, i.e. `$4E1F`) in
free-run mode. The IFR bit 6 (T1) flag will be set every 20 ms, and the polling becomes:

```c
#define T1_FLAG     0x40        /* IFR bit 6 */

static void timer_init(void)
{
    VIA_T1CL = 0x1F;
    VIA_T1CH = 0x4E;            /* 20000 → 20 ms PAL */
    VIA_ACR  = (VIA_ACR & 0x3F) | 0x40;  /* T1 free-run, no PB7 */
    VIA_IER  = 0x40;            /* disable T1 IRQ (polling only) */
    VIA_IFR  = 0x40;            /* clear initial flag */
}

static void frame_wait(void)
{
    unsigned char i;
    for (i = 0; i < VSYNCS_PER_FRAME; i++) {
        while (!(VIA_IFR & T1_FLAG)) { }
        VIA_T1CL_RESET;          /* re-read T1CL to clear IFR T1 */
    }
}
```

**Advantages**: works on a real Oric, Oricutron, Phosphoric, MAME. Drifts
by a few cycles per frame (acceptable for a 25 Hz game).

**Drawback**: not strictly synchronised with the screen scan → slight tearing
possible on dynamic HIRES. For Asteroids it is negligible.

### ~~Option B — Reading a ULA VSync bit~~ (does not exist on the stock Oric)

This option had been suggested in the first version of this note. **It
is invalid**: after checking primary sources (see §3), VSync
is not exposed to the CPU through a memory-mapped bit on a standard Oric‑1/Atmos.
The only "hardware" way to get VSync on CB1 is the DIY
RGB→TAPE modification, which is not part of a stock Oric and which no
modern buyer will have.

➡️ **That leaves option A (Timer 1 free-run): that is the one to adopt.**

### Option C — Frame counter via ROM NMI/IRQ

The Oric ROM already installs an IRQ handler that runs on every T1 (its own
configuration). Hook `$02FA`, or piggyback on a zero-page counter incremented by the
ROM. More fragile, depends on the exact ROM (BASIC 1.0 vs 1.1). Avoid.

## 6. Tests to redo after the fix

1. `asteroids.tap` fast-load on Phosphoric 1.16.11+: must get past the title screen
   and accept SPACE.
2. `asteroids.tap` on Oricutron WIP: must get past the title screen.
3. `asteroids.dsk` on a real Oric‑1 (PAL revision): must play normally
   at ~25 fps.

## 7. On the Phosphoric side — what changed

- **v1.16.10 and earlier**: Phosphoric pulsed CB1 every frame
  (equivalent to Oricutron's `vsynchack`). Not conformant with the factory wiring —
  it was a legacy convention.
- **v1.16.11** (commit `f98f828`): CB1 is never driven by the emulator any more.
  Idle state high. Phosphoric follows the factory wiring documented by the
  Oric sources.
- **v1.16.12** (commit `2f02b64`): independent fix — removal of
  `SDL_RENDERER_PRESENTVSYNC`, which caused a false Mutter "not responding"
  when the window stayed static (the case of the program stuck in the
  current `frame_wait`).

Note: Phosphoric does not (yet) drive CB1 from the tape signal
during an unpatched CLOAD — fast-load (`-f`) short-circuits the
ROM routine via PC patches, so real-time tape bit sampling is not
on the critical path. Implementing the CB1 tape signal remains a TODO
(see `docs/AGILE_PLAN.md` T‑319) but with no impact on Asteroids.

## 8. Contacts

- Phosphoric emulator: `/home/bmarty/Oric1`, version commit
  `EMU_VERSION 1.16.12-alpha`
- Asteroids code: `/home/bmarty/Oric asteroids`
- bmarty <bmarty@mailo.com>

## 9. Sources and references

This note was updated on **2026-05-13** after cross-checking
against primary sources of the Oric community. Refer to these links
first for any hardware question:

- [Hardware Programming on the Oric (oric.free.fr)](http://oric.free.fr/programming.html)
  — Fabrice Frances reference, explicit 6522 pinout
- [ORIC 1/ATMOS Unofficial ULA Guide](http://oric.free.fr/HARDWARE/ula.html)
  — internal ULA signals and their outputs
- [ULA Deconstruction 1 — signal11.org.uk](https://oric.signal11.org.uk/html/ula1.htm)
  — reverse-engineering analysis of the ULA
- [SERVICE MANUAL FOR THE ORIC‑1 and ORIC ATMOS (PDF, 48k atmos)](http://www.48katmos.freeuk.com/servman.pdf)
  — official factory schematics
- [Up to date Oric‑1/Atmos Schematic — defence-force.org forum](https://forum.defence-force.org/viewtopic.php?t=1959)
  — pointers to the schematics redrawn by Godzil
- [VIA — twilighte.oric.org](http://twilighte.oric.org/twinew/via.htm)
  — details of the VIA 6522 wiring on the Oric
- [Defence Force Wiki — Oric tape encoding](https://wiki.defence-force.org/doku.php?id=oric:hardware:tape_encoding)
  — tape protocol, CB1 = digitised bit input
- [Connecting a Commodore tape drive to the Oric‑1 (Marko Mäkelä)](https://www.ktverkko.fi/~msmakela/8bit/c2n-oric/index.en.html)
  — details of the tape circuit on the CB1 side
- [Tynemouth Software — Oric Atmos Repair](http://blog.tynemouthsoftware.co.uk/2022/11/oric-atmos-repair.html)
  — details of the audio/tape circuitry
- [MOS Technology 6522 — Wikipedia](https://en.wikipedia.org/wiki/MOS_Technology_6522)
  — general reference for the chip

Emulator sources (for comparison):

- **Oricutron** (`~/oricutron/tape.c:1414‑1423`) — `vsynchack` option,
  off by default, emulates the RGB→TAPE mod
- **Oricutron** (`~/oricutron/ula.c:295‑312`) — waveform comment for the
  mod (12µs delay + 260µs negative pulse)
- **Phosphoric** (`src/main.c:852‑858`) — since 1.16.11, CB1 idle

---

**Corrected executive summary (2026-05-13 v2)**:

> On a stock Oric, **CB1 = tape signal input**, *not* VSync.
> Asteroids Phase 9's "`frame_wait` on CB1" only runs on
> emulators with `vsynchack` enabled (or Phosphoric ≤ 1.16.10, which
> did it by default). The only frame sync that is portable across real Oric + all serious
> emulators is **the VIA's Timer 1 in free-run at 20 ms PAL**.
> Implementation: ~10 lines of diff in `game.c` (Option A §5).
