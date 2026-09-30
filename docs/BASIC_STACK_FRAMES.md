# GOSUB / FOR stack frames — ORIC BASIC 1.1 (Atmos)

> Layout of the frames pushed by the `FOR` and `GOSUB` statements onto the
> 6502 hardware stack (`$0100-$01FF`). **Every value in this document was
> extracted empirically from the Phosphoric emulator**, never guessed: each
> field is confirmed by controlled variation of the BASIC program and diffing of
> RAM dumps (differential method). Reference ROM: `roms/basic11b.rom`
> (BASIC 1.1, ORIC Atmos, auto-detected).

## Derivation method (reproducible)

The program is **typed in through the ROM's own tokeniser** (via
`--type-keys-when`), which guarantees authentic tokens — we do not depend on
the `bas2tap` tool (whose C tokeniser is incomplete, see the note at the end). A
sentinel `POKE #2FF,#AA` placed **inside** the subroutine is caught by
`--dump-ram-when`: at dump time, both the `GOSUB` **and** `FOR` frames are
still live on the stack (captured before the `RETURN`).

```bash
PROG='500 FORI=1TO9\n510 GOSUB600\n520 NEXTI\n530 END\n600 POKE767,170\n610 RETURN\nRUN\n'
./oric1-emu-web -r roms/basic11b.rom --headless \
  --type-keys-when BC9A:52:"$PROG" \
  --dump-ram-when 2FF:AA:/tmp/stk.bin -c 30000000
```

The frame is located by **scanning for the marker** (`$8D` for FOR, `$9B` for
GOSUB) in the stack page — robust, and independent of the value of SP at dump
time.

Two essential safeguards in this environment:

1. **Always** use `--headless` + a deterministic exit condition
   (`-c N`, `--dump-ram-at`, `--dump-ram-when`). A launch in actual SDL/real-time
   mode fails silently without a display — this is not an instability of the
   emulator.
2. To observe a text pointer that **crosses a page** (to resolve the endianness
   ambiguity), give the FOR loop a **high** line number (e.g. `500`) with
   **low** filler lines: otherwise the ROM sorts the loop to the start of the
   program and its text never leaves page `$05`.

## FOR frame — 18 bytes

Marker `$8D` (`FOR` token) at the **lowest** address of the frame (pushed last).
Offsets counted from the marker, towards increasing addresses:

| Offset | Size | Field | Byte order | Empirical confirmation |
|-------:|:------:|-------|:------------:|------------------------|
| +0      | 1 | Marker `$8D` (`FOR` token) | —             | constant across all runs |
| +1..2   | 2 | Loop variable pointer (address of the variable's **value** = VARTAB descriptor + 2) | little-endian | follows VARTAB+2: `$0539` → `$0548` → `$0651` |
| +3..7   | 5 | **STEP** value (5-byte MS float, **magnitude only**) | MS float | `1.0`→`2.0`: `81 80 00 00 00` → `82 80 00 00 00` |
| +8      | 1 | **Sign of STEP** | —             | `$01` (positive) → `$FF` (negative) |
| +9..13  | 5 | **TO** limit value (5-byte MS float) | MS float | `9.0`→`7.0`: `84 10 00 00 00` → `83 60 00 00 00` |
| +14..15 | 2 | **Line number** of the `FOR` | little-endian | `10`→`15`→`500`: `0A 00` / `F4 01` |
| +16..17 | 2 | **Text pointer** (position of the terminating `$00` of the FOR statement) | **big-endian** | `$050B`→`$0513`→`$0623` |

Non-trivial points, impossible to guess:

- **Mixed endianness within the same frame.** The variable pointer and the line
  number are stored *little-endian*, but the text pointer is stored
  **big-endian** (high byte at the lower address) — reflecting the ROM's push
  order.
- **The STEP float does not carry its sign.** The magnitude is stored at
  +3..7 and the sign lives in byte **+8** (a ±1 flag, Microsoft BASIC style).
  `STEP 1` and `STEP -1` give the same float `81 80 00 00 00`; only +8 changes
  (`$01` vs `$FF`).

### Annotated example

Program `500 FORI=1TO9 : GOSUB600 : NEXTI` (line 500, text in page `$06`),
captured at the sentinel. FOR frame found at `$01ED`:

```
$01ED: 8D                 FOR marker
$01EE: 51 06              varptr  = $0651  (value of I, VARTAB+2)     [LE]
$01F0: 81 80 00 00 00     STEP    = 1.0    (magnitude)
$01F5: 01                 sign    = +
$01F6: 84 10 00 00 00     TO      = 9.0
$01FB: F4 01              line    = $01F4 = 500                       [LE]
$01FD: 06 23              textptr = $0623  (end of the FOR statement) [BE]
```

## GOSUB frame — 5 bytes

Marker `$9B` (`GOSUB` token) at the lowest address of the frame:

| Offset | Size | Field | Byte order | Empirical confirmation |
|-------:|:------:|-------|:------------:|------------------------|
| +0    | 1 | Marker `$9B` (`GOSUB` token) | —             | constant |
| +1..2 | 2 | **Line number** of the `GOSUB` | little-endian | `20`→`25`→`510`: `14 00` / `FE 01` |
| +3..4 | 2 | Return **text pointer** (within the GOSUB line) | little-endian | `$0511`→`$0519`→`$0629` |

Unlike the FOR frame, the GOSUB text pointer is stored
**little-endian** — a real asymmetry, observed rather than assumed.

### Annotated example

Same capture, GOSUB frame at `$01E6` (line 510, target 600):

```
$01E6: 9B                 GOSUB marker
$01E7: FE 01              line    = $01FE = 510                       [LE]
$01E9: 29 06              textptr = $0629  (resume point in the line) [LE]
```

## Float format (reminder: 5-byte Microsoft BASIC)

`biased exponent` (bias `$80`), followed by 4 mantissa bytes (implicit most
significant bit = 1). Value ≈ `(1 + mantisse/2^32) × 2^(exposant-129)`
(mantisse = mantissa, exposant = exponent). Examples verified in the frames
above:

| Bytes | Value |
|--------|:------:|
| `81 80 00 00 00` | 1.0 |
| `82 80 00 00 00` | 2.0 |
| `83 60 00 00 00` | 7.0 |
| `84 10 00 00 00` | 9.0 |

## Note — the `bas2tap` tool

The C tool `tools/bas2tap.c` (via `tap_from_basic` → `basic_tokenize_line`) is
**not** a stub, but its tokeniser is **incomplete**: at least the case of
"keyword glued to a letter" (`FORI`) leaves the keyword as raw ASCII instead of
emitting the token, producing a `.tap` that does not run correctly — while
still reporting "Conversion successful!". Identified technical debt; do **not**
rely on it to generate test programs. The reliable method is to let the ROM do
the tokenising via `--type-keys` (see above).
