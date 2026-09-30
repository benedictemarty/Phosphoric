# Phosphoric User Guide

**Version 1.110.0-alpha** | ORIC-1 / Atmos emulator

---

## Table of contents

1. [Installation](#installation)
2. [Quick start](#quick-start)
3. [Loading programs](#loading-programs)
4. [Keyboard](#keyboard)
5. [Joystick](#joystick)
6. [Video and display](#video-and-display)
7. [Audio](#audio)
8. [Printer and plotter](#printer-and-plotter)
9. [Save states](#save-states)
10. [Interactive debugger](#interactive-debugger)
11. [CPU trace and profiler](#cpu-trace-and-profiler)
12. [Advanced debugging (GDB, IPC, HTTP API)](#advanced-debugging-gdb-ipc-http-api)
13. [ROM analysis](#rom-analysis)
14. [Serial and modems](#serial-and-modems)
15. [LOCI](#loci)
16. [Video recording and replay](#video-recording-and-replay)
17. [ULA-NG (video extensions)](#ula-ng-video-extensions)
18. [Chromecast](#chromecast)
19. [Headless mode and automation](#headless-mode-and-automation)
20. [WebAssembly (browser)](#webassembly-browser)
21. [Conversion tools](#conversion-tools)
22. [Complete CLI reference](#complete-cli-reference)
23. [Troubleshooting](#troubleshooting)

---

## Installation

### Dependencies

```bash
# Debian / Ubuntu
sudo apt-get install build-essential libsdl2-dev

# Fedora
sudo dnf install gcc SDL2-devel

# Arch Linux
sudo pacman -S base-devel sdl2

# Optional: Chromecast support
sudo apt-get install libssl-dev
```

### Building

```bash
make SDL2=1                    # Standard build with SDL2
make                           # Headless build (without SDL2)
make DEBUG=1 SDL2=1            # Debug build (-g -O0)
make SDL2=1 CAST=1             # With Chromecast support
make tools                     # Conversion tools
sudo make install              # Install into /usr/local
```

### Verification

```bash
make tests                     # 908 tests (100% must pass)
```

---

## Quick start

Phosphoric needs an ORIC ROM file to start. The ROMs are not distributed with the emulator for copyright reasons.

```bash
# ORIC-1 with BASIC 1.0
./oric1-emu -r roms/basic10.rom

# ORIC Atmos with BASIC 1.1 (auto-detected)
./oric1-emu -r roms/basic11b.rom

# Force the model
./oric1-emu -r roms/basic10.rom --model oric1
./oric1-emu -r roms/basic11b.rom --model atmos
```

### Supported ROMs

| ROM | Size | Model | Detection |
|-----|--------|--------|-----------|
| basic10.rom | 16384 bytes | ORIC-1 (BASIC 1.0) | JMP $EA59 |
| basic11b.rom | 16384 bytes | Atmos (BASIC 1.1) | JMP $ECCC |
| microdis.rom | variable | Microdisc (overlay) | N/A |

---

## Loading programs

### Tapes (.TAP)

**Interactive loading (CLOAD):**
```bash
./oric1-emu -r basic10.rom -t jeu.tap
```
At the BASIC prompt, type `CLOAD""` then Enter. The program loads from the virtual tape.

**Fast loading (direct injection):**
```bash
./oric1-emu -r basic10.rom -t jeu.tap -f
```
The program is injected directly into memory via the ROM patch, with no delay.
Multi-block programs (e.g. TYRANN.TAP) are supported: the first block is injected,
the following blocks are loaded by CLOAD via the ROM patches. Stale BASIC next-line
pointers are automatically fixed up after each load.

**Signal-level loading (`--tape-signal`):**
```bash
./oric1-emu -r basic10.rom -t jeu.tap --tape-signal
```
Generates the real tape waveform on the VIA CB1 input, read by the ROM's
**real** CLOAD routine — as on a real machine or under
Euphoric. Use it for **non-standard / protected tape loaders**
(turbo-loaders, deprotection routines, home-made multi-block sequencing) that
the ROM patches cannot reproduce — for example *Soccer Manager*
(KnightSoft, `EOR #$55` decryption), which fails in patch mode just as under
Oricutron. At the prompt, `CLOAD""` is typed automatically. Incompatible with `-f`.

> Loading happens at **real tape speed** (~5 min for
> 45 KB in windowed mode): that is the price of fidelity. For standard games,
> `-f` remains much faster.

**Tape saving (CSAVE):**
When a BASIC program runs `CSAVE"name"`, the data is captured into a file
`name.tap` in the current directory. If the name is empty (`CSAVE""`), the file will be
`csave_output.tap`.

### Disks (.DSK)

Booting from disk requires the BASIC ROM and the Microdisc ROM:

```bash
# A single drive (A:)
./oric1-emu -r basic10.rom --disk-rom microdis.rom -d SEDORIC.DSK

# Several drives (A: B: C: D:)
./oric1-emu -r basic10.rom --disk-rom microdis.rom \
  -d systeme.dsk --disk1 donnees.dsk --disk2 jeux.dsk --disk3 outils.dsk
```

### Host file system

Share a directory between the PC and the emulator:
```bash
./oric1-emu -r basic10.rom --hostfs /chemin/vers/dossier
```

---

## Keyboard

### Keyboard layout

By default, the emulator uses a QWERTY layout. To switch to AZERTY:

```bash
./oric1-emu -r basic10.rom --keyboard azerty
```

In AZERTY mode, the emulator uses SDL2 text events, so typing works naturally whatever the physical keyboard layout.

### ORIC special keys

| PC key | ORIC key |
|-----------|-------------|
| Escape | ESC |
| Backspace | DEL |
| Left Ctrl | CTRL |
| Left/Right Shift | SHIFT |
| Return/Enter | RETURN |

### Emulator function keys

| Key | Function |
|--------|----------|
| F1 | I/O peripherals menu (floppies, tape, snapshots, printer, joystick, keyboard; see the README) |
| F2 | Quicksave |
| F3 | Change the display scale (x1 -> x2 -> x3 -> x4) |
| F4 | Quickload |
| F5 | Warm reset |
| F7 | Memory dump (64 KB of RAM into a timestamped .bin file) |
| F9 | Enter the debugger |
| F10 | Quit |
| F11 | Full screen |
| F12 | Screenshot |

---

## Joystick

Phosphoric emulates the IJK joystick interface, the most common adapter for the ORIC. The joystick is read via the PSG's Port A (active low).

### Keyboard mode

```bash
./oric1-emu -r basic10.rom -j keys
```

| Key | Direction |
|--------|-----------|
| Up/down/left/right arrows | Directions |
| Right Ctrl | Fire 1 |
| Right Alt | Fire 2 |

### SDL2 gamepad mode

```bash
./oric1-emu -r basic10.rom -j gamepad
```

Uses the first SDL2 gamepad detected. Buttons A, B and X map to fire. The D-pad and the left analogue stick control the directions. Hot-plugging is supported.

---

## Video and display

### Video modes

The ORIC has two display modes:
- **Text mode**: 40 columns x 28 lines, 8 colours (ink/paper attributes)
- **HIRES mode**: 240 x 200 pixels, 6 colours with serial attributes

### Display scale

```bash
./oric1-emu -r basic10.rom --scale 2
```

| Scale | Window resolution |
|---------|--------------------|
| x1 | 240 x 224 |
| x2 | 480 x 448 |
| x3 (default) | 720 x 672 |
| x4 | 960 x 896 |

Rendering uses nearest-neighbour scaling (pixel-perfect, no blur). Press **F3** to change the scale in real time. **F11** toggles full screen.

### Screenshots

```bash
# Capture on exit
./oric1-emu -r basic10.rom --screenshot sortie.bmp

# Capture after N cycles
./oric1-emu -r basic10.rom --screenshot-at 1000000:sortie.ppm

# Periodic frame dump
./oric1-emu -r basic10.rom --frame-dump /tmp/frames --frame-dump-interval 50
```

Supported formats: PPM (binary P6) and BMP (uncompressed 24-bit).

---

## Audio

The ORIC uses an AY-3-8910 PSG (General Instrument):
- 3 independent tone channels (12-bit period)
- 1 noise generator (5-bit period, 17-bit LFSR)
- 16 envelope shapes (attack, decay, hold, alternate)
- Mixer control (tone/noise enable per channel)

Audio output goes through SDL2 at 44100 Hz stereo. The PSG runs at 1 MHz, like the original hardware.

### BASIC commands

```basic
REM Play a sound on channel A
SOUND 1,100,15

REM Play with an envelope
PLAY 0,0,0,0

REM Simple music
MUSIC 1,4,1,15 : MUSIC 2,4,5,15 : PLAY 1,0,1,0
```

---

## Printer and plotter

### Text printer (Centronics)

Captures LPRINT and LLIST output into a text file:

```bash
./oric1-emu -r basic10.rom -p sortie.txt
```

```basic
REM In BASIC:
LPRINT "Bonjour le monde"
LLIST
```

### MCP-40 plotter

Emulates the MCP-40 4-colour plotter (Sharp CE-150 / CGP-115):

```bash
./oric1-emu -r basic10.rom -p traceur.bmp --printer-type mcp40
```

The plotter uses a 480x400-pixel framebuffer and exports to BMP on exit.

**Plotter commands** (sent via LPRINT):

| Command | Description |
|----------|-------------|
| H | Home (return to the origin) |
| D x,y | Draw (draw a line to x,y) |
| M x,y | Move (move without drawing) |
| J n | Color (change pen: 0=black, 1=blue, 2=green, 3=red) |
| P text | Print (write text at the current position) |
| I | Init (reset the plotter) |
| L n | LineType (line style: 0=solid, 1-4=dotted) |
| Q n | CharSize (character size) |

```basic
REM Example: draw a red square
LPRINT "J3"          : REM Red pen
LPRINT "M0,0"        : REM Go to the origin
LPRINT "D100,0"      : REM Draw to the right
LPRINT "D100,100"    : REM Draw upwards
LPRINT "D0,100"      : REM Draw to the left
LPRINT "D0,0"        : REM Close the square
```

---

## Save states

### Quick save and restore

- **F2**: quicksave (`oric1_quicksave.ost`)
- **F4**: quickload (`oric1_quicksave.ost`)

### From the command line

```bash
# Save on exit
./oric1-emu -r basic10.rom --save-state partie.ost

# Load at startup
./oric1-emu -r basic10.rom --load-state partie.ost
```

### .ost format

The binary `.ost` format (Oric Save sTate) contains:
- Header: magic "OST1", version, size, CRC32
- 10 sections: CPU, MEM (64 KB), VIA, PSG, VID, KBD, FDC, MDC, TAP, META
- Typical size: ~65 KB
- The framebuffer is regenerated automatically on load

---

## Interactive debugger

### Starting

```bash
# Enter the debugger at launch
./oric1-emu -r basic10.rom --debug

# Set an initial breakpoint
./oric1-emu -r basic10.rom --break ED8A
```

During emulation, press **F9** to enter the debugger.

### Commands

| Command | Alias | Description |
|----------|-------|-------------|
| `s` | `step` | Execute one instruction |
| `n` | `next` | Execute up to the next PC (steps over JSRs) |
| `c` | `continue` | Resume emulation |
| `r` | `regs` | Show the CPU registers |
| `d [addr] [n]` | | Disassemble n instructions at addr |
| `m addr [n]` | | Dump n bytes of memory at addr |
| `b addr` | | Add a breakpoint (max 16) |
| `bd n` | | Delete breakpoint #n |
| `w addr` | | Add a memory watchpoint (max 8) |
| `wd n` | | Delete watchpoint #n |
| `via` | | Show the VIA 6522 registers |
| `psg` | | Show the AY-3-8910 PSG registers |
| `stack` | | Show the stack contents |
| `set reg val` | | Modify a register (a, x, y, sp, pc, p) |
| `q` | `quit` | Quit the emulator |
| `h` | `help` | Show help |

### Examples

```
dbg> b C000          # Breakpoint at $C000
dbg> c               # Continue to the breakpoint
dbg> r               # Show the registers
dbg> d C000 10       # Disassemble 10 instructions at $C000
dbg> m 0400 64       # Dump 64 bytes at $0400
dbg> w 0300          # Watchpoint on the VIA (port A)
dbg> set a 42        # A = $42
dbg> s               # Step
```

---

## CPU trace and profiler

### CPU trace

Records every executed instruction with its disassembly and the register state:

```bash
./oric1-emu -r basic10.rom --trace trace.log

# Limit to N instructions
./oric1-emu -r basic10.rom --trace trace.log --trace-max 10000
```

**Output format** (one line per instruction):
```
CCCCCCCC  AAAA  XX XX XX  MNEMONIC OPERAND       A=XX X=XX Y=XX SP=XX P=XX
00000000  F42D  4C 59 EA  JMP $EA59              A=00 X=00 Y=00 SP=FD P=24
00000003  EA59  A2 FF     LDX #$FF               A=00 X=00 Y=00 SP=FD P=24
```

### CPU profiler

Generates a performance report on exit:

```bash
./oric1-emu -r basic10.rom --profile profil.txt --cycles 1000000
```

The report contains:
- Total instructions and cycles, average cycles/instruction
- Top 20 most executed addresses (with % of the total)
- Top 20 addresses by cycle consumption
- Opcode frequency histogram

---

## Advanced debugging (GDB, IPC, HTTP API)

Beyond the interactive debugger (F9 / `--debug`), Phosphoric exposes several
external control and inspection channels.

### GDB remote (`--gdb`)

A **GDB Remote Serial Protocol** server: attach `gdb`, `lldb` or an IDE
(VS Code, CLion) to the emulated 6502 to set breakpoints, single-step and
read/write registers and memory.

```bash
./oric1-emu -r basic11b.rom --gdb=1234      # waits for `target remote :1234`
```

Details, session examples and register mapping: [docs/gdb_remote.md](../gdb_remote.md).

### IPC control (`--control`)

A line-based text protocol over stdin/stdout (logs on stderr) to drive
the emulator from an IDE or a script: keyboard injection, memory reads,
save/load state, media hot-swap, events. It is the foundation of OricForge.

```bash
./oric1-emu -r basic11b.rom -n --control
```

Complete message grammar: [docs/control_protocol.md](../control_protocol.md).

### HTTP/REST API (`--http-api`, build `HTTPAPI=1`)

The same dispatch as `--control`, exposed over HTTP/JSON: keyboard control, state,
tape/disk files in a sandbox.

```bash
make HTTPAPI=1
./oric1-emu -r basic11b.rom -n --http-api=8888 --http-api-root ./sandbox
```

`--http-api-bind` (default 127.0.0.1), `--http-api-root` (sandbox `/tape`,`/disk`).
Reference: [docs/http-api.md](../http-api.md).

### TUI debugger and symbols

- `--tui` — full-screen ncurses debugger (build `TUI=1`).
- `--symbols FILE` — loads a symbol table (`.sym`/`.lab`/`.sym65`) to
  annotate traces and disassembly.

---

## ROM analysis

Analyse a ROM to extract structural information:

```bash
# Print to stdout
./oric1-emu -r basic10.rom --rom-info

# Write to a file
./oric1-emu -r basic10.rom --rom-info rapport.txt
```

The report contains:
- **Hardware vectors**: RESET, NMI, IRQ (addresses from the vector table)
- **Subroutine map**: all JSR/JMP targets with their reference counts
- **ASCII strings**: text detected in the ROM (minimum 4 characters)
- **Usage statistics**: code bytes vs data vs padding ($00/$FF)

---

## Serial and modems

Phosphoric emulates a **6551 ACIA** (base `$031C` by default, `--acia-addr`)
and several historical serial/MIDI cards. The backend is chosen with
`--serial TYPE`:

| Backend | Description |
|---|---|
| `loopback` | Local loop (TX -> RX), for testing a protocol |
| `tcp:H:P` | TCP socket (BBS server, remote terminal) |
| `pty` | Host pseudo-terminal (`/dev/pts/N`) |
| `modem:H:P` | Hayes modem (AT commands) to TCP |
| `com:B,D,P,S,DEV` | Real host serial port (baud,bits,parity,stop,device) |
| `file:IN[:OUT]` | Replays/captures raw bytes in files |
| `picowifi[:SSID[:PASS]]` | PicoWiFiModemUSB WiFi modem (LOCI ACIA $0380) |

Related options: `--serial-v23` (1200/75, Minitel/Prestel), `--serial-baud`
(realistic external clock timing), `--serial-buffer N` (anti-overrun RX FIFO),
`--serial-irq-on-rdrf` (WDC 65C51 mode), `--serial-tcp-backpressure`
(bounded TCP back-pressure), `--serial-trace FILE` (timestamped TX/RX trace).

```bash
# BBS over TCP, real-time pacing (essential for network timing)
./oric1-emu -r basic11b.rom --serial tcp:bbs.example.org:6502 --realtime

# LOCI WiFi modem
./oric1-emu -r basic11b.rom --loci --serial picowifi:MyWiFi:password
```

### Dedicated cards

- **Digitelec DTL 2000** — `--dtl2000 TRANSPORT`: faithful V23 card (PIA 6821 +
  ACIA 6850) at `$03F8` (`--dtl2000-addr`). See
  [docs/digitelec-dtl2000/](../digitelec-dtl2000/README.md).
- **Mageco MIDI** — `--mageco TRANSPORT`: MIDI interface (ACIA 6850) at `$03FE`,
  31250 baud (`--mageco-addr`); transports `midi:` (ALSA, build `MIDI=1`),
  `smf:song.mid`, `file::out.mid`, etc.
- **ORICON** — `--oricon TRANSPORT`: MIDI variant (MC6850 at `$031C-$031F`,
  LOCI compatible).

Serial guide from the ORIC program side: [docs/orictel-serial-guide.md](../orictel-serial-guide.md),
Hayes modem: [docs/orictel-modem-hayes.md](../orictel-modem-hayes.md).

---

## LOCI

**LOCI** (Lovely Oric Computer Interface, sodiumlb 2024) is an RP2040
cartridge plugged into the Oric's bus: mass storage (USB / SD / internal
flash), USB HID keyboard-mouse-gamepads and a WiFi modem. Phosphoric emulates its
MIA (memory interface) at `$03A0-$03BF` with `--loci`.

| Option | Role |
|---|---|
| `--loci` | Enables the LOCI MIA at `$03A0-$03BF` |
| `--loci-flash DIR` | Sandbox root for LOCI file operations |
| `--loci-sdimg PATH` | Raw FAT16/32 SD image (read-only) |
| `--loci-usb DIR\|none` | Attaches DIR as a USB stick (repeatable, 4 max) |
| `--loci-web URL` | Drive A served by a web server + native Sedoric autoboot |
| `--loci-web-base URL` | "W: Web disks" pseudo-device in the LOCI menu |
| `--loci-mia-window LO-HI` | Models the reliable MIA `tior` range (0-31) |
| `--loci-irq-latency US` | I2C transport cost of LOCI IRQs |

The **Action button** (F8) triggers a session snapshot and then the LOCI menu
(short press) or the ROM diagnostics (long press). It boots a complete Sedoric V4
master via the LOCI firmware.

If F8 no longer reaches Phosphoric (on many laptops F8 is also
a multimedia key — volume — that the desktop captures before the application as soon
as the Fn lock toggles), **Ctrl+Alt+M** = short press (menu) and
**Ctrl+Alt+D** = long press (diagnostic ROM) do the same thing and are
never passed on to the Oric.

```bash
./oric1-emu -r basic11b.rom --loci --loci-flash ./loci_files
```

Complete documentation (menu, firmware ABI, timings, USB sticks):
[docs/loci.md](../loci.md).

---

## Video recording and replay

- **AVI video** — `--video FILE` records a Motion-JPEG AVI (`--video-fps`,
  default 50; `--video-quality` 1..100, default 85). The PSG sound is muxed (headless
  as well as GUI). `--export-border` includes the overscan border.
- **WAV audio** — `--audio-wav FILE` captures the PSG as 16-bit stereo
  44.1 kHz WAV (headless mode).
- **Frame dump** — `--frame-dump DIR` + `--frame-dump-interval N`.
- **Deterministic record / replay** — `--record FILE` records the keyboard
  input of a session, `--replay FILE` replays it identically ("TAS" style:
  only the keyboard matrix is non-deterministic, and it is captured per frame).

```bash
# Record a demo then replay it as video
./oric1-emu -r basic11b.rom --record demo.phm
./oric1-emu -r basic11b.rom --replay demo.phm --video demo.avi
```

Details: [docs/movie_replay.md](../movie_replay.md).

---

## ULA-NG (video extensions)

**ULA-NG** is a "next-generation" ULA: video extensions (indirect palette,
raster IRQ, fine scroll, hardware sprites, chunky 4bpp, 80-column text)
enabled by an unlocking mechanism — **indistinguishable from a standard HCS 10017 ULA
as long as it stays locked**. Registers at `$0340-$035F`.

`--ula-ng-poke SEQ` programs these registers at startup (`SEQ` = hex
`AAA=VV` pairs separated by commas):

```bash
./oric1-emu -r basic11b.rom --ula-ng-poke 340=4E,340=47,341=01,348=07,349=00,34A=F0
```

Specification and guide: [docs/ula-ng/](../ula-ng/README-ULA-NG.md).

---

## Chromecast

### MJPEG server

Streams the emulator screen as MJPEG:

```bash
./oric1-emu -r basic10.rom --cast-server
# Server started on http://localhost:8080/stream

./oric1-emu -r basic10.rom --cast-server=9090
# Custom port
```

Endpoints:
- `/stream`: MJPEG video stream (720x672, 3x upscale)
- `/audio`: WAV audio stream (real-time PSG)

### Native Chromecast casting (CASTV2)

```bash
# Discover Chromecast devices on the network
./oric1-emu -r basic10.rom --cast-discover

# Cast to a Chromecast
./oric1-emu -r basic10.rom --cast-server --cast-to
./oric1-emu -r basic10.rom --cast-server --cast-to="Salon"
```

The native CASTV2 protocol includes: TLS, protobuf, PING/PONG heartbeat, DashCast launch.

---

## Headless mode and automation

### Headless mode

Run without a display (for CI, tests, scripting):

```bash
./oric1-emu -r basic10.rom --headless --cycles 1000000
```

### Automatic keyboard input

Simulate keystrokes after a delay in cycles:

```bash
# Type CLOAD"" + Enter after 3M cycles
./oric1-emu -r basic10.rom -t jeu.tap --headless \
  --type-keys '3000000:CLOAD""\n'

# Special sequences:
#   \n  = RETURN key
#   \pN = pause of N seconds (1-9)
```

### Verbose output

```bash
./oric1-emu -r basic10.rom -v    # DEBUG logs
```

---

## WebAssembly (browser)

Phosphoric compiles to **WebAssembly** via Emscripten: the complete emulator runs
in a browser tab, rendered on a `<canvas>`, with audio via Web Audio and keyboard via the DOM.

```bash
make wasm     # requires the Emscripten emsdk (see docs/wasm.md)
```

The page (`web/shell.html` + `web/shell.js`) offers a JOric-style icon rail
(ROM selector, drag-and-drop of `.tap`/`.dsk`, Reset, full screen, CRT filter,
`.ost` save/restore), TAPE/DISK activity LEDs and a faithful ORIC keyboard
overlay. **Deep links**: `?rom=oric1|atmos` and `?media=<fichier>.tap|.dsk`
start directly on a program (a `.dsk` automatically enables the
Microdisc). Output is **byte-identical** to the native build.

Deployment guide (including the required CSP): [docs/wasm.md](../wasm.md).

---

## Conversion tools

### bas2tap — BASIC to tape

Convert a BASIC text file into .TAP format:

```bash
./bas2tap programme.bas -o programme.tap
```

The BASIC file must contain numbered lines:
```basic
10 PRINT "BONJOUR LE MONDE"
20 GOTO 10
```

### bin2tap — Binary to tape

Convert a machine-code binary into a .TAP with a load/execution address:

```bash
./bin2tap programme.bin --start 0x0400 --exec 0x0400 -o programme.tap
```

### tap2sedoric — Tape to Sedoric disk

Inject a .TAP file into a copy of a Sedoric disk (MFM_DISK): it
shows up in the catalogue (`DIR`) and can be loaded/executed.

```bash
# base.dsk = an existing Sedoric MFM disk (e.g. disks/SEDO40u.DSK)
./tap2sedoric programme.tap -o disque.dsk -b base.dsk -n PROG.COM

# auto-executing machine-code file (AUTO): loads AND runs via LOAD"PROG"
./tap2sedoric programme.tap -o disque.dsk -b base.dsk -n PROG.COM -a -e 0x5000

# set a boot autoexec (INIST) that launches the file at startup
./tap2sedoric programme.tap -o disque.dsk -b base.dsk -n PROG.COM -a -i 'LOAD"PROG"'
```

Options: `-n NOM.EXT` (Sedoric name), `-a` (AUTO), `-e EXEC_HEX` (execution
address), `-i "INIST"` (boot autoexec). The catalogue is extended
automatically (chaining) beyond ~15 files. Format and recipe detailed
in [`docs/SEDORIC.md`](../SEDORIC.md).

### sedoric-info — Inspecting a Sedoric disk

Shows the VTOC (free sectors / number of files), the disk name,
the INIST, the catalogue and the decoded descriptors (type / load / end / exec):

```bash
./sedoric-info disque.dsk
./sedoric-info disque.dsk --check 1445:95   # regression guard on the VTOC
```

### RAW chain and "bare" master (Python scripts)

An alternative working in RAW (direct offsets), followed by conversion to MFM:

```bash
# inject a binary (exec given => AUTO file) then convert RAW -> MFM
python3 tools/sedoric_inject.py base.raw prog.bin 0x5000 PROG.COM out.raw 42 17 "" 0x5000
python3 tools/dsk_raw2mfm.py out.raw out.dsk sidemajor 2 42 17

# "bare" Sedoric master: neutralises the INIST -> boots straight to the Ready prompt
python3 tools/sedoric_mkbare.py disks/SEDO40u.DSK bare.dsk
# ... or replaces the INIST to autorun a program at boot
python3 tools/sedoric_mkbare.py disks/SEDO40u.DSK auto.dsk 'LOAD"PROG"'
```

### Running a machine-code program under Sedoric

Once the disk is mounted and Sedoric has started (`Ready`), an **AUTO `.COM`**
file is launched with Sedoric's **`LOAD`** command:

```
LOAD"PROG"
```

Beware: the command really is `LOAD` (not `LOADM`, which is the ROM's tape
command and returns `?TYPE MISMATCH ERROR`). Typing the bare name only launches
a BASIC program (`?SYNTAX ERROR` for a binary). The default extension
is `.COM` (a `.BIN` would give `?FILE NOT FOUND ERROR`).

---

## Complete CLI reference

> Exhaustive list aligned with `./oric1-emu --help` (v1.110.0-alpha).
> Some options require a specific build: `--tui` (TUI=1),
> `--http-api` (HTTPAPI=1), `--cast-*` (CAST=1), `midi:` backend (MIDI=1).

```
./oric1-emu [OPTIONS]

ROM, model and host:
  -r, --rom FILE             Load a ROM file (BASIC 1.0/1.1)
  -m, --model MODEL          Model: oric1 or atmos (default: auto-detection)
  -k, --keyboard LAYOUT      Keyboard layout: qwerty (default) or azerty
  -h, --hostfs PATH          Mount a host directory (file sharing)

Tape:
  -t, --tape FILE            Load a .TAP tape file
  -f, --fast-load            Fast loading (memory injection, no CLOAD)
      --tape-signal          Signal-level tape (VIA CB1 waveform, real ROM
                             read) — custom/protected loaders; excludes -f
      --tape-out-capture FILE  Capture the CSAVE waveform (PB7/Timer1) and decode it
                             into a .TAP (path A; disables the CSAVE hooks)

Disk (Microdisc WD1793):
  -d, --disk FILE            Load a .DSK image into drive A
      --disk1 FILE           .DSK image in drive B
      --disk2 FILE           .DSK image in drive C
      --disk3 FILE           .DSK image in drive D
      --disk-rom FILE        Load the Microdisc ROM (microdis.rom)
      --disk-writeback       Write disk changes back to the .dsk files on
                             exit (in place; only drives written to are saved)
      --disk-create FILE     Create a blank Sedoric disk (drive A) -> FILE
      --disk-web URL         Drive A served by a web server (loci-webdisk arch. B),
                             MFM tracks read over HTTP on demand via the Microdisc
      --fdc-timing MODE      WD1793 timing: real (default, 3" mechanics) or fast
      --bad-sector [D:]S:T:N Mark a sector unreadable (RNF): drive D (default A),
                             side S, track T, sector N; repeatable

Save state:
      --save-state FILE      Save the state on exit (.ost)
      --load-state FILE      Load the state at startup

Video and display:
      --scale N              Scale: 1, 2, 3 (default) or 4
      --render-software      Force the software SDL renderer (fixes a black window
                             on some GPUs/drivers)
      --no-border            Disable the overscan border in the window
      --export-border        Include the overscan border in image/AVI exports
      --ula-ng-poke SEQ      Program the ULA-NG registers ($0340-$035F) at boot,
                             SEQ = hex AAA=VV pairs separated by commas

Captures and export:
      --screenshot FILE          Capture on exit (.ppm or .bmp; .png supported)
      --screenshot-at C:FILE     Capture after C cycles (-at family, REPEATABLE)
      --screenshot-when A:V:FILE Capture when RAM[A]==V (A,V hex; exit 2 if never)
      --screenshot-text FILE     Dump the text screen ($BB80, 40x28) as ASCII on exit
      --screenshot-text-at C:FILE   Text dump after C cycles
      --screenshot-text-when A:V:FILE  Text dump when RAM[A]==V (A,V hex)
      --screenshot-ansi FILE     Dump the framebuffer as true-colour ANSI text on exit
      --screenshot-ansi-at C:FILE   ANSI dump after C cycles
      --dump-ram-at C:FILE       Dump 64 KB of RAM when cycle >= C
      --dump-ram-when A:V:FILE   Dump 64 KB when RAM[A]==V (A,V hex; exit 2 if never)
      --frame-dump DIR           Periodic dump of frames into a directory
      --frame-dump-interval N    Dump one frame out of N (default 50)
      --video FILE               Record a Motion-JPEG AVI video
      --video-fps N              Recording frame rate (default 50)
      --video-quality N          JPEG quality 1..100 (default 85)

Audio:
      --audio-wav FILE       Capture the PSG as 16-bit stereo 44.1 kHz WAV (headless)
      --psg-trace FILE       Log AY register writes (0-13) + CPU cycle

Headless and automation:
  -n, --headless             No display (headless mode)
      --realtime             Pace at 50 Hz PAL even in headless (nanosleep);
                             required for network serial timing and deterministic --type-keys
  -c, --cycles NUM           Run N cycles then quit
  -v, --verbose              Verbose logs
      --type-keys C:TEXT     Automatic keyboard input after C cycles (escapes \n \e
                             \b \u \d \l \r \Cx \Fx \Lx \Rx \pN; repeatable)
      --type-keys-when A:V:TEXT  Arm --type-keys when RAM[A]==V (A,V hex)
      --poke-at C:ADDR=VAL       Write RAM[ADDR]=VAL once after C cycles (hex; repeatable)
      --poke-when A:V:ADDR=VAL   Write RAM[ADDR]=VAL once when RAM[A]==V (hex; repeatable)
      --record FILE          Record keyboard input (deterministic replay)
      --replay FILE          Replay an input movie (ignores the live keyboard)
      --bench                Headless throughput bench (`BENCH cycles=... mhz_eq=...`)

Input/output peripherals:
  -j, --joystick MODE        Joystick: keys (arrows) or gamepad (SDL2 gamepad)
  -p, --printer FILE         Capture printer output (LPRINT/LLIST)
      --printer-type TYPE    Type: text (default) or mcp40 (4-colour plotter)

Debugger, trace and profile:
  -D, --debug                Start in the debugger (break at the 1st instruction)
  -b, --breakpoint ADDR      Break when PC reaches ADDR (hex)
      --break ADDR           Initial breakpoint of the interactive debugger (hex)
      --tui                  ncurses TUI debugger (build TUI=1)
      --gdb[=PORT]           Remote GDB stub on TCP PORT (default 1234)
      --control              IPC mode for IDE integration (stdin protocol)
      --symbols FILE         Load a symbol table (.sym/.lab/.sym65)
      --trace FILE           Instruction-by-instruction CPU trace
      --trace-max N          Limit on traced instructions (keeps the FIRST N)
      --trace-ring N         Keep the LAST N instructions (ring, written on exit)
      --trace-irq FILE       Log every IRQ entry + RTI
      --kbd-scan-trace FILE  Log every VIA Port B read (col, reg7, reg14, matrix, PB3)
      --profile FILE         Write a CPU performance profile on exit
      --rom-info [FILE]      Analyse the ROM (vectors, targets, strings)

Serial (ACIA 6551) and modems:
      --serial TYPE          loopback, tcp:H:P, pty, modem:H:P, com:B,D,P,S,DEV,
                             file:IN[:OUT], picowifi[:SSID[:PASS]]
      --serial-v23           V23 mode: 1200/75 baud (Minitel/Prestel/Digitelec)
      --serial-buffer N      RX FIFO of N bytes (anti-overrun; default: off)
      --serial-baud N        External clock baud rate (realistic timing vs instant transfer)
      --serial-irq-on-rdrf   WDC 65C51 IRQ mode (re-triggers while RDRF is set)
      --serial-trace FILE    Serial trace (timestamped TX/RX/signals)
      --serial-tcp-backpressure[=N]  Bounded back-pressure for tcp: (SO_RCVBUF cap)
      --acia-addr ADDR       ACIA base address in hex (default 031C)

Dedicated serial/MIDI cards:
      --dtl2000 TRANSPORT    Digitelec DTL 2000 (PIA 6821 + ACIA 6850) at $03F8
      --dtl2000-addr ADDR    DTL 2000 base address in hex (default 03F8)
      --mageco TRANSPORT     Mageco MIDI interface (ACIA 6850) at $03FE, 31250 baud
      --mageco-addr ADDR     Mageco base address in hex (default 03FE)
      --oricon TRANSPORT     ORICON MIDI variant (MC6850 at $031C-$031F, LOCI compatible)

LOCI (Lovely Oric Computer Interface):
      --loci                 Enable the LOCI MIA at $03A0-$03BF
      --loci-flash DIR       Sandbox root for LOCI file ops (implies --loci)
      --loci-sdimg PATH      Raw FAT16/32 SD image (read-only; excludes --loci-flash)
      --loci-usb DIR|none    Attach DIR as a LOCI USB stick (repeatable, 4 max; 'none' disables)
      --loci-web URL         LOCI drive A served by a web server + native Sedoric autoboot
      --loci-web-base URL    "W: Web disks" pseudo-device in the LOCI menu
      --loci-mia-window LO-HI  Model the reliable MIA tior range (0-31)
      --loci-irq-latency US  I2C transport cost of LOCI IRQs (delays each /IRQ by US µs)

Chromecast:
      --cast-server[=PORT]   MJPEG server (default 8080)
      --cast-to[=DEVICE]     Cast to a Chromecast (native CASTV2)
      --cast-discover        Discover Chromecasts on the network

HTTP API (build HTTPAPI=1):
      --http-api[=PORT]      HTTP/REST control API (default 8888)
      --http-api-bind ADDR   API listen address (default 127.0.0.1)
      --http-api-root DIR    Sandbox root for /tape,/disk file ops (default CWD)

Help:
  -?, --help                 Show help

Function keys (SDL window):
  F1 Peripherals menu  F2 Quicksave  F3 Scale  F4 Quickload  F5 Reset
  F6 Hot tape/disk OSD  F8 LOCI Action button  F9 Debugger
  F10 Quit  F11 Full screen  F12 Screenshot
```

---

## Troubleshooting

**No sound**: Check that the build uses `SDL2=1` and that the system volume is not muted.

**Keyboard dead after a Sedoric boot**: Bug fixed in v1.0.0-beta.8. The VIA's T1 timer is correctly re-asserted after Sedoric initialisation.

**The program does not load with CLOAD**: Check that the .TAP file is valid. Try fast-load mode (`-f`).

**Black screen**: Check that the ROM file is correct (16384 bytes). The emulator needs a valid ORIC ROM to start.

**Slow performance**: The emulator runs at ~90+ MHz equivalent (90x real time). If performance is insufficient, disable tracing (`--trace`) and profiling (`--profile`).

**No Chromecast detected**: Check that the PC and the Chromecast are on the same network. The build must include `CAST=1` and the OpenSSL dependencies.

---

*Phosphoric v1.110.0-alpha — User guide*
*Last updated: 2026-08-30*
