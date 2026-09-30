# The ORIC audio output stage — what the schematic says

**Read on**: 2026-09-11 (V2-E5, 2.0.0-alpha.5)
**Source**: Oric-1 / Atmos schematic redrawn in KiCad by Manoël Trapier
(986 Studio), sheet *PSG 1 Keyboard* (`audio.sch`), revision **Issue 6.1** —
[OricSchematics.pdf](https://www.986-studio.com/wp-content/uploads/2014/09/OricSchematics.pdf)
Supplement: thread ["Oric's 8912 volume scale"](https://forum.defence-force.com/viewtopic.php?t=1357)
on the Defence Force forum (measurement of the mixing).

This document exists because two "accepted deviations" of the PSG rested on
an uncertainty that could be removed by reading the schematic. One was **wrong**,
the other **irrelevant**. A third one remains open, and we say why.

## The circuit

```
   AY-3-8912 (IC4)
   CH_C (1) ──┬───────────────── R2 ──┬──┬──────── C4 ──── SOUND_OUT ──► R33 ──► LM386 (+)
   CH_B (4) ──┤                 4,7 k │  │                                22 k
   CH_A (5) ──┘                       │  │
              └── R4 ── AGND        C5│  │R3
                  1 k              10 n│  │470
                                     AGND AGND
```

| Reference | Value | Role |
|-----------|--------|------|
| `R4` | 1 kΩ | common load: the **three outputs are tied together** on it |
| `R2` | 4.7 kΩ | series resistor of the output divider |
| `R3` | 470 Ω | shunt of the divider |
| `C5` | 10 nF | low-pass, in parallel with `R3` |
| `C4` | *ambiguous* (marked "2k2") | coupling to the amplifier — blocks DC |
| `R33` | 22 kΩ | bias of the LM386 + input |
| `IC2` | LM386 | power amplifier (default gain 20) |
| `C1` | 220 µF | coupling to the speaker `SP1` |

## What we learn from it, and what we do with it

### 1. The mixing averages the channels — our `sum / 3` is correct

The three outputs do not go through separate summing resistors:
they are **directly tied** to the same point, loaded by `R4`. Two idle outputs
therefore "pull" on the active one. The measurement reported on the
Defence Force forum says it unambiguously: a single channel at 1 V gives **≈ 0.33 V**
at the mixing point, not 1 V.

This is exactly what the model does: each channel goes through the volume table
(non-linear, measured), then the sum is divided by 3. The deviation
"the real AY sums currents, so the sum is not linear" was
**unfounded**. Locked in by `test_ay_parallel_mixing_averages_channels`.

### 2. The low-pass cuts off at 37 kHz — irrelevant at 44.1 kHz

`C5` sees `R2 ∥ R3` = 427 Ω, hence:

```
f_c = 1 / (2π · 427 Ω · 10 nF) ≈ 37,2 kHz
```

That is **above Nyquist** (22.05 kHz): this filter shapes nothing in the
band we can reproduce. And since the PSG is clocked like the hardware with
per-sample integration (V2-E5), the ultrasonic range is already handled. Nothing to
model: the "no analogue low-pass filter" deviation is **irrelevant**.

The `R2`/`R3` divider attenuates by **−20.8 dB**, but that is **gain**, made up by
the LM386 (×20): it does not change the shape of the signal.

### 3. The coupling blocks DC — and that is the only certainty about `C4`

The PSG signal is **unipolar**: it goes from 0 to +max, never below.
Measured before the fix, three channels at full volume gave a DC offset of
**+8188** — half the amplitude. No speaker reproduces that, and
`C4` prevents it from getting out.

The emulator therefore blocks DC (fixed-point estimator, cutoff
**≈ 1.7 Hz**, inaudible). Measured afterwards: DC **+15**, **symmetrical** signal
(−8232 / +8252 instead of 0 / +16383).

**What we do not do, and why.** The value of `C4` cannot be determined
from this document: it is marked "**2k2**" there, with no unit, whereas all the
other capacitors on the sheet carry theirs (`10n`, `47n`, `220uF`,
`10uF`). The two plausible readings give radically different
circuits:

| Hypothesis | `f_c` with `R33 ∥ Z_in(LM386)` ≈ 15.4 kΩ | Consequence |
|-----------|------------------------------------------|-------------|
| `C4` = 2.2 nF | ≈ **4.7 kHz** | severe high-pass: the sound loses all its bass |
| `C4` = 2.2 µF | ≈ **4.7 Hz** | plain DC blocking |

A factor of a thousand on the audible result. We do not decide by guesswork:
only the **certain** effect (DC blocking) is modelled. A measurement on a
real machine, or a legible photo of the PCB, would settle it — and it is the
only thing that will.

### 4. The amplifier and the speaker are not modelled

The LM386 and the small speaker `SP1` have their own response, not measured. What
the emulator reproduces is the **electrical signal** — what anyone listening
on headphones or speakers expects. Modelling the response of the internal
speaker would be a separate piece of work, and it would require measurements.

## Summary

| Open question before | After reading the schematic |
|------------------------|--------------------------|
| Is the mixing non-linear? | **No**: parallel → average. The model was already right. |
| Is an analogue low-pass missing? | **Irrelevant**: it cuts off at 37 kHz. |
| Should DC be blocked? | **Yes**, `C4` does it on the machine. Fixed. |
| What is the exact cutoff of the coupling? | **Undetermined** from this document (factor-of-1000 uncertainty). Not modelled. |
