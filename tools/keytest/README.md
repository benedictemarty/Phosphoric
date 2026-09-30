# Keyboard validation — key detection (ORIC-1 / Atmos)

Tools for detecting and automatically validating the Oric keyboard under Phosphoric,
covering **every key and combination** in **ORIC-1 (BASIC 1.0)** and
**Atmos (BASIC 1.1)** modes.

## Files

| File | Role |
|---|---|
| `keydetect.bas` | **Interactive** BASIC program: displays the decimal code of each key pressed (`KEY$`). Demonstration/diagnostic tool. |
| `keytest.bas` | **Capture** BASIC program: `POKE`s the ASCII code of each detected key into a buffer at `#2000`. Used by the harness. |
| `validate_keys.sh` | **Automated validation** harness: injects a key sequence via `--type-keys`, dumps the RAM, compares it with the expected vector, for both ROMs. |

## Usage

```bash
# Interactive demonstration (shows the codes on screen)
make tools                                   # build bas2tap
./bas2tap tools/keytest/keydetect.bas -o /tmp/kd.tap --auto-run
./oric1-emu -r roms/basic11b.rom -t /tmp/kd.tap -f   # then RUN, type some keys

# Full automated validation (ORIC-1 + Atmos)
./tools/keytest/validate_keys.sh
```

## How the harness works

`keytest.bas` runs inside the emulator:

```basic
10 A=#2000
20 K$=KEY$ : IF K$="" THEN 20      : REM wait for a key
30 POKE A,ASC(K$) : A=A+1          : REM store the code in the buffer
40 K$=KEY$ : IF K$<>"" THEN 40     : REM wait for release
50 GOTO 20
```

The harness types `RUN`, injects the test sequence, then dumps the RAM and compares
the bytes at `#2000…` with the expected vector.

## Validated coverage (86 keys/combos × 2 ROMs = 172 checks, 0 errors)

| Category | Detail | Code(s) |
|---|---|---|
| Letters | A–Z (CAPS on → upper case) | `$41`–`$5A` |
| Digits | 0–9 | `$30`–`$39` |
| Symbols | `! " # $ % & + - = . , /` | `$21`–`$2F` |
| Space | SPACE | `$20` |
| RETURN | `\n` | `$0D` |
| ESC | `\e` | `$1B` |
| Arrows | `\u \d \l \r` (up/down/left/right) | `$0B $0A $08 $09` |
| CTRL+letter | `\Ca`…`\Cz` (except C) | `$01`–`$1A` |
| FUNCT+key | `\Fx` | **base key** (the BASIC ROM ignores FUNCT) |
| Left/right SHIFT | `\Lx` / `\Rx` | shift+digit=symbol, shift+letter=upper case |

## Notable behaviours discovered (validated empirically)

- **CTRL+C = BREAK**: interrupts the BASIC program. Cannot be tested via `KEY$`
  in a loop; validated separately on the LOCI File Manager (`keyb_char`=`$03`).
- **FUNCT ignored by the BASIC ROM**: `FUNCT+1`→`'1'`, `FUNCT+A`→`'A'`. FUNCT
  only matters for software that reads the matrix (e.g. the LOCI File Manager,
  where `FUNCT+1`→`KEY_F1`). Hardware position of FUNCT = **col 5 / row 4**.
- **Left and right SHIFT**: **distinct** hardware positions
  (LSHIFT col 4 / RSHIFT col 7, verified on LFMV2 `keyb_matrix[4]` vs `[7]`),
  but **equivalent for the ROM** (`\L1`=`\R1`=`'!'`).
- **`shift+2` = `'@'`** (not `'"'`) on the Oric keyboard.
- **Arrows**: UP=`$0B`, DOWN=`$0A`, LEFT=`$08`, RIGHT=`$09`.
- **ORIC-1 and Atmos identical** across the whole tested set (`KEY$` present and
  consistent in BASIC 1.0 as in 1.1).

## Known limitations

- Two **consecutive modifier combos on the same base key** (e.g. `\L1\R1`)
  may merge: the ROM's keyboard debounce filters out a one-frame release
  (~20 ms) as noise. Workaround: insert `\pN` or use distinct
  base keys.
- **DEL/BACKSPACE** has no dedicated `--type-keys` escape (not covered here).
