# Feasibility study — Mageco MIDI interface for the Oric

- **Status**: ✅ **IMPLEMENTED — levels A + B** delivered in Sprint 61 (v1.21.25 → v1.21.26-alpha)
- **Author**: bmarty
- **Date**: 2026-06-23
- **Source**: Defence-Force forum thread ["Mageco MIDI interface" (t=2525)](https://forum.defence-force.org/viewtopic.php?t=2525)
- **Decision**: GO — sprint completed (`src/io/mageco.c`, `--mageco`, `make test-midi`)

> **Level A delivery** (v1.21.25): MC6850 card at $03FE + MIDI file backend
> + tests. Code: `src/io/mageco.c`, `include/io/mageco.h`. CLI: `--mageco
> file:in[:out]|loopback|tcp|pty` (+ `--mageco-addr`). 13 tests + e2e (`90 3C 7F`).
>
> **Level B delivery** (v1.21.26): cross-platform **real-time MIDI** backend
> (`SERIAL_BACKEND_MIDI`, `src/io/serial_backend.c`). CLI: `--mageco midi[:TARGET]`,
> optional build `MIDI=1`. **ALSA** branch (Linux, links `-lasound`): a port named
> "Phosphoric MIDI" that can be connected via `aconnect` to FluidSynth/a DAW, validated in real time
> (`aseqdump` receives the Note On sent from BASIC). **CoreMIDI (macOS)** / **WinMM
> (Windows)** branches written against the APIs but not verified on this Linux host.
>
> **Extensions delivery** (v1.21.27): **Standard MIDI File** reader (`src/io/smf.c`,
> format 0/1, tempo map) + **`smf:FILE[:loop]`** transport replaying a `.mid`
> into the Oric as paced MIDI IN (`CLOCK_MONOTONIC`). 6 tests (`make test-smf`),
> validated in real time.
>
> **ORICON mode delivery** (v1.21.28): after reading the whole thread (4 pages),
> added the **ORICON** variant — 6850 at **$031C/$031D** + clock generator
> at **$031E/$031F** (latches), decoding placed in front of the Microdisc. Option `--oricon
> TRANSPORT` (Mageco mode `--mageco` kept). 4 tests, validated e2e (`POKE796/797`
> → capture `90 3C 7F`). The clock generator is modelled as latches (the divider
> encoding is not published in the thread); the rate stays at 31250 baud.

---

## 1. Context

> **Correction (after reading the 4 pages / 48 posts of the thread)**: the 1st version of this
> study only covered page 1. The thread describes **two distinct designs**, now
> both emulated (`--mageco` / `--oricon`):
>
> | | Original **Mageco** card (p.1, Dbug) | Modern **ORICON** (p.3, iss, 2025) |
> |---|---|---|
> | 6850 ACIA | **#3FE / #3FF** | **$31C / $31D** |
> | Clock | fixed crystal | **clock generator at $31E/$31F** |
> | Decoding | risk of conflict | "100% LOCI compatible" |
> | Synth | external (MIDI DIN) | MIDI-2 shield with built-in GM **SAM2695** |
> | Software | — | Lua tool **"midi2oric"**: pre-converts the .mid into C byte arrays (ms delays via 6502 waits) |
>
> Verbatim quote (iss, p.3, 26 Oct. 2025): *« The #ORICON uses standard serial I/O
> only at $31C..#31F. $31C/$31D for MC6850 ACIA and $31E/$31F for clock generator. »*

The **Mageco MIDI** interface is a 1980s Oric hardware extension, whose
reconstruction was revived by Dbug (modern PCBs, modular "Oric-Con / ORICON" project
with a MIDI shield). On the software side, the Lua tool "midi2oric" pre-converts MIDI.

Hardware characteristics of the original card (page 1):

| Item | Detail |
|---|---|
| Serial core | **Motorola MC6850 ACIA** |
| I/O addresses | **#3FE / #3FF** (RS=0 control/status, RS=1 data) |
| Decoding | 7411 (AND), 7404 (NOT) gates |
| Input isolation | **TIL 111** optocoupler (MIDI IN only) |
| Connectors | 2× DIN-5 (MIDI IN / OUT) |
| Clock | dedicated crystals (`xtal1`/`xtal2`) → MIDI rate **31250 baud**, 8N1 |

The thread does not document the exact control bits or IRQ handling: it is a
standard 6850, which we already know how to model.

---

## 2. What Phosphoric already has (heavy reuse)

Studying the code shows that **most of the core already exists**, which radically changes
the estimated cost:

1. **Standalone, reusable MC6850 model** — `src/io/acia6850.c` /
   `include/io/acia6850.h`. Pure UART logic (control/status/data registers, RDRF,
   TDRE, IRQ via the `irq_out` callback, FE/OVRN/PE, DCD/CTS). It is *exactly* the chip
   on the Mageco card. Already proven by the Digitelec DTL 2000.
   - Note: this model deliberately leaves the baud rate and byte transport to the host
     ("the clock is external, divided /1, /16 or /64"). The 31250-baud MIDI rate is therefore
     set on the host pacing side, not in the chip — no change to the 6850 core required.

2. **Configurable I/O routing** — `main.c` already routes an ACIA to a configurable
   base address (`--acia-addr`, `emu->acia_base_addr`, default $031C / $0380 LOCI). The
   #3FE/#3FF case is just a new base in the $0300-$03FF area.

3. **Serial backend abstraction** — `serial_backend.h` (enum `SERIAL_BACKEND_*`,
   struct `serial_backend_s`) already with `FILE` (replay/capture), `PTY`, `COM`, `TCP`…
   The "transparent transport" in `main.c` is shared between `--serial` and
   `--dtl2000`: a MIDI backend can be grafted onto it without duplicating the plumbing.

**Interim conclusion**: we do *not* have to write a new ACIA core. The real work
focuses on **(a) wiring at #3FE/#3FF** and **(b) a MIDI backend**.

---

## 3. Remaining work (the real costs)

### 3.1 Wiring the card at #3FE/#3FF
- New `--mageco` (or `--midi`) "device" instantiating an `acia6850_t` at base
  $03FE.
- Extend the I/O routing of `main.c` (read/write callbacks) for this base, handling
  the **address conflict**: #3FE/#3FF lies in the current $0300-$03FF VIA mirror
  (`memory.c:94`). The card must take priority when active, as is
  already done for the 6551 ACIA vs the Microdisc.
- **Known risk, documented in the thread**: « #3FE and #3FF can rise
  incompatibilities with other extensions ». To be reflected in `COMPATIBILITY.md`.

### 3.2 MIDI backend (the real chunk)
Three levels of ambition to choose from:

| Level | Description | Effort |
|---|---|---|
| **A. File capture/replay** | TX → raw MIDI bytes into a `.syx`/`.mid`, RX ← file. Reuses `SERIAL_BACKEND_FILE`. No real-time sound. | Low |
| **B. Host MIDI port (ALSA seq / CoreMIDI)** | TX/RX wired to a real system MIDI port → drives a real synth/DAW. | Medium (optional dependency, like SDL2) |
| **C. Internal synth** | Audio rendering of the MIDI stream inside the emulator (General MIDI). | High, beyond a reasonable scope |

Recommendation: aim for **A** first (testable deliverable with no dependency), keep **B**
as a build option (`MIDI=1`, in the style of `CAST=1`).

### 3.3 Timing
MIDI = 31250 baud, 8N1, ~320 µs/byte (≈ 320 CPU cycles at 1 MHz). The existing timing
step (per-instruction aggregation, `main.c:1679`) applies as is; it is enough
to set the byte rate to 31250 baud for this base.

### 3.4 Tests & docs (mandatory, agile method)
- New `make test-midi` suite (model: `test-serial`): 6850 reset, control
  write, TX of one byte → backend, RX → RDRF+IRQ, master reset, address conflict.
- Update `CHANGELOG`, `VERSION_TRACKING`, `CIRRUS_OS`, `ROADMAP`, `EMU_VERSION`,
  `COMPATIBILITY.md`, README (`--mageco`/`--midi` section).

---

## 4. Estimate (level A, file capture)

| Batch | Description | Load |
|---|---|---|
| S1 | `--mageco` device, 6850 instantiated at $03FE, I/O routing + address priority | ~150 LOC |
| S2 | MIDI file backend (reuses FILE), 31250-baud pacing | ~120 LOC |
| S3 | `make test-midi` suite + complete agile docs | ~200 LOC of tests |

≈ **450–500 LOC** for a testable MVP. Level B (real ALSA port) adds ~200 LOC
and an optional dependency.

---

## 5. Risks

- **#3FE/#3FF address conflict** with the VIA mirror and other extensions
  (explicitly flagged by Dbug). Manageable with conditional priority, to be tested.
- **No validation software on the emulated side**: an Oric program that
  drives the card (Fabrice's `.mid` player) is needed for end-to-end validation;
  otherwise the tests remain unit tests.
- **Level B** introduces a platform dependency (ALSA/CoreMIDI) → keep it optional.

---

## 6. Recommendation

**Feasible, at moderate cost** thanks to the already standalone MC6850 model and the configurable ACIA
routing. Proposal: one "level A" MVP sprint (card at #3FE/#3FF + MIDI
file backend + tests), with the real-time host MIDI port ("level B") as an optional
follow-up sprint. Go/no-go decision to be validated before opening the backlog.

---

## 7. Equivalence: emulator ↔ real Oric + Mageco card

Key point for the user: **from the MIDI data point of view, the emulator
and a real Oric fitted with the card are equivalent**, because MIDI is a
universal standard. An Oric program writes to the 6850, which emits MIDI bytes
at 31250 baud — whether the chip is physical or emulated, **the byte stream is
identical**. Only the physical transport layer differs.

### Physical Oric + Mageco card

```
Real Oric ──6850──► DIN-5 MIDI OUT socket ──MIDI cable──► [USB-MIDI interface] ──USB──► PC
```

- Real MIDI electrical signal (current loop, opto-isolated by the TIL 111 on
  input) on the DIN-5 sockets.
- To connect to the PC: a hardware **USB-MIDI interface** is mandatory.
- On the PC side, **the same software** as with the emulator (FluidSynth, DAW, `aconnect`…).
  The PC cannot tell a real Oric from the emulator.

### Comparison table

| | Real Oric + Mageco | Phosphoric emulator |
|---|---|---|
| MIDI stream / data | ✅ identical (MIDI standard) | ✅ identical |
| Transport to the PC | DIN-5 cables + **USB-MIDI interface** | **virtual port** (`file:`/loopback/tcp/pty) — no cable |
| Isolation / electronics | real optocoupler, risk of noise | not applicable (pure logic) |
| $03FE/$03FF address conflict | **real risk** if another extension is plugged in | software routing priority (warning if Microdisc) |
| 31250-baud timing | native (real 1 MHz) | faithfully emulated (320 cycles/byte) |

### Practical consequence

Since the emulation faithfully reproduces the 6850 at $03FE/$03FF and the 31250-baud
timing, **Oric MIDI software developed/tested on the emulator will run unchanged
on a real Oric fitted with the card**, and vice versa. The empirical proof is
the e2e test: a BASIC `POKE` of a Note On produces, in the file capture, the exact
bytes `90 3C 7F` — precisely what a real Oric+Mageco would put on
the MIDI OUT line.

### What a PC lets you do (by level)

- **Level A delivered (file)**: capture the MIDI stream emitted by the Oric into a
  `.mid`/`.syx` (readable by any player/DAW), or inject a file as MIDI IN.
  Ideal for developing/debugging without hardware, archiving, and non-regression testing.
- **Level B (upcoming, real-time host port)**: the emulated Oric drives a software
  synth (FluidSynth + GM soundfont) or a DAW, or a MIDI keyboard plays *into*
  the Oric, via the ALSA sequencer (`aconnect`, `a2jmidid`) / CoreMIDI / loopMIDI.

---

## Code references
- `include/io/acia6850.h`, `src/io/acia6850.c` — reusable MC6850 core
- `src/io/dtl2000.c` — example of integrating a 6850 + backend
- `include/io/serial_backend.h` — transport abstraction (enum `SERIAL_BACKEND_*`)
- `src/main.c:606-614`, `:762-771` — ACIA routing with address priority
- `src/memory/memory.c:93-94` — $0300-$03FF I/O mirror (the conflict area)
