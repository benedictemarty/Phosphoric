# Driving the Phosphoric emulator from a development agent

A guide for **a development agent** to develop and **automatically test**
an ORIC-1 / Atmos program with the **Phosphoric** emulator (`oric1-emu`).

> Copy this file to the root (or into `docs/`) of a new Oric project.
> Every option mentioned comes from `./oric1-emu --help` — check them with
> `--help` if a version differs. **Never invent an option: check it first.**

---

## 1. Principle: everything is tested *headless*

Phosphoric runs **without a screen** (`-n / --headless`) and can produce artefacts
that an agent can inspect (text, image, RAM, WAV, CPU trace). This is what makes an
Oric program **self-testable** in CI, without human intervention.

Typical loop of an automated test:

1. Boot a ROM + load the program (`.tap` tape, `.dsk` disk, or binary).
2. Let it run for N cycles (`-c`) — or trigger on a memory state.
3. **Capture** an artefact (text/PNG screenshot, RAM dump, WAV…).
4. **Check** the artefact (comparison, pattern, memory value) → exit code.

The CPU runs at 1 MHz, **19968 cycles per frame**, 50 frames/s (PAL).
So: `1 emulated second ≈ 1 000 000 cycles ≈ 50 frames`.

---

## 2. Basic commands

```bash
# Boot BASIC 1.0 (ORIC-1) / BASIC 1.1 (Atmos)
./oric1-emu -r roms/basic10.rom
./oric1-emu -r roms/basic11b.rom

# Headless, run 5 emulated seconds then quit
./oric1-emu -r roms/basic10.rom -n -c 5000000

# Load and run a tape instantly (no manual CLOAD)
./oric1-emu -r roms/basic10.rom -t prog.tap -f

# Load a Sedoric disk
./oric1-emu -r roms/basic10.rom --disk-rom roms/microdis.rom -d disque.dsk

# Choose the model explicitly (otherwise auto-detected from the ROM)
./oric1-emu -r roms/basic11b.rom -m atmos
```

Useful loading options:

| Option | Role |
|---|---|
| `-r, --rom FILE` | system ROM (mandatory in practice) |
| `-t, --tape FILE` | `.tap` tape |
| `-f, --fast-load` | direct injection of the tape (no `CLOAD`) |
| `-d / --disk1/2/3` | `.dsk` disk in drive A/B/C/D |
| `--disk-rom FILE` | Microdisc ROM (`microdis.rom`) |
| `-h, --hostfs PATH` | mounts a host directory |
| `-m, --model` | `oric1` or `atmos` |
| `-k, --keyboard` | `qwerty` (default) or `azerty` |

---

## 3. Injecting keyboard input (`--type-keys`)

To drive a program without a human:

```bash
# Type a BASIC line then Return, after ~2.7 M cycles (end of boot)
./oric1-emu -r roms/basic10.rom -n -c 5000000 \
  --type-keys 2700000:'PRINT "HELLO"\n'
```

Escapes: `\n`=Return, `\e`=Esc, `\b`=Del, `\u \d \l \r`=arrows,
`\Cx`=Ctrl+x, `\Fx`=Funct+x, `\Lx`/`\Rx`=Shift+x, `\pN`=pause for N emulated seconds.

- **`--type-keys` is repeatable**: several occurrences are sequenced by increasing
  arming cycle.
- The pace is **synchronised with the real keyboard scan** → no key is lost, even if
  the program polls slowly.
- **Better than guessing the cycle**: arm on a memory state with
  `--type-keys-when A:V:TEXT` (fires when `RAM[A]==V`, A and V in hex).

```bash
# Wait until the game is ready (RAM[$BC9A]==$52) before typing
./oric1-emu -r roms/basic10.rom -n -c 20000000 \
  --type-keys-when BC9A:52:'\n'
```

> For deterministic injection timing **and** for network serial (modem/XMODEM),
> add `--realtime` (paces headless mode at 50 Hz).

> **Debugging a home-made keyboard scanner** (native ASM game that scans the VIA+PSG matrix
> itself, without the ROM): `--kbd-scan-trace FILE` writes one line per VIA
> Port B read (`$0300`) — `<cycle> col reg7 reg14 matrix PB3`. Two classic causes of a
> "silent scan" become visible: `reg7` bit6=0 (Port A as output → PB3 forced to 0)
> and `matrix`≠`FF` (keys pressed/held). `--psg-trace` **excludes** reg 14/15
> (matrix), hence this dedicated tool.

---

## 4. Capturing a result (the heart of automated testing)

### 4.1 Screen as text (the easiest to check)

```bash
# Dump the 40x28 text screen ($BB80) as ASCII on exit
./oric1-emu -r roms/basic10.rom -n -c 5000000 --screenshot-text out.txt

# At a precise instant (repeatable for several instants)
--screenshot-text-at 3000000:step1.txt --screenshot-text-at 6000000:step2.txt

# Triggered on a memory state (exit 2 if never reached)
--screenshot-text-when BC9A:52:ready.txt
```

### 4.2 Image (PNG/BMP/PPM) and colour ANSI

```bash
--screenshot out.bmp                 # or .ppm, on exit
--screenshot-at 6000000:frame.ppm    # at a given cycle (repeatable)
--screenshot-when A:V:FILE           # on RAM[A]==V
--screenshot-ansi out.ans            # framebuffer as true-colour ANSI text (readable in a terminal)
```

### 4.3 RAM (deterministic check)

```bash
--dump-ram-at 5000000:dump.bin       # 64 KB at cycle >= C
--dump-ram-when A:V:dump.bin         # on RAM[A]==V
```

### 4.4 PSG sound (AY-3-8910)

```bash
--audio-wav son.wav      # 16-bit stereo 44.1 kHz WAV (headless)
--psg-trace psg.log      # log of AY register writes (reg 0-13) + CPU cycle
```

> Checking sound without a human listening: `--audio-wav` then analysis (e.g.
> `ffmpeg -i son.wav -af volumedetect -f null -` to prove a non-silent signal).

### 4.5 Video / frames

```bash
--video demo.avi --video-fps 50      # Motion-JPEG AVI
--frame-dump DIR --frame-dump-interval 25
```

---

## 5. Forcing a state without playing (pokes)

Useful to bring a game to a precise scene deterministically:

```bash
--poke-at 4000000:9611=01      # writes RAM[$9611]=$01 after 4 M cycles (repeatable)
--poke-when A:V:ADDR=VAL       # writes RAM[ADDR]=VAL when RAM[A]==V (all in hex)
```

---

## 6. Debugging

```bash
--debug / -D               # start in the REPL debugger (break on 1st instruction)
-b, --breakpoint ADDR      # break when PC reaches ADDR (hex)
--trace trace.log          # full trace of CPU instructions
--trace-ring N             # keep only the last N instructions (ideal for a hang)
--trace-irq irq.log        # log of IRQ entries + RTI
--symbols prog.sym         # symbol table (.sym / .lab / .sym65)
--rom-info                 # ROM analysis
--gdb[=PORT]               # remote GDB stub (target remote :PORT)
--profile prof.txt         # CPU performance profile
```

---

## 7. Deterministic recording / replay

```bash
--record session.movie     # records keyboard input
--replay session.movie     # deterministic replay (ignores the live keyboard)
--save-state s.ost / --load-state s.ost
```

Replaying a `.movie` (combined with a fixed `-c`) gives **reproducible** runs — ideal for CI.

---

## 8. Automated test skeleton (reusable)

```bash
#!/usr/bin/env bash
# tests/test_smoke.sh — example of a headless e2e test
set -euo pipefail
EMU=./oric1-emu
ROM=roms/basic10.rom
OUT=$(mktemp -d)

# 1. Boot + load the program, capture the screen when it is ready
$EMU -r "$ROM" -n -c 8000000 \
     -t build/prog.tap -f \
     --screenshot-text-when BC9A:52:"$OUT/screen.txt"

# 2. Check the expected result
if grep -q "READY" "$OUT/screen.txt"; then
    echo "PASS"; exit 0
else
    echo "FAIL — screen obtained:"; cat "$OUT/screen.txt"; exit 1
fi
```

Useful exit codes: the `*-when` options return **exit 2** if the state is never
reached within the cycle budget → a failing `--screenshot-*-when` makes the test fail.

---

## 9. Known pitfalls (lessons learned)

- **Black SDL window** on some machines/GPUs → run with `--render-software`.
- **Headless too fast** (~25-45× real time) breaks network serial and injection
  timing → add `--realtime`.
- **`--screenshot-at` with a single variable**: the *last* trigger wins. For
  several instants, use several `--screenshot-*-at`/`-when` (they are repeatable).
- **Changing build mode** (`make SDL2=0` then `make tests`): safe since 2.2.0,
  the objects of each configuration live in `build/<config>/` (previously, mixed
  objects → false regressions, `make clean` was required).
- **Cycle window**: too short = program not ready yet; prefer `*-when` on a
  memory state rather than guessing an absolute cycle.

---

## 10. Key cheat sheet (GUI mode)

`F1` help · `F2` save-state · `F3` scale · `F4` load-state · `F5` reset · `F6` hot-swap
media OSD · `F9` debugger · `F10` quit · `F11` full screen · `F12` screenshot.

---

## Project references

- `README.md` — full CLI reference.
- `docs/user-guide/README.md` — user guide.
- `docs/control_protocol.md` / `docs/http-api.md` — IPC / REST control.
- `docs/loci.md`, `docs/SEDORIC.md` — LOCI and the Sedoric filesystem.
- the project's local rules — conventions of the Phosphoric repository.
