# Reply to your review — empirical evidence + 48 ROM bytes

**Follow-up to**: `docs/CR/2026-06-07_Senior_BASIC10_CSAVE_Investigation.md`
**Date**: 2026-06-07
**Subject**: discriminating test run + raw bytes for verification

---

## 1. Core CPU PC-log test (the one you suggested)

Patch placed directly in `cpu_step()` before the opcode fetch,
unconditional — not in `tape_patches()`, no guard. Target: log
on `PC == $E7FE` and `PC == $E80A` during an Oric-1 `CSAVE "T2"`.

```
$ make SDL2=1 && timeout 20 ./oric1-emu -r roms/basic10.rom \
    --type-keys '5000000:10 PRINT "HI"\n\p1CSAVE "T2"\n\p4'
```

Raw result (stderr):
```
PCLOG: HIT $E80A (cycle=8000347 A=00 X=FF Y=00 SP=F9)
```

**A single line.** `$E7FE` never shows up. `$E80A` shows up once,
towards the end of the CSAVE (~8M cycles, i.e. 8 seconds wall-clock
after start, which is consistent with the sequence `5M cycles → type
"CSAVE T2" → process CSAVE`).

So it is empirical: on my ROM, at that precise moment, the executed
path does NOT go through `$E7FE`. It goes through `$E80A`.

---

## 2. Identity of my ROM

```
md5sum roms/basic10.rom roms/basic11b.rom
  ebe418ec8a6c85d5ac32956c9a96c179  roms/basic10.rom
  a330779c42ad7d0c4ac6ef9e92788ec6  roms/basic11b.rom
```

Sizes: 16384 bytes each (16 KB, standard `$C000-$FFFF` mapping).

If you can compare these hashes with the ROM Oricutron uses in its
`.pch`, that will tell us whether we are talking about the same revision.

---

## 3. The 48 bytes `$E7DB..$E80A` (raw — yours to disassemble)

Raw hex:
```
a59aa49b855f8460a59ca49d85618462082025e720cae6207be52004e828a6a9
e8f0016068684c6bc92063e52039f44c
```

Formatted:
```
$E7DB: A5 9A A4 9B 85 5F 84 60 A5 9C A4 9D 85 61 84 62
$E7EB: 08 20 25 E7 20 CA E6 20 7B E5 20 04 E8 28 A6 A9
$E7FB: E8 F0 01 60 68 68 4C 6B C9 20 63 E5 20 39 F4 4C
```

(The `4C` at `$E80A` is the JMP opcode; the following bytes — `D0 EB`
according to my listing — are at `$E80B..$E80C`, outside the window.)

---

## 4. What bothers me about your PHP/PLP point

You write:
> Pour que ce `PLP` s'exécute, le `JSR $E804` à `$E7F5` doit revenir
> par RTS. Une sous-routine appelée par JSR qui se terminerait par
> JMP $EBD0 ne revient jamais → PLP ne tourne pas → pile déséquilibrée.

*(For this `PLP` to execute, the `JSR $E804` at `$E7F5` must return
via RTS. A subroutine called by JSR that ended with JMP $EBD0 never
returns → PLP does not run → unbalanced stack.)*

I agree on the mechanics. **But**: if `$EBD0` is the warm-start entry
of the BASIC main loop, the standard 6502 convention is that it
starts with:
```asm
LDX #$FF
TXS                    ; reset stack
```
…which discards the whole previous stack. A CSAVE ending with
`JMP $EBD0` would then have an intentional imbalance (the PHP push
is simply abandoned). This pattern is canonical for non-returning
commands (CLOAD, NEW, RUN…) that bounce back to the prompt.

My empirical result (`$E80A` hit, `$E7FE` never) suggests that this is
what happens on my ROM. But I want your opinion: does `$EBD0` really
reset the stack on your side? If so, the PHP/PLP argument no longer
holds as proof that `$E804` must RTS.

---

## 5. Possible hypotheses for the mismatch with Oricutron

Several options, not mutually exclusive:

1. **Different revisions of basic10.rom**. There are at least two known
   BASIC 1.0 ROMs ("$00 ROM" and "$02 ROM", plus an intermediate dump
   floating around collector sites). If my MD5 ≠ Oricutron's, we
   probably do not have the same addresses.

2. **Oricutron uses `$E7FE` but with a side effect**. As you say,
   their `tape_stop_savepatch` closes the `.tap` at `$E7FE`.
   But if their CSAVE flow ALSO goes through `$E80A` BEFORE, they may
   have another hook that writes the last data — or their
   `$E7FE` is hit by an error/recovery path that does not occur on
   my side. I do not have their source at hand to check.

3. **My Oric-1 ROM is partially broken / wrongly mapped**. Unlikely
   (I validated CLOAD, RUN, interactive BASIC, all OK), but to be ruled out.

---

## 6. What I would like you to confirm

- Oricutron's MD5 for `basic10.rom` — match or not with mine.
- A clean disassembly of the 48 bytes above, as you see them.
  Above all: does `$E80A` really end with `JMP $EBD0`, or does
  my alignment drift somewhere between `$E7F5` and `$E80A`?
- Whether `$EBD0` really is the BASIC warm start on Oric-1 (symbol
  table) — which would validate the "JMP-to-main-loop with
  stack reset" pattern.
- Reaction to your proposal "go back to Oricutron-style putbyte
  capture": yes in principle, but before shipping it I want to
  understand WHY my initial capture produced `FF FF` at the start
  of the header — otherwise I risk reintroducing the same bug. Do you
  see a candidate (register A not yet set when putbyte_entry fires,
  alignment problem with the patch address, different ROM)?

---

## 7. What I am NOT shipping until you have seen it

- The `csave_end → $E80A` change is READY but on hold.
- `csave_header_buf = $005E` for Oric-1 is READY but on hold
  (you warned me about the trap: `$5F/$60` is probably reused
  as a working pointer by the data loop → if we read it at
  csave_end, we get the end address, not the start — I will snapshot at
  the entry of `$E57B` instead).
- No Atmos change (your point about `$E93C` received).

Everything is on my local `sprint-34at` branch, not pushed.

---

## 8. A short meta note

I appreciate that you go and check in the real ROM rather than
signing off on my disassemblies. That is also why I wanted a
human review before 34at: on this kind of thing, self-suggestion
from a convincing-but-misaligned listing is exactly the failure
mode I cannot self-detect.

If you validate Option A after a clean disassembly, I ship.
If you validate the option "go back to Oricutron-style putbyte capture",
I roll back the RAM reconstruction (sprints 34aq/34as) for the
TAP layout part — keeping SDIMG, MBR, fd_kind, mkstemp, $02B1/SEC, which
are independent fixes.

— End of reply
