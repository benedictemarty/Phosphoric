# Phosphoric 2.0: a machine clocked per cycle — and the claim we are withdrawing

*Public technical note, 2026-09-11 (2.0.0-beta.1). English version; the
test and option names are those of the repository.*

## What we had said, and why it was wrong

Up to 1.120.0-alpha, Phosphoric described itself as accurate "to the cycle".
That was not true in the sense reference emulators give to the term.
The 6502 core executed an instruction in one block: the bus accesses came out in
the right order and the total cycle count per opcode was right, but the internal
cycles were **caught up by padding at the end of the instruction**, the NMOS
dummy accesses did not exist, and interrupts were taken at instruction
boundaries. The VIA received batches of cycles, the ULA rendered
a whole line at once, the PSG ran at the audio sample rate.

That is a respectable level — we named it **N2, "ordered at bus-cycle level"** —
but it is not "exact to the cycle". We withdrew the wording, wrote a
[verifiable scale](../ACCURACY.md) (N1 to N4, each level backed by the test that
proves it), and an automatic safeguard (`make test-docs-claims`) that rejects any
unqualified claim of cycle accuracy in the showcase documents. Then we
built V2.

## What V2 changed

**An oracle first.** Before touching the core, `make test-cycle` replays the
SingleStepTests/65x02 vectors — 10,000 cases per opcode, with the expected bus
trace cycle by cycle — and scores four properties separately. The starting
point, measured and published: **44.26 %** of exact bus sequences. The oracle
also revealed five logic defects in the core, fixed before we even
started.

**A micro-sequenced core.** Each instruction becomes a plan of micro-operations,
one per cycle, each doing exactly its bus access — including the dummy
accesses (zero-page indexing, page crossing, RMW write-back, dead stack
reads). Result: **100.00 %** over 2,440,000 cases. Interrupts
are sampled on the **penultimate cycle**, which gives for free the
delayed I flag of `CLI`/`SEI`/`PLP` and the hijacking of a `BRK` by an NMI.
Both cores share the same computation (flags, BCD, illegal opcodes):
only the scheduling differs. The old one remains available (`--cpu-legacy`).

**A master clock.** `emu_cycle()` advances **the whole** machine by one
cycle, in a fixed order: the CPU makes its bus access, the peripherals
advance by one cycle — never by a batch — then the ULA fetches the cell of that
same cycle (the order measured on the hardware by Mike Brown; up to 2.0.1
the ULA went first, and every split landed one cell too far to the right). The
main loop no longer computes anything: it asks.

**The components, one by one.** The VIA counts per cycle and its Timer 1 is back to
its **N+2** period (the old model gave N: a 20 % error for N=10). The ULA
fetches **one 6-pixel cell per cycle**, at the instant the beam reads it:
a write in the middle of a line only reaches the cells not yet scanned —
raster splits become possible. The PSG runs at `clock/8` (125 kHz) and
its output is integrated: a tone programmed above Nyquist is attenuated instead
of aliasing, and the **envelope, twice too slow since forever**, runs at the
right speed. The analogue output stage was read from the official schematic.
The WD1793 signals `LOST DATA` and write protection. The signal-level tape
has recovered its **odd parity**.

## What changes visibly

- **Raster splits**: a program that changes the ink or the mode in the middle of
  a line gets what the hardware would give, not a uniform line.
- **Sound**: envelopes with the right duration, no more ghost whistling on
  very high tones, centred signal (DC blocked as on the board).
- **IRQs**: an interrupt can no longer be taken *before* the current
  instruction; `SEI` does not protect the instruction that follows it from an already pending IRQ.
- **Destructive-read registers**: a BASIC `POKE` to the data register
  of an ACIA **loses a byte**, because `STA (zp),Y` does a dummy
  read before writing — on the emulator as on the machine. Phosphoric
  was more permissive than the hardware; it no longer is.
- **Savestates**: a state taken mid-frame resumes exactly where it
  stopped (beam position, timers, interrupt sample).

## What we learned along the way

Two defects would never have been seen without changing method.

The PSG envelope was declared conformant "by recalculation" — a recalculation that
assumed 16 states instead of 32. A wrong assumption is invisible on
re-reading; it only shows up by **measuring the signal**. The audio tests
now measure frequencies and durations instead of comparing bytes.

The second is even more instructive. A branch not taken made its
decision **one cycle too late**, in a micro-op with no bus access. The CPU's
counter stayed right, so the oracle was 100 % green. But the master clock
had been called one extra time: the ULA advanced by a cycle that neither the CPU nor
the VIA had lived through — about 410 times per frame on the BASIC ROM, i.e. one
frame of drift per second between the picture and the rest of the machine. An oracle
that looks only at the CPU does not prove the synchronisation of the machine. What
revealed it: a savestate determinism test, which required
raster stops to land on exactly the same cycle.

## What we do not claim

"Phosphoric is exact to the cycle" — no. The FDC is still paced by fixed
delays on a flat image (no MFM stream, so no real byte
loss and no CRC). The horizontal reference, on the other hand, is no longer a convention: the
ULA counter (measured) puts column 0 at count 0 — but the absolute
phase between that counter and the CPU can only be observed on a real ORIC with
the "VSYNC hack", which we do not emulate. The half-cycle of the
VIA one-shot is not represented. Default tape loading
remains the ROM patch, by choice: same content loaded, 2.4× fewer cycles.

The exact authorised wording, and the test that would bring down each of its
lines, are in [docs/ACCURACY.md](../ACCURACY.md).

## Cost

Moving to per-cycle stepping cost, on the reference machine at full speed,
491 → 611 µs per emulated frame: **3 % of the 20 ms budget**. `make test-bench`
now rejects any overrun beyond 5 %.
