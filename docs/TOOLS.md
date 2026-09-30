# Phosphoric tools

Command-line utilities shipped with the emulator (`tools/` directory).
They cover converting ORIC programs (BASIC / binary → tape),
building and inspecting Sedoric floppies, and exporting to
"physical" formats (cassette audio, floppy magnetic flux).

## Building

```bash
make tools          # builds all the C tools below
```

Each tool can also be built on its own, e.g. `make tap2wav`. The Python
and shell scripts need no compilation.

| Tool             | Input → output             | Role                                             |
|------------------|----------------------------|--------------------------------------------------|
| `bas2tap`        | `.bas` → `.tap`            | BASIC text program → tape                        |
| `bin2tap`        | `.bin` → `.tap`            | Machine-code binary → tape (load/exec)           |
| `tap2sedoric`    | `.tap` (+ base `.dsk`) → `.dsk` | Injects a `.tap` into a Sedoric floppy      |
| `sedoric-info`   | `.dsk` → text              | Inspects the VTOC / catalogue of a Sedoric floppy |
| `tap2wav`        | `.tap` → `.wav`           | **Cassette audio** (playable on a real machine)  |
| `dsk2hfe`        | `.dsk` (MFM_DISK) → `.hfe` | **HFE magnetic image** (HxC / Gotek / Greaseweazle) |

Helper scripts: `dsk_raw2mfm.py`, `sedoric_inject.py`, `sedoric_mkbare.py`,
`make_bootable_sedoric.sh` (see below).

---

## bas2tap — BASIC text → tape

Converts an ORIC BASIC listing (text) into a `.tap` file (tokenised, with a
ROM-compatible 9-byte tape header).

```
bas2tap <input.bas> -o <output.tap> [--auto-run]
```

- `--auto-run`: sets the auto-execution flag (automatic RUN after
  `CLOAD`).

```bash
bas2tap jeu.bas -o jeu.tap --auto-run
```

## bin2tap — binary → tape

Packs a machine-code binary into a `.tap` with load and
execution addresses.

```
bin2tap <input.bin> --start <addr> --exec <addr> -o <output.tap> [--name <name>] [--no-autorun]
```

- `--start`: load address (hex, e.g. `0x500`).
- `--exec`: execution address (auto-started after loading).
- `--name`: tape file name (default `PROGRAM`).
- `--no-autorun`: forces the auto-run flag to `$00`.

```bash
bin2tap demo.bin --start 0x9800 --exec 0x9800 -o demo.tap --name DEMO
```

## tap2sedoric — injecting a .tap into a Sedoric floppy

Copies a base `.dsk` image (**MFM_DISK** format) and injects the contents
of a `.tap` into it as a Sedoric file. See `docs/SEDORIC.md` for the format of
the descriptors.

```
tap2sedoric <input.tap> -o <output.dsk> -b <base.dsk> [-n NAME.EXT] [-a] [-e EXEC_HEX] [-i "INIST"]
```

- `-b`: base floppy (must be an MFM_DISK container).
- `-n`: name of the file on the floppy (e.g. `JEU.COM`).
- `-a`: marks the file as AUTO (auto-started).
- `-e`: execution address (hex) for a `.COM`.
- `-i`: INIST string (customises the boot).

```bash
tap2sedoric jeu.tap -o jeu.dsk -b master.dsk -n JEU.COM -a
```

## sedoric-info — inspecting a Sedoric floppy

Displays the VTOC counters and the catalogue, and can serve as a
non-regression safeguard.

```
sedoric-info <disk.dsk> [--check FREE:FILES]
```

- `--check FREE:FILES`: exits with an error if the number of free sectors or of
  files differs from the expected values (useful in CI).

```bash
sedoric-info jeu.dsk
sedoric-info jeu.dsk --check 640:3
```

---

## tap2wav — cassette audio (`.tap` → `.wav`)

Produces the **real ORIC cassette signal** as a `.wav`, playable into the tape input
of a real machine (via a jack/DIN cable) or archivable as audio.

```
tap2wav IN.tap OUT.wav [--rate HZ] [--leader N] [--amp N] [--lead-silence MS] [--tail-silence MS]
```

| Option           | Default | Effect                                             |
|------------------|---------|----------------------------------------------------|
| `--rate`         | 44100   | WAV sampling rate (Hz)                             |
| `--leader`       | 512     | Number of `0x16` sync frames at the start          |
| `--amp`          | 16000   | Peak amplitude (int16)                             |
| `--lead-silence` | 200     | Silence before the signal (ms)                     |
| `--tail-silence` | 500     | Silence after the signal (ms)                      |

**Encoding** (taken identically from the emulator's signal generator,
`src/io/cassette.c`, modelled on the ROM CSAVE routine `$E619`): 14-bit frame,
LSB first — `start(0) · 8 data · parité impaire · 4 stop(1)` (start, 8 data bits,
odd parity, 4 stop bits). Each bit is
a LOW half-pulse of 208 cycles followed by a HIGH half-pulse of 208 cycles (bit `1`)
or 416 cycles (bit `0`), at 1 MHz. The ROM decoder separates `1`/`0` at a threshold of
~512 cycles.

```bash
make tap2wav
./tap2wav jeu.tap jeu.wav
# then: play jeu.wav into the ORIC's cassette input and type CLOAD"" on the machine
```

## dsk2hfe — floppy magnetic image (`.dsk` → `.hfe`)

Converts an ORIC floppy in **MFM_DISK** format into an **HFE v1** image
(`HXCPICFE`), the magnetic-flux format read/written by an **HxC Floppy Emulator**,
a **Gotek** running FlashFloppy, or a **Greaseweazle** — the disk equivalent of
`tap2wav` for writing real floppies.

```
dsk2hfe IN.dsk OUT.hfe [--bitrate KBPS] [--rpm RPM]
```

| Option      | Default | Effect                         |
|-------------|---------|--------------------------------|
| `--bitrate` | 250     | Cell rate / 2 (kbit/s)         |
| `--rpm`     | 300     | Rotation speed (RPM)           |

**The input must be an MFM_DISK container** (the Oricutron/Phosphoric one:
`MFM_DISK` header, raw 6400-byte tracks). For a flat/raw image,
convert it first with `dsk_raw2mfm.py`. The tool MFM-encodes each track into a
cell stream (16 cells/byte, clock/data pairs), with the standard sync marks
A1=`0x4489` and C2=`0x5224` (missing clock) in front of the address
marks; output complies with the HxC spec (512-byte header, track table, sides
interleaved in 256-byte blocks, bits **LSB first**).

```bash
make dsk2hfe
./dsk2hfe jeu.dsk jeu.hfe
# copy jeu.hfe onto the SD card of the HxC/Gotek, or write it with Greaseweazle
```

---

## Helper scripts

| Script                     | Role                                                         |
|----------------------------|-------------------------------------------------------------|
| `dsk_raw2mfm.py`           | **Raw** disk image (concatenated sectors, e.g. `--disk-create`) → **MFM_DISK** container |
| `sedoric_inject.py`        | Injects a binary as a SEDORIC file into an image             |
| `sedoric_mkbare.py`        | Builds a "bare" Sedoric master (INIST neutralised)           |
| `make_bootable_sedoric.sh` | Complete chain: hands-free **bootable** Sedoric floppy       |

See `docs/SEDORIC.md` for the Sedoric format and loading recipes.

---

## Typical conversion chains

**BASIC program → bootable floppy**
```bash
bas2tap jeu.bas -o jeu.tap --auto-run
tap2sedoric jeu.tap -o jeu.dsk -b master.dsk -n JEU -a
```

**To real hardware**
```bash
tap2wav jeu.tap jeu.wav       # load through the cassette input
dsk2hfe jeu.dsk jeu.hfe       # write a real floppy (HxC/Gotek/Greaseweazle)
```

**Raw image → MFM → HFE**
```bash
python3 tools/dsk_raw2mfm.py brut.dsk mfm.dsk
dsk2hfe mfm.dsk sortie.hfe
```
