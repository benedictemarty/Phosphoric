# SEDORIC disk format & tooling

> Primary reference: the **SEDORIC 3.0** manual (`sedna3_0.pdf`), cross-checked with
> A. Chéramy's « SEDORIC 3.0 à NU » and the tooling of the SCUMM-Oric / scoop-oric project.
> **Nothing is made up**: the offsets below are the manual's, verified
> byte-exact against the descriptor dump given as an example by the manual itself.

## 1. MFM_DISK container

256-byte header: `"MFM_DISK"` + `sides` (u32 LE) + `tracks` (u32 LE) + geometry (u32 LE).
Then one **6400-byte** track block per (side, track), in **SIDE-MAJOR** order:
all tracks of side 0, then all tracks of side 1
(`bloc = side * tracks + track`; see `src/storage/sedoric.c`, `track_idx = s*tracks+t`).

Each track block: initial gap `60×$4E`, then per sector
`12×$00 | A1 A1 A1 FE [trk side sec size] CRC | 22×$4E | 12×$00 | A1 A1 A1 FB [256 data] CRC | 38×$4E`,
padded with `$4E` up to 6400 bytes. CRC-16/CCITT (poly `$1021`, init `$FFFF`) over the
4 mark bytes + the data.

A **RAW** image (concatenated 256-byte sectors, same side-major order, no framing)
is accepted by `sedoric-info` and produced by `oric1-emu --disk-create` /
`tools/sedoric_inject.py`. `tools/dsk_raw2mfm.py` converts RAW → MFM_DISK.

## 2. System sectors (track 20)

| Sector | Role | Key fields |
|---|---|---|
| **s1** System | disk name + autoexec | `+9..+29` disk name (21 bytes) · `+0x1E..+0x59` **INIST** (60 bytes: ASCII boot commands, separated by `:`, terminated by `#00`) |
| **s2** VTOC/bitmap | counters | `+2,+3` free sectors (LE) · `+4,+5` number of files (LE) |
| **s4** Directory | catalogue | `+0,+1` link to next dir sector (0=end) · `+2` high-water mark · 16-byte entries starting at `+16` |

Directory entry (16 bytes): `name[9]` `ext[3]` `track` `sector` `nsec` `status`
(V4 valid file = `$40`; bit 7 = deleted).

> Note (observed on Master disks, see `analyze_sedoric_vtoc.py`): the VTOC
> `free`/`files` counter can be an **80-track template** stamped onto a smaller
> physical image — it is a model, not a recomputed state. `sedoric-info`
> therefore reports both the VTOC counter and the number of files actually
> walked in the catalogue.

## 3. File descriptor sector

Pointed to by the directory entry (`track`,`sector`). **Real dump from the manual** (descriptor
of system file BANQUE no. 7, p.16):

```
 0 1 2 3 4 5 6 7 8 9 A B C D E F
00 00 FF 40 00 C4 FF C7 00 00 04 00 05 0B 05 0C 05 0D 05 0E 00 00
```

| Offset | Field | Example |
|---|---|---|
| `+0,+1` | link to next descriptor (00 00 = none) | `00 00` |
| `+2` | first-descriptor marker | `FF` |
| `+3` | **type**: b0=AUTO, b6=data block, b7=BASIC → `$40`=ML, `$41`=AUTO ML | `40` |
| `+4,+5` | load address (LE) | `00 C4` = $C400 |
| `+6,+7` | end address (LE) | `FF C7` = $C7FF |
| `+8,+9` | **execution address if AUTO** (LE) | `00 00` |
| `+0xA,+0xB` | **number of data sectors** (LE) | `04 00` = 4 |
| `+0xC..` | data sector map `(track,sector)×n`, terminated by `00 00` | `05 0B 05 0C 05 0D 05 0E 00 00` |

This dump **corrects** the old implementation of `tap2sedoric`, which left `+3=0`
(no type) and wrote the sector count at `+9,+10` **big-endian**
(overlapping the execution address). Fixed since v1.64.0.

### Chained descriptors (files > 122 sectors) — validated

A descriptor sector holds only part of the map: the **1st
descriptor** carries the header (12 bytes) then the map from `+0x0C` (**122 pairs**
max, `0x0C..0xFE`); each **following descriptor** carries only the link `+0,+1`
then the map from `+0x02` (**127 pairs** max). The link `+0,+1` points to the
next descriptor (`00 00` = last). `+0xA,+0xB` (1st descriptor) = **total**
number of data sectors; the reader follows the descriptor chain and stops at
this count. **Validated in situ**: a 150-sector file (2 descriptors) fully loaded
by `LOAD` (sectors of the 2nd descriptor included). `tap2sedoric` and
`sedoric_inject.py` chain descriptors since v1.65.0 (before: single descriptor, crash
beyond ~121 sectors).

## 4. Tools

| Tool | Role |
|---|---|
| `tap2sedoric <in.tap> -o out.dsk -b base.dsk [-n NAME.EXT] [-a] [-e EXEC] [-i "INIST"]` | injects a CSAVE `.tap` into a Sedoric MFM disk (file + conformant descriptor + dir entry + VTOC; `-a`/`-e` AUTO, `-i` boot autoexec) |
| `sedoric-info <disk.dsk> [--check FREE:FILES]` | inspects the VTOC, disk name, INIST, catalogue and decoded descriptors; `--check` = regression guard on the counters |
| `tools/sedoric_inject.py <raw_in> <bin> <load> NAME.EXT <raw_out> [tracks] [sectors] [init] [exec]` | injection into RAW (direct offsets) |
| `tools/dsk_raw2mfm.py <raw> <out.dsk> [order] [sides] [tracks] [sectors]` | RAW → MFM_DISK (side-major blocks) |

Typical RAW chain: `oric1-emu --disk-create base.raw` (then INIT at boot for a
real VTOC) → `sedoric_inject.py` → `dsk_raw2mfm.py` → `oric1-emu --disk-rom microdis.rom -d`.

## 5. Multi-file injection (limitation lifted)

Historically, `tap2sedoric` **and** `sedoric_inject.py` always allocated
from track 21 sector 1 without checking which sectors were already in use: a
**second injection onto the same disk put its descriptor in the same sector
as the first one and overwrote it** (the previous file vanished from `LOADM`).

Since v1.64.0, both tools **walk the catalogue (full chain) and
the sector maps of the existing descriptors** — including the **chained catalogue
sectors themselves** — to mark used sectors before
allocating. Two successive injections get distinct descriptors.
Verified by `make test-sedoric-tools` (`tests/integration/test_sedoric_inject.sh`).

### Directory sector chaining (implemented)

When the current catalogue sector is full (15 entries), the tools
**walk the chain** via the `+0,+1` link; if the whole chain is full, they
**allocate a free sector, initialise it as a blank catalogue and chain it in**.
Since catalogue sectors are located **by track/sector link** (not by a fixed
area, manual ANNEXE 7), a chained catalogue can live on any free
sector. **Validated in situ**: a file placed in the 2nd catalogue sector
(including one injected *after* this sector was created) is listed by `DIR` and
loaded+executed by `LOAD"NAME"` in the emulator.

## 6. Running an ML file under Sedoric (recipe **validated in situ**)

Loading/execution verified in the emulator (bare boot + `LOAD` → `$5000`
loaded and code executed). Pitfalls encountered and settled:

| Command / case | Result |
|---|---|
| typing `PROBE` (bare name) at Ready | `?SYNTAX ERROR` — the bare name only launches an AUTO **BASIC** file (see `MENU`) |
| `LOADM"PROBE"` | `?TYPE MISMATCH` — `LOADM` is the **cassette ROM** command, not Sedoric |
| `CLOAD"PROBE"` | no error but **loads nothing** (`CLOAD`/`CLOAD,J` = BASIC files) |
| `LOAD"PROBE"` on `.BIN` | `?FILE NOT FOUND` — **`.COM` is the default extension** of `LOAD` |
| `LOAD"PROBE",J` on AUTO | `BREAK ON BYTE #5000` — `,J` conflicts with the AUTO flag |
| **`LOAD"PROBE"`** on **`.COM` AUTO (type $41)** | **loads AND executes** (the AUTO flag jumps to the execution address) ✅ |

Recipe: inject as **`.COM` AUTO** (`tap2sedoric ... -n NAME.COM -a -e EXEC`), then
at Ready: **`LOAD"NAME"`**. The Sedoric command is **`LOAD`** (auto-detects
BASIC vs binary via the type byte `+3`), with options `,A` (address) and `,J`
(jump), see manual §VSALO1 (`C04E`: b6=`,A`, b7=`,J`).

### Bootable "bare" Sedoric Master (deterministic)

To validate a home-made `.COM`, you need a disk that boots to `Ready` **with no
competing application**. `tools/make_bootable_sedoric.sh` (INIT driven by
`--type-keys`) is **fragile** (fails if the master system disk boots into a
menu, e.g. `SEDO40u`). Robust method, with no timing, via the dedicated tool:

```
tools/sedoric_mkbare.py disks/SEDO40u.DSK bare.dsk        # neutralise the INIST
tools/sedoric_mkbare.py disks/SEDO40u.DSK auto.dsk 'LOAD"PROBE"'   # or autorun
```

It **neutralises (or replaces) the INIST** (track 20 sector 1, `+0x1E..+0x59`, MFM
sector CRC recomputed; RAW handled too) → the disk drops to a bare `SEDORIC V4.0 /
Ready` while remaining bootable. Verified on `SEDO40u` (INIST
`CLS:MENU.LNG:MENU` → bare Ready).

> VTOC note: `DIR` on such a Master shows `D/80/17` (80-track template) on
> a 42-track physical image — safe coexistence as long as the injected
> files stay within the physical tracks (see `analyze_sedoric_vtoc`).

### Loading a file from machine code (recipe **validated in situ**)

From a running ML program (launched by `LOAD`), the **Sedoric RAM
overlay is not mapped**: calling the DOS routines directly (`$DB2D`
search, `$E0EA` read) **crashes** (they land in the BASIC ROM). The robust
way goes through the **"!" vector `$0467`** (in low RAM `$04xx`, always
mapped), which switches to the overlay, runs the **SEDORIC interpreter** (`$D3AE`)
on the command line pointed to by **TXTPTR (`$00E9/$00EA`)**, then switches back
to the ROM and does `RTS` (manual: `!` vector at `$0467`, switch stub
`$0477` → `STA $0314`).

```asm
        LDA #<CMD : STA $E9        ; TXTPTR = address of the command line
        LDA #>CMD : STA $EA
        JSR $0467                  ; "!" vector: executes the command (overlay handled)
        ; ... file loaded; normal continuation ...
        RTS
CMD:    .byte "LOAD\"SCDATA\"", 0  ; SEDORIC line terminated by $00
```

This is the ML equivalent of typing `!LOAD"SCDATA"`. **Validated**: `LOAD"SCDATA"`
called this way loads the file (`$6000..` = exact data), and the calling
program resumes normally.

> **Pitfall (isolated by a test matrix)**: the line must be terminated by
> **`$00`**, **never by CR (`$0D`)**. With CR the `$D3AE` interpreter does not
> complete and **does not `RTS`** (file not loaded, TXTPTR does not advance, the
> calling program never regains control). The location of the buffer (TIB
> `$0035` or private RAM) does not matter; TXTPTR points to the **first
> character** (the `L`), not `cmd-1`. The low-level sequence `$DB2D` (search →
POSNMP `$C025`/POSNMS `$C026`/POSNMX `$C027`) then `$E0EA` (read according to
POSNMX/VSALO1 `$C04E`/DESALO `$C052`) is real but requires handling the overlay
switch yourself — prefer `$0467` unless you need fine control.

#### Switching Sedoric RAM overlay ↔ BASIC ROM (validated)

From an ML program launched by `LOAD`, the **BASIC ROM** is mapped at
`$C000-$FFFF`: even the system variables (`BUFNOM $C029`, `DESALO $C052`,
`POSNMX $C027`…) and the routines (`$DB2D`, `$E0EA`) are **in the overlay** → you
must switch in the overlay **before** any `$C000+` access.

The simplest way: **`JSR $0477`** (Sedoric stub in low RAM, always mapped) —
a ROM↔overlay *toggle* that **preserves** drive/side/IRQ via the `$04FB` shadow:
`PHP:PHA:SEI : LDA $04FB : EOR #$02 : STA $04FB : STA $0314 : PLA:PLP:RTS`.
Call it once to enter the overlay, once to come back.

`$0314` values (decoding in `src/io/microdisc.c`): **bit 1 (ROMDIS, `$02`)** = 0 →
RAM overlay at `$C000-$DFFF`, = 1 → BASIC ROM; **bit 7 (EPROM, `$80`)** = 1 →
RAM overlay at `$E000-$FFFF`. Since `$0314` is write-only, do not write a
raw constant (drive/side bits must be preserved): go through the `$04FB` shadow
or `$0477`. **Validated in situ**: `$0477` + `$DB2D` + `$E0EA` loads the file
(`$6000..` exact).

Address reminder (manual): BUFNOM `$C028` drive + `$C029` name(9) + `$C032`
ext(3); XDEFLO `$DFE6`; search `$DB2D`; read `$E0EA`; TXTPTR
`$00E9/$00EA`; TIB (keyboard buffer) `$0035`-`$0084`.

### Reading a file sector by sector at an offset (OSGBPB / random access) — validated

To read `N` bytes at an offset **without loading the whole file** (OSGBPB
equivalent). Overlay ON (`$0477`) is mandatory — descriptor, routines and variables
are in the overlay.

1. `BUFNOM` filled in + `JSR $DB2D` (SEARCH) → `X = POSNMX`, catalogue sector in BUF3.
2. `LDA $C30C,X : LDY $C30D,X : JSR $DA5D` → **descriptor loaded into BUF1 (`$C100`)**.
3. BUF1 layout: `$C100/01` link to next descriptor (`00 00`=end) · `$C102`=`FF` ·
   `$C103` type · `$C104-05` load · `$C106-07` end · `$C108-09` exec · `$C10A-0B`
   sector count · **(track,sector) map from `$C10C`**: data sector *k* =
   `[$C10C+2k]` (track, b7=side B) / `[$C10C+2k+1]` (sector). Beyond 122
   sectors, follow the `$C100/01` link to the next descriptor.
4. `sect_index = off/256`, `byte_in_sect = off & 255`. Set **DRIVE `$C000`**,
   **PISTE (track) `$C001`** = `[$C10C+2*sect_index]`, **SECTEUR (sector) `$C002`** = `[+1]`,
   **RWBUF `$C003/$C004`** = destination buffer.
5. **`JSR $DA73`** (XPRSEC: reads one sector according to DRIVE/PISTE/SECTEUR/RWBUF).
   Copy `N` bytes from `buffer + byte_in_sect`; crossing 256 →
   next sector. (Low-level alternative: `XRWTS $CFCD` with the FDC command
   code in X, e.g. `#$88` read — but XPRSEC is simpler.)

**Validated in situ**: reading data sector #1 (offset 256) of a
512-byte file → exact bytes of the 2nd sector, without loading the file.

> XRWTS/XPRSEC interface (manual, byte-exact): DRIVE `$C000`, PISTE `$C001`
> (b7=side B), SECTEUR `$C002`, RWBUF `$C003/$C004`. `$C009` = DRVDEF (default
> drive), `$C00A` = DRVSYS. (⚠ do not confuse with `$C006-$C00A` = XRWTS retry
> counters.)
