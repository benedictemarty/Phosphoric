# Investigation: CSAVE BASIC 1.0 (ORIC-1) — `csave_end` never fires

**For**: senior engineer, follow-up to the v1.16.44 → v1.16.45 review
**Date**: 2026-06-07
**Versions affected**: everything since v1.16.43 (sprint 34aq, first
implementation of TAP reconstruction at `csave_end`)
**Status**: root cause identified by disassembly, fix proposed, awaiting
your OK

---

## 1. Symptom

On Atmos (BASIC 1.1):
```
CSAVE "T1" → CSAVE: built TAP T1.tap (30 bytes, prog $0501-$050E) ✓
CLOAD "T1" → program loaded, LIST OK
```

On Oric-1 (BASIC 1.0) with the same scenario:
```
CSAVE "T2" → file T2.tap created but 0 bytes
CLOAD "T2" → endless "Searching ..."
```

---

## 2. Reachability of the traps

`log_info` instrumentation at the very beginning of each branch of the
`tape_patches()` handler (unpushed commits):

### Atmos (BASIC 1.1) — working reference
```
TRACE: writeleader_entry $E75A (PC=$E75A SP=$F7)
CSAVE: saving to T1.tap
TRACE: putbyte_entry $E65E cnt=0   ← sync $24
TRACE: putbyte_entry $E65E cnt=1
TRACE: putbyte_entry $E65E cnt=2
TRACE: putbyte_entry $E65E cnt=3
TRACE: putbyte_entry $E65E cnt=4
TRACE: csave_end $E93C fired       ← ✓
CSAVE: built TAP T1.tap (30 bytes, ...)
```

### Oric-1 (BASIC 1.0) — not working
```
TRACE: writeleader_entry $E6BA (PC=$E6BA SP=$F7)
CSAVE: saving to T2.tap
TRACE: putbyte_entry $E5C6 cnt=0
TRACE: putbyte_entry $E5C6 cnt=1
TRACE: putbyte_entry $E5C6 cnt=2
TRACE: putbyte_entry $E5C6 cnt=3
TRACE: putbyte_entry $E5C6 cnt=4
<nothing — csave_end $E7FE never shows up>
```

Finding: writeleader and putbyte fire normally. The trap
`csave_end = $E7FE` never does.

---

## 3. Disassembly of the Oric-1 CSAVE chain

### Outer CSAVE routine at `$E7DB`

```asm
$E7DB: A5 9A         LDA $9A         ; TXTTAB lo
$E7DD: A4 9B         LDY $9B         ; TXTTAB hi
$E7DF: 85 5F         STA $5F         ; copy TXTTAB → $5F/$60
$E7E1: 84 60         STY $60
$E7E3: A5 9C         LDA $9C         ; VARTAB lo
$E7E5: A4 9D         LDY $9D         ; VARTAB hi
$E7E7: 85 61         STA $61         ; copy VARTAB → $61/$62
$E7E9: 84 62         STY $62
$E7EB: 08            PHP
$E7EC: 20 25 E7      JSR $E725       ; ?? (setup)
$E7EF: 20 CA E6      JSR $E6CA       ; SetupTapeOutput (PSG init)
$E7F2: 20 7B E5      JSR $E57B       ; WriteFileHeader → writes leader + sync + header + filename
$E7F5: 20 04 E8      JSR $E804       ; WriteDataBlock — does not return!
$E7F8: 28            PLP
$E7F9: A6 A9         LDX $A9
$E7FB: E8            INX
$E7FC: F0 01         BEQ $E7FF
$E7FE: 60            RTS              ← current csave_end (never reached)
$E7FF: 68            PLA
$E800: 68            PLA
$E801: 4C 6B C9      JMP $C96B
```

### The `WriteDataBlock` subroutine at `$E804` (the "keystone")

```asm
$E804: 20 63 E5      JSR $E563        ; writes the program data
$E807: 20 39 F4      JSR $F439        ; cleanup?
$E80A: 4C D0 EB      JMP $EBD0        ; ← JMP! never returns!
```

**Root cause**: `$E80A` is a `JMP`, not an `RTS`. Control NEVER
returns to the caller's `JSR $E804` at `$E7F5`, and therefore the `RTS` at
`$E7FE` is never executed.

`$EBD0` is very probably the entry point of the BASIC "Ready" prompt
(to be confirmed by you if you have the ORIC-1 symbol table, but
the `JMP $EBD0` after cleanup pattern is canonical for non-returning
commands like NEW, CLOAD, CSAVE that bounce straight back to the
main loop).

---

## 4. Comparison with Atmos (BASIC 1.1)

On Atmos, `csave_end = $E93C` fires normally. Without the full symbol
table, I have not confirmed whether `$E93C` is an `RTS` or whether
Atmos has a different structure (maybe a clean `RTS`, or maybe
my trap fires on an intermediate instruction and the routine carries on).

Question for you: does the Atmos `CSAVE` really return cleanly through an RTS
chain, or does my trap at `$E93C` fire by chance because it is
a common instruction on the path and `csave_byte_count` lets me
catch the first hit?

---

## 5. Fixes considered

### Option A — Trap on the JMP at `$E80A`

```c
.csave_end = 0xE80A,   /* BASIC 1.0 — JMP $EBD0 marks end of CSAVE work */
```

Advantage: minimally invasive, follows exactly the semantics of "end
of all CSAVE output". My code rebuilds the TAP before the JMP
executes, so the file and `tapebuf` are ready when BASIC returns to the
prompt.

Risk: none seen. The `JMP` is at the end of the chain, not in the middle of a loop.
No side effect on CPU registers (we intercept before the JMP
but leave PC unchanged unless explicitly exiting).

### Option B — Trap on the `JSR $F439` at `$E807`

```c
.csave_end = 0xE807,
```

Fires after `JSR $E563` (writing of the program data) but before
the `JSR $F439` cleanup. Gives an earlier point, useful if we still want
to manipulate RAM before BASIC does its cleanup. But we
intercept an instruction in the middle of a cleanup → semantically less
clean than Option A.

### Option C — Extend the `csave_end` mechanism to several addresses

The `rom_patches_t.csave_end` field is a single `uint16_t`. If a ROM
finished its CSAVE via several paths (for example a normal JMP or an
error RTS), a single point would not be enough. A `uint16_t csave_ends[4]`
+ 0-terminated count would give more flexibility. **Not justified
for 34at**, just something to keep in mind.

### Option D — Source the Oric-1 staging buffer

Additional discovery during the investigation: the routine
`$E57B WriteFileHeader` reads its header from the buffer **`$5E..$66` (zero page)**:

```asm
$E57B: 20 BA E6      JSR $E6BA       ; writeleader (writes 6 leader bytes)
$E57E: A9 24         LDA #$24        ; sync
$E580: 20 C6 E5      JSR $E5C6       ; PutByte
$E583: A2 09         LDX #$09        ; X = 9
$E585: B5 5D         LDA $5D,X       ; ← header @ $5D+1..$5D+9 = $5E..$66
$E587: 20 C6 E5      JSR $E5C6       ; PutByte each byte
$E58A: CA            DEX
$E58B: D0 F8         BNE $E585
$E58D: B5 35         LDA $35,X       ; X=0 → filename @ $0035
$E58F: F0 06         BEQ +6
$E591: 20 C6 E5      JSR $E5C6       ; PutByte filename char
$E594: E8            INX
$E595: D0 F6         BNE $E58D
```

**The Oric-1 buffer is in ZP at `$5E..$66`** (9 bytes, read in reverse via
LDX#9 / DEX / BNE — same pattern as Atmos). Filename at `$0035` (like
our historical fallback).

Memory → tape mapping (X=9..1):

| Address | Tape byte | Meaning (presumed, to be confirmed) |
|---------|-----------|------------------------------|
| `$66` | #1 | padding |
| `$65` | #2 | padding |
| `$64` | #3 | type |
| `$63` | #4 | auto-flag |
| `$62` | #5 | end_hi |
| `$61` | #6 | end_lo |
| `$60` | #7 | start_hi |
| `$5F` | #8 | start_lo |
| `$5E` | #9 | null sep |

Confirmation: the outer CSAVE routine at `$E7DB` copies `TXTTAB → $5F/$60`
and `VARTAB → $61/$62` just before calling `$E57B`. So `$5F/$60 = start`
and `$61/$62 = end`. This matches the mapping above.

→ We can give Oric-1 its own proper `csave_header_buf = 0x005E` in
`rom_patches_t`, just as Atmos has `0x02A8`. This subsumes the
TXTTAB/VARTAB fallback and also works for Oric-1 machine-code CSAVEs
(`,A,E`) if the ROM supports them.

---

## 6. Proposed plan of attack for 34at

1. **Fix Oric-1 `csave_end`**: `0xE7FE` → `0xE80A` (Option A)
2. **Add `csave_header_buf = 0x005E`** for `rom_patches_basic10`
3. **Test the BASIC 1.0 round trip**: `10 PRINT "HI"` → `CSAVE "T2"` →
   `NEW` → `CLOAD "T2"` → `LIST`
4. **Validate that TAP `T2.tap` matches the AIGLE.TAP format** byte for byte
   on the header (except addresses)
5. **Atmos regression**: make sure nothing breaks

Open questions for you:

- Can you confirm `$EBD0` = main loop entry on Oric-1? If so, the
  JMP-trap fix pattern is right. Otherwise, perhaps another point
  is needed.

- On Atmos, is `$E93C` really the RTS of the outer CSAVE routine,
  or does the same JMP-to-main-loop structure exist, with my
  trap firing by accident on an instruction in the middle? If so,
  we should perhaps migrate Atmos to the same strategy
  (trap on the final JMP, not on the presumed RTS) for consistency.

- Is there a case where `BEQ $E7FF` at `$E7FC` (on Oric-1) is taken
  (`X+1 == 0`)? If so, the code at `$E7FF: PLA PLA JMP $C96B` is an
  alternative exit path (error?). My trap at `$E80A` misses it.
  Probably harmless (ROM error = no TAP to rebuild), but
  I prefer to check with you.

---

## 7. Quick metrics

| Item | Value |
|------|--------|
| Versions affected | All since 1.16.43 (sprint 34aq) |
| Severity | Non-blocking (Atmos works, the main LOCI use goes through Atmos), but embarrassing for Oric-1 users |
| Regression tests needed | 470 existing tests + 1 new Oric-1 round-trip E2E test |
| Estimated fix LOC | < 10 (2 lines in `rom_patches_basic10` + tests) |
| Disassembly needed | Done, attached |

---

## 8. Appendices

### A.1 — Disassembled `basic10.rom` bytes

```
$E5C6  PutByte:
       85 2F           STA $2F          ; byte to write stored in ZP
       8A 48           TXA PHA
       98 48           TYA PHA
       20 27 E6        JSR $E627
       ... (bit-bang parity logic)
$E5F2  60              RTS              ; putbyte_end

$E6BA  WriteLeader (6 bytes):
       A2 02           LDX #$02         ; 2 outer iterations
       A0 03           LDY #$03         ; 3 inner iterations
       A9 16           LDA #$16
       20 C6 E5        JSR $E5C6        ; PutByte
       88              DEY
       D0 F8           BNE $E6BE
       CA              DEX
       D0 F5           BNE $E6BE
$E6C9  60              RTS              ; writeleader_end

$E57B  WriteFileHeader:
       20 BA E6        JSR $E6BA        ; → 6 leaders
       A9 24           LDA #$24
       20 C6 E5        JSR $E5C6        ; → sync $24
       A2 09           LDX #$09
$E585: B5 5D           LDA $5D,X        ; ← ZP BUFFER $5E..$66
       20 C6 E5        JSR $E5C6
       CA              DEX
       D0 F8           BNE $E585
       B5 35           LDA $35,X        ; ← FILENAME $0035..
       F0 06           BEQ +6
       20 C6 E5        JSR $E5C6
       E8              INX
       D0 F6           BNE $E58D
       20 C6 E5        JSR $E5C6        ; final separator?

$E7DB  CSAVE outer routine:
       A5 9A           LDA $9A          ; TXTTAB → $5F/$60
       A4 9B           LDY $9B
       85 5F           STA $5F
       84 60           STY $60
       A5 9C           LDA $9C          ; VARTAB → $61/$62
       A4 9D           LDY $9D
       85 61           STA $61
       84 62           STY $62
       08              PHP
       20 25 E7        JSR $E725        ; ??
       20 CA E6        JSR $E6CA        ; SetupTapeOutput (PSG)
       20 7B E5        JSR $E57B        ; WriteFileHeader
       20 04 E8        JSR $E804        ; ↓ NEVER returns
$E7F8: 28              PLP              ; ← dead code on the normal path
       A6 A9           LDX $A9
       E8              INX
       F0 01           BEQ $E7FF
$E7FE: 60              RTS              ; ← current csave_end, never reached
$E7FF: 68 68           PLA PLA          ; error path?
       4C 6B C9        JMP $C96B

$E804  WriteDataBlock:
       20 63 E5        JSR $E563        ; writes program data
       20 39 F4        JSR $F439
$E80A: 4C D0 EB        JMP $EBD0        ; ← REAL end of CSAVE
```

### A.2 — Atmos staging buffer comparison

| Sub | Oric-1 (BASIC 1.0) | Atmos (BASIC 1.1) |
|-----|--------------------|---------------------|
| WriteFileHeader | `$E57B` | `$E607` |
| Header buffer base | `$005E` (ZP) | `$02A8` |
| Filename buffer base | `$0035` | `$027F` |
| Leader bytes count | 2×3 = 6 | (TBD, probably more) |
| putbyte_entry | `$E5C6` | `$E65E` |
| putbyte_end | `$E5F2` | `$E68A` |
| csave_end (current, buggy) | `$E7FE` | `$E93C` (seems OK?) |
| csave_end (proposed) | `$E80A` | (to be confirmed) |

---

**Explicit request**: your OK on Option A (`csave_end = $E80A`) +
the Oric-1 buffer table (`csave_header_buf = $005E`), and your answers
to the 3 open questions in section 6.

— End of the investigation
