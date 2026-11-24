# Task #32 — `FF` hypothesis confirmed empirically

**Follows**: your v1.16.45 review (`docs/CR/2026-06-07_Senior_BASIC10_CSAVE_*`), section §5 "Why `FF FF` at the start of the header (capture 34aq)".
**Date**: 2026-06-07
**Verdict**: **your hypothesis was right byte for byte.** My initial capture was faithful. The `FF`s come from the ROM dumping uninitialised ZP.

---

## 1. Discriminating test run

Temporary instrumentation in `tape_patches()` (branch
`investigate-putbyte-ff`, not merged):

1. At Atmos `writefileheader_entry` (`$E607`): dump of the 9 ZP bytes of the
   staging buffer `$02A8..$02B0`.
2. At Atmos `putbyte_entry` (`$E65E`): log `(count, A, X, Y)` for the
   first 12 calls.

Scenario: ORIC Atmos cold boot → `10 PRINT "HI"` → `CSAVE "T1"`.

---

## 2. Raw results

```
INVESTIG_FF: WFH entry buffer dump @$02A8: FF 01 05 0E 05 00 00 FF FF
INVESTIG_FF: putbyte #1 A=$24 X=$11 Y=$01     ← sync (immediate, not ZP)
INVESTIG_FF: putbyte #2 A=$FF X=$09 Y=$01
INVESTIG_FF: putbyte #3 A=$FF X=$08 Y=$01
INVESTIG_FF: putbyte #4 A=$00 X=$07 Y=$01
INVESTIG_FF: putbyte #5 A=$00 X=$06 Y=$01
INVESTIG_FF: putbyte #6 A=$05 X=$05 Y=$01
INVESTIG_FF: putbyte #7 A=$0E X=$04 Y=$01
INVESTIG_FF: putbyte #8 A=$05 X=$03 Y=$01
INVESTIG_FF: putbyte #9 A=$01 X=$02 Y=$01
INVESTIG_FF: putbyte #10 A=$FF X=$01 Y=$01     ← null sep but ZP=$FF
INVESTIG_FF: putbyte #11 A=$54 X=$00 Y=$01     ← 'T' (filename)
INVESTIG_FF: putbyte #12 A=$31 X=$01 Y=$01     ← '1'
```

---

## 3. 1:1 correlation buffer ↔ putbyte

`WriteFileHeader` reads in reverse with `LDX #9` / `LDA $02A7,X` / `DEX`.
So X=9 reads `$02B0`, X=8 reads `$02AF`, etc. down to X=1 reading `$02A8`.

| putbyte # | X | ZP addr | ZP byte | A captured | Match |
|-----------|---|---------|---------|-------------|-------|
| #2 | 9 | `$02B0` | `FF` | `FF` | ✅ |
| #3 | 8 | `$02AF` | `FF` | `FF` | ✅ |
| #4 | 7 | `$02AE` | `00` | `00` | ✅ |
| #5 | 6 | `$02AD` | `00` | `00` | ✅ |
| #6 | 5 | `$02AC` | `05` | `05` | ✅ |
| #7 | 4 | `$02AB` | `0E` | `0E` | ✅ |
| #8 | 3 | `$02AA` | `05` | `05` | ✅ |
| #9 | 2 | `$02A9` | `01` | `01` | ✅ |
| #10 | 1 | `$02A8` | `FF` | `FF` | ✅ |

**Perfect correlation, on all 9 bytes.** Each putbyte receives in
`A` exactly what the corresponding ZP address contains.

---

## 4. Functional mapping

| ZP addr | "Official" role | Observed value | Initialised by BASIC? |
|---------|----------------|-----------------|------------------|
| `$02A8` | null sep | `FF` | **no** (left as is) |
| `$02A9` | start_lo | `01` | yes (= `$01` of `$0501`) |
| `$02AA` | start_hi | `05` | yes (= `$05` of `$0501`) |
| `$02AB` | end_lo | `0E` | yes |
| `$02AC` | end_hi | `05` | yes |
| `$02AD` | auto-flag | `00` | yes (CSAVE without `,A` → 0) |
| `$02AE` | type | `00` | yes (BASIC program) |
| `$02AF` | padding[1] | `FF` | **no** |
| `$02B0` | padding[0] | `FF` | **no** |

Three positions (`$02A8`, `$02AF`, `$02B0`) are left untouched by
the ROM before calling `WriteFileHeader`. If they contained `00` at
the cold boot of a real Oric (RAM initialised to zero), these bytes would be
`00 00 00` on tape. On Phosphoric, the RAM init uses a mixed
pattern (`128×$00 + 128×$FF` per page, Oricutron compliance) → the
addresses `$02A8/$02AF/$02B0` fall into the `FF` half.

---

## 5. Implications

### For your point §5 of the v1.16.45 review

You wrote:
> Your capture was faithful. It is the ROM that emitted `FF` because the
> reserved bytes of the staging buffer were not reset to zero on
> your emulator. The "unreliable capture" that pushed you towards
> reconstruction may not have been a capture bug at all.

**Confirmed byte for byte.** It was not a capture bug. It is
the actual behaviour of the Atmos BASIC 1.1 ROM. On a real Oric
cold-booted with zeroed RAM, these bytes would be `00`. On Phosphoric,
they come out as `FF` because of the Oricutron-compatible RAM init pattern.

### Consequence for the TAP strategy

The choice between **reconstruction** (sprints 34aq/34as/34at) and
**Oricutron-style capture** depends on the semantics we want:

| | Reconstruction (current) | Oricutron-style capture |
|---|---|---|
| Bytes emitted | Standard layout `00 00 00 C7|00 ...` | Exactly what the ROM emits |
| Hardware fidelity | Low (we normalise) | Maximum |
| TAP predictability | High | Depends on RAM state (FF or 00) |
| ROM-version coupling | Low (abstract header buffer) | None (purely data-driven) |
| CLOAD compatibility | Validated (round-trip OK) | To be validated |
| LOC | ~150 (current sprint 34aq) | ~50 (just an `fputc(cpu.A)`) |

In my view, **keeping reconstruction** is the right choice for
Phosphoric:

- The TAPs produced are byte-compatible with the canonical Oric format
  (vs AIGLE.TAP), not with RAM-init artefacts.
- The snapshot at `writefileheader_entry` (34at) covers BASIC + machine code.
- Uninitialised ZP `$02A8/$02AF/$02B0` → reconstruction forces `00`,
  which matches the behaviour of a real Oric at cold boot.

But the capture option remains on the table if you prefer. The sprint 34at
fix keeps both paths viable.

### Consequence for `init_ram`

Secondary question: Phosphoric could initialise the critical ZP bytes
(`$02A8`, `$02AF`, `$02B0`) to `00` at boot to match an Oric
cold start. Trivial patch, but fragile semantics (why these 3 and not
others?). The senior might prefer "leave the RAM as it is",
in which case reconstruction remains the right approach.

---

## 6. Decision expected

I understood your implicit point correctly: "**IF the discriminating test shows
A==FF at the pad positions and the ZP itself is `FF`, then your
capture was sound — no need to migrate to Oricutron-style, the
initial bug was not one.**"

The test shows exactly that. So the argument for migrating to
capture (fewer LOC, simplicity) is still valid but **is no longer
forced** by a bug. It is an architectural choice.

My vote: **keep reconstruction**, ship sprint 34at as
is, close the task #32 debt with this report.

If you approve, I merge everything into main (already done for 34at, this report
will be the next commit).

---

## 7. Instrumentation removed

The `investigate-putbyte-ff` commit contains the temporary `log_info` calls.
Before merging I rebase to keep only the report (the logs are already
removed from the code).

— End of the task #32 report
