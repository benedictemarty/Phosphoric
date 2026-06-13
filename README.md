# Phosphoric

A cycle-accurate ORIC-1 / Atmos emulator written in C11.

**Version: 1.16.75-alpha** | **499 unit tests + 12 E2E, 100% pass** | **Zero memory leaks**

```
 ____  _                      _                _
|  _ \| |__   ___  ___ _ __ | |__   ___  _ __(_) ___
| |_) | '_ \ / _ \/ __| '_ \| '_ \ / _ \| '__| |/ __|
|  __/| | | | (_) \__ \ |_) | | | | (_) | |  | | (__
|_|   |_| |_|\___/|___/ .__/|_| |_|\___/|_|  |_|\___|
                       |_|
```

## Quick Start

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt-get install build-essential libsdl2-dev

# Build with SDL2
make SDL2=1

# Boot ORIC-1 BASIC
./oric1-emu -r roms/basic10.rom

# Boot ORIC Atmos BASIC (auto-detected)
./oric1-emu -r roms/basic11b.rom

# Load a tape program
./oric1-emu -r roms/basic10.rom -t program.tap -f

# Boot Sedoric from disk
./oric1-emu -r roms/basic10.rom --disk-rom roms/microdis.rom -d SEDO40u.DSK
```

## Features

### Core Emulation
- **MOS 6502 CPU** — Cycle-accurate, 151 official opcodes, 13 addressing modes, BCD, level-triggered IRQ
- **64KB Memory** — RAM ($0000-$BFFF), ROM ($C000-$FFFF), banking, I/O routing
- **VIA 6522** — 16 registers, Timer 1/2, IFR/IER interrupts, edge-triggered CB1, keyboard matrix
- **ULA Video** — Text mode (40x28) + HIRES (240x200), serial attributes, PAL timing (312 lines x 64 cycles)
- **AY-3-8910 PSG** — 3 tone channels, noise, 16 envelope shapes, SDL2 audio output
- **Microdisc** — WD1793 FDC, 4 drives (A-D), overlay ROM, Sedoric disk boot
- **Cassette** — TAP format, CLOAD/CSAVE via ROM patching, fast load mode, multi-block support, post-CLOAD rechain
- **ACIA 6551** — Serial controller at $031C-$031F, 5 backends (loopback, TCP, PTY, modem AT, COM), V23 mode (Minitel/Digitelec)
- **Digitelec DTL 2000** — Faithful PIA 6821 + ACIA 6850 modem card at $03F8-$03FD (OCR-verified registers, V23 75/1200 & symmetric 1200, line/carrier control, IRQ wired)
- **LOCI** — Lovely Oric Computer Interface (sodiumlb 2024) : MIA bus $03A0-$03BF, 35/36 API ops, USB HID, WD1793 cycle-accurate, FAT16/32 SD image, runtime ROM swap (`--loci`, `--loci-flash DIR`, `--loci-sdimg PATH`). Boote Sedoric V4 master complet via le firmware LOCI.

### ORIC-1 & Atmos Support
- **ROM auto-detection** — Detects BASIC 1.0 (ORIC-1) or 1.1 (Atmos) from ROM header
- **`--model` CLI flag** — Force model selection (`oric1`, `atmos`, `1.0`, `1.1`)
- **ROM-specific tape patching** — Correct patch addresses for both ROM versions

### IJK Joystick
- **IJK interface** — Most common ORIC joystick adapter (active low on PSG Port A)
- **Keyboard mode** — Arrow keys + RCtrl/RAlt as fire (`-j keys`)
- **Gamepad mode** — SDL2 game controller with D-pad, analog stick, A/B/X fire (`-j gamepad`)
- **Hot-plug** — Game controllers detected automatically
- **Blending** — Joystick and keyboard signals combined on Port A

### Centronics Printer & MCP-40 Plotter
- **LPRINT/LLIST capture** — Printer output saved to text file (`-p output.txt`)
- **MCP-40 plotter** — 4-color pen plotter emulation (`--printer-type mcp40`)
- **Plotter commands** — H (Home), D (Draw), M (Move), J (Color), P (Print), L (LineType)
- **480x400 framebuffer** — Bresenham line drawing, 5x7 font, BMP export
- **Centronics protocol** — VIA Port A data + CA2 STROBE edge detection

### Save States
- **`.ost` format** — Binary save state with CRC32 integrity check
- **10 sections** — CPU, MEM, VIA, PSG, VID, KBD, FDC, MDC, TAP, META
- **Hotkeys** — F2 (quick save), F4 (quick load)
- **CLI** — `--save-state FILE`, `--load-state FILE`

### Interactive Debugger
- **Breakpoints** — Up to 16 PC breakpoints, conditional (`b ADDR if EXPR`), 8 raster-line breakpoints (`br LINE`)
- **Watchpoints** — Up to 8 memory write watchpoints
- **Commands** — step, next, **step-out**, continue, **undo** (rewind 16 snapshots CPU+RAM), registers, set, disassembly (paginated with symbol-resolved operands), memory dump+edit, stack
- **Live peripheral introspection** — `via`, `psg`, `disk`/`fdc`, `acia`/`serial`, `tape`, `loci` snapshots
- **Symbols** — Load `.sym`/`.lab`/EQU/VICE formats with `--symbols FILE`. Disasm and trace operands auto-annotated.
- **TUI mode** — ncurses 6-pane interface (regs, stack, disasm, mem, bp+wp, status). Build with `TUI=1`, launch with `--tui`.
- **CLI** — `--debug` (break at start), `--break ADDR`

### IPC Control Mode (OricForge IDE integration)
- **`--control` flag** — Phosphoric speaks a text protocol on stdin/stdout, logs on stderr.
- **24 commands** : `hello`, `regs`, `set`, `read`, `bread` (binary), `write`, `peek <subsys>`, `break`, `unbreak`, `break-list`, `watch`, `raster`, `step`, `next`, `step-out`, `continue`, `pause`, `reset`, `quit`, `load-tap`, `load-rom`, `load-sym`, `disasm`, and more.
- **3 event types** : `EVT ready`, `EVT stopped reason=…`, `EVT halt reason=…`.
- **Async pause** while running, capability negotiation via `hello`, SIGPIPE safe.
- **Python smoke client** (`tests/integration/phos_smoke_client.py`) — stdlib only, ~250 LOC reference implementation.
- **Spec** : [docs/control_protocol.md](docs/control_protocol.md)

### Chromecast Streaming
- **MJPEG server** — HTTP stream at `/stream` (720x672, 3x upscale)
- **WAV audio** — Real-time PSG audio streaming at `/audio`
- **Native CASTV2** — Direct Chromecast control via `--cast-to`
- **mDNS discovery** — `--cast-discover`

### Display Scaling
- **Integer scaling** — x1 (240x224), x2 (480x448), x3 (720x672, default), x4 (960x896)
- **Pixel-perfect** — Nearest-neighbor upscaling, no blur
- **Runtime toggle** — F3 cycles through scale factors
- **CLI** — `--scale N` (1, 2, 3, 4)

### CPU Trace Logging
- **Instruction trace** — Log every CPU instruction with disassembly and register state
- **CLI** — `--trace FILE` to enable, `--trace-max N` to limit
- **Output** — `CYCLES  PC  BYTES  DISASM  A=XX X=XX Y=XX SP=XX P=XX`

### CPU Performance Profiler
- **Execution profiling** — Per-address hit counts and cycle usage across full 64K space
- **Opcode histogram** — Frequency distribution of all 256 opcodes
- **Hotspot report** — Top 20 addresses by execution count and cycle usage
- **CLI** — `--profile FILE` writes report on exit

### ROM Analysis Tools
- **Vector detection** — Extracts RESET, NMI, IRQ hardware vectors
- **Subroutine map** — Scans JSR/JMP targets with reference counts
- **String detection** — Finds ASCII strings in ROM (min 4 chars)
- **Usage statistics** — Code vs data vs fill byte classification
- **Pattern search** — Find arbitrary byte sequences in ROM
- **CLI** — `--rom-info [FILE]` prints to stdout or writes to file

### Modern Features
- **Video export** — PPM, BMP, ASCII screenshots
- **Keyboard layouts** — QWERTY, AZERTY (`--keyboard azerty`)
- **Headless mode** — No display, for CI/automation
- **Host filesystem** — Share files with `--hostfs DIR`
- **Conversion tools** — `bas2tap`, `bin2tap`, `tap2sedoric`
- **Keyboard automation** — `--type-keys CYCLES:TEXT`

## Building

### Prerequisites

```bash
# Debian/Ubuntu
sudo apt-get install build-essential libsdl2-dev

# Fedora
sudo dnf install gcc SDL2-devel

# Arch
sudo pacman -S base-devel sdl2

# Optional: Chromecast support
sudo apt-get install libssl-dev
```

### Build

```bash
make SDL2=1                    # Standard build with SDL2
make                           # Headless build (no SDL2)
make DEBUG=1 SDL2=1            # Debug build (-g -O0)
make SDL2=1 CAST=1             # With Chromecast support
make tools                     # Conversion tools (bas2tap, bin2tap, tap2sedoric)
sudo make install              # Install to /usr/local
```

## Usage

```
./oric1-emu [OPTIONS]

ROM & Model:
  -r, --rom FILE            Load BASIC ROM (required)
  -m, --model MODEL         Force model: oric1, atmos, 1.0, 1.1

Tape & Disk:
  -t, --tape FILE           Load .TAP cassette file
  -f, --fast-load           Fast load (direct memory injection)
  -d, --disk FILE           Load .DSK disk image (drive A)
  --disk-rom FILE           Load Microdisc ROM
  --disk1/2/3 FILE          Drives B/C/D

Save States:
  --save-state FILE         Save state on exit
  --load-state FILE         Load state at startup

Joystick:
  -j, --joystick MODE       Joystick mode: keys, gamepad

Printer:
  -p, --printer FILE        Capture printer output to FILE (LPRINT/LLIST)
  --printer-type TYPE       Printer type: text (default), mcp40

Display:
  --scale N                 Display scale: 1, 2, 3 (default), 4

Trace:
  --trace FILE              Log CPU instruction trace to FILE
  --trace-max N             Max instructions to trace (default: unlimited)

Profiler:
  --profile FILE            Write CPU performance profile to FILE on exit

Analysis:
  --rom-info [FILE]         Analyze ROM: vectors, targets, strings, usage

Debugger:
  -D, --debug               Start in debugger
  --break ADDR              Set initial breakpoint
  --symbols FILE            Load symbol table (.sym / .lab / .sym65 / EQU)
  --tui                     Use ncurses TUI debugger (requires TUI=1 build)
  --control                 IPC control mode for IDE integration (stdin protocol)

LOCI peripheral:
  --loci                    Enable LOCI MIA at $03A0-$03BF
  --loci-flash DIR          Sandbox root for LOCI file ops (implies --loci)
  --loci-sdimg PATH         Raw FAT16/32 SD image (implies --loci)

Serial (ACIA 6551):
  --serial TYPE             loopback | tcp:host:port | pty | modem | com:baud,...
  --serial-v23              V23 asymmetric mode (Minitel)
  --serial-trace FILE       Trace TX/RX/signals with timestamps
  --acia-addr XXXX          Override ACIA base address (default $031C)

Digitelec DTL 2000 (faithful PIA 6821 + ACIA 6850 at $03F8-$03FD):
  --dtl2000 TRANSPORT       loopback | tcp:host:port | pty
  --dtl2000-addr XXXX       Override base address (default $03F8)

Chromecast:
  --cast-server[=PORT]      Start MJPEG server (default 8080)
  --cast-to[=DEVICE]        Cast to Chromecast
  --cast-discover           Discover Chromecast devices

Display & Export:
  --keyboard LAYOUT         qwerty (default) or azerty
  --headless                No display
  --cycles N                Run N cycles then exit
  --screenshot FILE         Screenshot at exit (.ppm/.bmp)
  --screenshot-at N:FILE    Screenshot after N cycles
  --type-keys N:TEXT        Simulate keyboard input
  -v, --verbose             Debug logging
```

### Digitelec DTL 2000 example

The Digitelec DTL 2000 is a memory-mapped V23 modem card (PIA 6821 + ACIA 6850
at `$03F8-$03FD`). A ready-to-run BASIC test program is provided in
`examples/dtl2000-test.bas` (and its auto-run tape `examples/dtl2000-test.tap`).

```bash
# Build the conversion tools (once) if you want to regenerate the tape
make tools
./bas2tap examples/dtl2000-test.bas -o examples/dtl2000-test.tap --auto-run

# Run the test program with a loopback modem (what you transmit comes back)
./oric1-emu -r roms/basic11b.rom -t examples/dtl2000-test.tap -f --dtl2000 loopback

# Connect the card to a real BBS / Minitel-style host over TCP instead
./oric1-emu -r roms/basic11b.rom --dtl2000 tcp:bbs.example.org:23
```

The program drives the card exactly as the period software did — PIA Port A
selects the line/mode, the ACIA carries the data. With `loopback` it reports:

```
TEST 1 TDRE= 1                     transmitter ready
TEST 2 DCD (1=PAS PORTEUSE)= 1     no carrier before connecting
TEST 3 DCD APRES CONNEXION= 0      carrier present after POKE PA,208
TEST 4 LOOPBACK= 10 /10            all bytes echoed back
```

> ⚠️ `$03F8-$03FF` aliases the VIA mirror (and, on real hardware, the Jasmin
> disc electronics). Phosphoric intercepts the range for the DTL 2000 when
> `--dtl2000` is active; avoid enabling it together with a Microdisc/Jasmin.
> Register reference and OCR sources: `docs/digitelec-dtl2000/`.

### Key Bindings

| Key | Function |
|-----|----------|
| F2 | Quick save state |
| F3 | Cycle display scale (x1→x2→x3→x4) |
| F4 | Quick load state |
| F5 | Warm reset |
| F7 | Memory dump (64KB RAM to timestamped .bin file) |
| F9 | Enter debugger |
| F10 | Quit |
| F11 | Fullscreen |
| F12 | Screenshot |

## Testing

```bash
make tests               # All 499 unit tests (100% pass)
make test-cpu            # CPU tests (74)
make test-memory         # Memory tests
make test-io             # VIA/I/O tests
make test-storage        # Storage tests
make test-system         # Integration tests
make test-video          # Video export tests
make test-audio          # PSG audio tests
make test-debugger       # Debugger tests
make test-savestate      # Save state tests
make test-atmos          # Atmos support tests
make test-joystick       # Joystick tests
make test-printer        # Printer tests
make test-mcp40          # MCP-40 plotter tests
make test-renderer       # Display scaling tests
make test-trace          # CPU trace logging tests
make test-profiler       # CPU profiler tests
make test-rominfo        # ROM analysis tests
make test-serial         # ACIA 6551 serial tests
make test-symbols        # Symbol loader tests (.sym / .lab / EQU / VICE)
make test-loci           # LOCI MIA tests (133 tests)
make test-loci-sdimg     # LOCI FAT16/32 SD image tests
make test-loci-sdimg-write # LOCI write API tests
make test-loci-e2e       # 12 end-to-end scenarios (Sedoric boot + IPC control)
make valgrind            # Memory leak detection
make static-analysis     # Compiler warnings analysis
```

End-to-end regression (`make test-loci-e2e`) covers :
- Sedoric V4 boot via LOCI (5 scenarios)
- IPC control protocol handshake + step + break (3 scenarios)
- IPC async pause-while-running
- IPC watchpoint, raster bp, EVT halt cycle_limit, disasm
- IPC Python smoke client (handshake + bread binary read)

## Architecture

```
+-----------------------------------------------+
|                  Phosphoric                    |
+-----------------------------------------------+
|  +--------+  +-------+  +------------------+  |
|  |  6502  |<-|  BUS  |->|  Memory (64KB)   |  |
|  |  CPU   |  +---+---+  |  RAM/ROM/Banking |  |
|  +--------+      |      +------------------+  |
|                   |                             |
|   +---------------+------------------+          |
|   |               |                  |          |
|   v               v                  v          |
|  VIA 6522      Video ULA       AY-3-8910       |
|  (I/O+IRQ)     (Text+HIRES)   (3ch+Noise)     |
|   |               |                  |          |
|   v               v                  v          |
|  Keyboard      Framebuffer       SDL2 Audio    |
|  Microdisc     PPM/BMP Export                   |
|  Cassette      MJPEG Cast                       |
+-----------------------------------------------+
|        SDL2 (Display / Audio / Input)          |
+-----------------------------------------------+
```

## Project Structure

```
src/
  cpu/           6502 CPU (opcodes, addressing modes)
  memory/        64KB memory map, ROM/RAM banking
  io/            VIA 6522, keyboard, cassette, Microdisc, ACIA 6551,
                 LOCI (loci_core + loci_fs + loci_bus + loci_boot)
  video/         ULA rendering (text+HIRES), export (PPM/BMP/ASCII)
  audio/         AY-3-8910 PSG, SDL2 audio output
  storage/       TAP cassette, Sedoric filesystem, WD1793 FDC
  network/       MJPEG cast server, CASTV2 Chromecast client
  hostfs/        Host filesystem sharing, VFS abstraction
  utils/         Logging, INI config parser, CPU trace, profiler,
                 ROM info, symbols loader
  main.c         Emulation loop, CLI, I/O wiring
  savestate.c    Save/load state (.ost format)
  debugger.c     Interactive REPL debugger
  control.c      IPC control mode (--control, OricForge integration)
  tui.c          ncurses TUI debugger (TUI=1 build)

include/         Public headers
tests/unit/      499 unit tests across CPU, memory, I/O, video, audio,
                 storage, debugger, savestate, LOCI, symbols, etc.
tests/integration/ E2E regression (Sedoric boot, IPC control, Python smoke client)
tools/           bas2tap, bin2tap, tap2sedoric
examples/        Example BASIC programs (.bas + .tap)
roms/            ROM files (not distributed)
docs/            User guide, control_protocol.md, CR review docs
```

## ORIC Hardware Reference

| Component | Chip | Details |
|-----------|------|---------|
| CPU | MOS 6502 | 1 MHz, 8-bit |
| RAM | — | 48 KB |
| ROM | — | 16 KB (BASIC 1.0 or 1.1) |
| Video | ULA | Text 40x28, HIRES 240x200 |
| Sound | AY-3-8910 | 3 channels + noise + envelopes |
| I/O | MOS 6522 VIA | Timers, interrupts, keyboard |
| FDC | WD1793 | Microdisc controller (optional) |

## Documentation

- [User Guide](docs/user-guide/README.md)
- [API Reference](docs/api/README.md)
- [Compatibility List](docs/COMPATIBILITY.md)
- [Contributing](CONTRIBUTING.md)
- [Changelog](CHANGELOG)
- [Roadmap](ROADMAP)

## Code generé par IA

> **L'intégralité de ce code a été générée par une intelligence artificielle
> (Claude Opus 4.6, Anthropic)** sous la direction et la supervision d'un
> opérateur humain.

### Avertissements

- **Aucune vérification formelle** : le code n'a pas été audité par un
  ingénieur logiciel professionnel. Bien que 499 tests unitaires + 12
  scénarios E2E passent, la couverture de test n'est pas exhaustive et
  des cas limites peuvent exister.
- **Non adapté à la production** : il s'agit d'un projet expérimental et
  éducatif. Il ne doit pas être utilisé dans des environnements critiques,
  sensibles en termes de sécurité ou en production sans une revue
  indépendante approfondie.
- **Inexactitudes possibles** : la précision de l'émulation matérielle repose
  sur la documentation disponible et des implémentations de référence
  (Oricutron, EUPHORIC). Certains comportements peuvent différer du
  matériel ORIC réel.
- **Sécurité** : le code n'a fait l'objet d'aucun audit de sécurité. Les
  fonctionnalités réseau (serveur cast, CASTV2) ne doivent être utilisées
  que sur des réseaux de confiance.
- **Limites de l'IA** : le code généré par IA peut contenir des erreurs de
  logique subtiles, des pratiques non idiomatiques ou des choix
  architecturaux qu'un développeur humain aborderait différemment.
- **Maintenance** : les mises à jour futures dépendent de la disponibilité
  du modèle IA et peuvent introduire des régressions ou des incohérences
  entre sessions.

Utilisation à vos propres risques. Les contributions et revues de code sont bienvenues.

## Crédits et sources

### Auteurs
- **Claude Opus 4.6 / 4.7 (Anthropic)** — Génération IA du code (architecture, implémentation, tests, documentation)
- **bmarty** — Direction du projet, supervision, tests sur matériel réel

### Émulateurs de référence

Le comportement de Phosphoric s'appuie largement sur l'étude de ces émulateurs :

- **[Oricutron](https://github.com/pete-gordon/oricutron)** (Pete Gordon) — Émulateur ORIC de référence, source principale d'inspiration pour :
  - Table de volume logarithmique du PSG AY-3-8910 (courbe DAC réelle)
  - Diviseurs d'horloge du PSG (TONETIME=8, ENVTIME=16)
  - Décodage du bus PSG via BDIR/BC1 sur PCR
  - Mapping clavier SDL2 (64 touches, matrice QWERTY)
  - Feedback PB3 du scan clavier VIA
  - Pattern d'initialisation RAM (128x 0x00 + 128x 0xFF par page de 256 octets)
  - Détection des attributs série HIRES (masque `(byte & 0x60) == 0`)
  - Timing ULA et rendu vidéo texte/HIRES
- **[EUPHORIC](http://music.riskweb.fr/Fabrice.Frances/Euphoric/english.html)** (Fabrice Frances) — Émulateur ORIC pionnier, travail fondateur sur l'émulation ORIC-1/Atmos

### Documentation technique

- **[MOS 6502 Programming Manual](http://archive.6502.org/datasheets/mos_6502_mpu.pdf)** — Jeu d'instructions, modes d'adressage, timing cycles, mode BCD, bug JMP indirect page boundary
- **[MOS 6522 VIA Datasheet](http://archive.6502.org/datasheets/mos_6522_via.pdf)** — 16 registres, Timer 1/2, IFR/IER, Shift Register, contrôle CA1/CA2/CB1/CB2, protocole handshake Centronics
- **[AY-3-8910 Datasheet](https://f.rdw.se/AY-3-8910-datasheet.pdf)** — PSG : 3 canaux tonaux, générateur de bruit (LFSR 17 bits), 16 formes d'enveloppe, registres I/O
- **[WD1793 FDC Datasheet](https://www.datasheetarchive.com/WD1793-datasheet.html)** — Contrôleur disquette : commandes Type I-IV, registres status/track/sector/data, DRQ/INTRQ
- **[Defence Force / oric.org](https://www.defence-force.org/)** — Documentation technique ORIC (mémoire, ULA, I/O, Microdisc, Sedoric)
- **[ORIC Technical Manual](https://library.defence-force.org/books/)** — Schémas matériels, carte mémoire, interface clavier 8x8
- **[Sedoric documentation](http://music.riskweb.fr/Fabrice.Frances/Sedoric/english.html)** — Système de fichiers disque : 42 pistes x 17 secteurs x 256 octets, structure SED
- **[MCP-40 / CGP-115 Manual](https://www.manualslib.com/manual/1070534/Sharp-Ce-150.html)** — Table traçante 4 couleurs : protocole commandes (H, D, M, J, P, I, L, Q), résolution, interface Centronics
- **[Google Cast V2 Protocol](https://github.com/niccoloterreri/chromecast-protocol)** — Protocole CASTV2 : framing protobuf, TLS, namespaces, CONNECT/LAUNCH/LOAD, heartbeat PING/PONG

### Bibliothèques tierces

- **[stb_image_write.h](https://github.com/nothings/stb)** (Sean Barrett) — Encodeur JPEG header-only, domaine public (v1.16). Utilisé pour le streaming MJPEG du serveur cast.

### Communauté ORIC

- **[Forum Defence Force](https://forum.defence-force.org/)** — Discussions techniques sur le matériel ORIC
- **[CEO (Club Europe ORIC)](http://music.riskweb.fr/)** — Archives de programmes et documentation
- **[ORIC International](https://www.oric.org/)** — Préservation du patrimoine ORIC

## Repository

```bash
git clone https://git.nagominosato.fr:6775/chipinette/Phosphoric.git
cd Phosphoric
make SDL2=1
```

## License

This project is licensed under the [MIT License](LICENSE).

## Contact

- **Maintainer**: bmarty
- **Email**: bmarty@mailo.com

---

Phosphoric v1.16.75-alpha | 499 unit tests + 12 E2E | ORIC-1 + Atmos | LOCI MIA boot Sedoric V4 + ACIA 6551 + IPC control (OricForge) + Symbols + TUI + Conditional/Raster BPs + Rewind + Live peripheral introspection + bread binary + MCP-40 + Printer + Joystick + Cast | 2026-06-07
