# LOCI — Lovely Oric Computer Interface

Emulation of the LOCI peripheral by **Sodiumlightbaby** (sodiumlb, 2024): an
RP2040 cartridge that plugs into the Oric bus and provides mass
storage (USB / SD / internal flash), USB HID keyboard-mouse-gamepads, a WiFi modem
(PicoWiFiModemUSB), hot ROM swapping and a built-in menu.

References: [loci-hardware](https://github.com/sodiumlb/loci-hardware) ·
[loci-firmware](https://github.com/sodiumlb/loci-firmware) ·
[loci-rom](https://github.com/sodiumlb/loci-rom) (menu). The emulation is
aligned with the firmware source (reference release: **v0.3.1**) and
verified against it; known discrepancies are listed at the end of this document.

## Quick start

```bash
# LOCI + picowifi WiFi modem (ACIA 6551 at $0380, address $0380 NOT $03A0)
./oric1-emu -r roms/basic11b.rom --loci --serial picowifi

# LOCI menu straight at boot
./oric1-emu -r roms/loci/locirom --loci

# Raw FAT16/32 SD image as storage
./oric1-emu -r roms/basic11b.rom --loci --loci-sdimg carte.img
```

### One card, three modes (F1 menu)

In the F1 menu, the **LOCI** card has a **Mode** setting; only the settings of the
selected mode are shown:

| Mode | Equivalent to | Settings |
|------|-----------|----------|
| `intégré` | `--loci` | boot menu, SD image, flash folder, picowifi modem |
| `firmware` | `--loci-emu ELF` | firmware ELF (required), flash image, USB stick image |
| `usb` | `--loci-hw PORT` | LOCI-USB port (Feather) |

A single binary contains all three modes: `firmware` exists when `~/loci/emul`
(libemul) was present at build time, `usb` when `~/loci/loci-usb` was (not
on Windows). The menu only offers the modes that are present. Backends are selected
at launch (`src/io/loci_backend.c`). `make LOCI_EMU=0` or `LOCI_HW=0` removes
one.

### LOCI-USB: the Feather (`--loci-hw`, mode `usb`)

A **LOCI-USB** is a LOCI without the Oric interface (no CN1, no level translators), with a
second USB port: today an Adafruit Feather RP2040 USB Host (5723), tomorrow the
LOCI-USB board (`~/loci/loci-usb`). It runs the LOCI firmware itself (`LOCI_USB`
variant, or loci-fw-usb); it is **not** a bridge to a LOCI 1.3 cartridge.
Phosphoric plays the Oric: its emulated 6502 sends its `$03xx` and ROM accesses over USB-C,
and the firmware replays them through its usual bus path; the USB-A port serves as
the LOCI's USB ports (stick, keyboard, modem). It does not plug into a real Oric:
for a physical Oric, you need the LOCI 1.3. Procedure:
`~/loci/loci-usb/docs/RECETTE-FEATHER.md`.

```bash
./oric1-emu --loci-hw /dev/ttyACM0
```

When left empty, the **Port de la LOCI-USB** (LOCI-USB port) setting is detected on Linux from the
USB product name, which starts with "LOCI-USB" (firmware `feature/loci-usb` or
loci-fw-usb). Detection only reads `/sys`, without opening the port. If no LOCI-USB
is detected and no port is given, the menu reports it and refuses
to apply. In `phosphoric.cfg`: `carte.loci=oui`, `loci.mode=usb`,
`loci.port_usb=/dev/ttyACM0`. The legacy keys `carte.loci_emu`, `loci_emu.*`,
`carte.loci_hw`, `loci_hw.port`, `loci.pont` and the value `loci.mode=réelle` are
still read.

**Automatic activation.** Launched without a disk card (no `--disk-rom`, no
`--jasmin-rom`, no LOCI card chosen, on the command line or in `phosphoric.cfg`),
Phosphoric starts on a plugged LOCI-USB, as with `--loci-hw PORT`, if it passes
three checks:

1. **hardware**: USB product « LOCI-USB… » (`/sys`);
2. **protocol**: the loci-usb `PING` answers (protocol version, bridge firmware);
3. **LOCI firmware**: `$0319` reads `'L'` (`LOCI_DSK_IO_ID`, read-only identity
   register, no side effect).

The port is not taken if another program already has it open (`/proc/*/fd`).
Never in `--headless`, under `make tests` (`PHOSPHORIC_NO_CONFIG`) nor when the F1
menu restarts the emulator (the cards are chosen there). `--no-auto-loci` turns the
automatic activation off. The log says what was found or why the card was not taken
(port busy, `$0319` other than `'L'`, no answer). Code: `src/io/loci_hw_probe.c`,
`main_auto_loci_usb()` (main.c).

### In the browser (WebAssembly build)

`phosphoric.html?loci=1` (or the **LOCI** button in the rail) starts on the LOCI
menu, with an internal flash persisted in IndexedDB; loaded files are
copied into it and can be mounted from the menu. Details and limits (no picowifi,
USB sticks or SD image on the web): [wasm.md](wasm.md).

## Memory map

| Window | Contents |
|--------|----------|
| `$0310-$031F` | WD1793 + DSK control (LOCI Microdisc mode; `$0319` = 'L') |
| `$0315-$0317` | Low-level TAP protocol (PLAY/REC/READ_BIT, 14-bit frame) |
| `$0380-$0383` | ACIA 6551 (picowifi modem) — default under `--loci` |
| `$03A0-$03BF` | MIA: UART console, API registers (xstack `$03AC`, errno `$03AD/E`, op `$03AF`), stub `$03B0` (spin/BLOCKED), BUSY `$03B2` bit 7, button trap `$03BA-$03BF` |

## API (op `$03AF`) — 36/36 ops implemented

System (`PIX_XREG`, `CPU_PHI2`→1000 kHz, `OEM_CODEPAGE`, `RNG_LRAND`,
`STDIN_OPT`), clock (`CLOCK`, `CLK_GET/SETTIME`, `GETRES`), files
(`OPEN/CLOSE/READ/WRITE_XSTACK/XRAM/LSEEK/UNLINK/RENAME`), directories
(`OPENDIR/CLOSEDIR/READDIR/MKDIR/GETCWD`), mounting (`MOUNT/UMOUNT`, TAP
`SEEK/TELL/READ_HEADER`, `UNAME`), boot/tuning (`MIA_BOOT`, `MAP_TUNE_*`,
`ADJ_SCAN`), `$FF` sentinel (exit → spin).

### Firmware-compliant ABI (checked against the source)

- **errno**: filesystem errors = `32 + FRESULT` FatFS (missing file → 36,
  missing directory → 37, denied/not empty/full → 39, already exists → 40…);
  codes 1-18 are reserved for API errors (`EBADF`, `EMFILE`, `ENODEV`,
  `ENOSYS`, anti-escape guard) — exactly as in the firmware's `api.h`.
- **Descriptors**: files 3-18 (FAT, `STD_FIL_OFFS=3`), directories 64+
  (`FD_OFFS_FAT`); the device iterator is fd 0 (`FD_OFFS_DEV`).
- **xstack**: 512 bytes, compliant push/pop and NUL-less strings.
- **`MAP_TUNE_*`**: value in register A; A ≤ 31 sets the delay, any
  other value is a *query*; the op always returns the current value
  in AX. `ADJ_SCAN` sweeps tior 0-31 (~100 ms + 5 ms/step) with progress
  visible in ROM byte `$FFF0` (`0x80|tior` then the configured tior).

## Storage

The real LOCI has three tiers; their equivalents in Phosphoric:

| Real hardware | Emulation |
|---------------|-----------|
| **Internal flash** (RP2040 LittleFS, pre-seeded with `basic11b.rom`, `basic10.rom`, `microdis.rom`, `locirom`) | **flash root**: `--loci-flash DIR` (default: current directory). `0:` and bare paths. System ROMs missing from the flash root are resolved as a fallback in the folder of the `-r` ROM (hence `roms/`) |
| **USB stick** (FAT, USB host port) | `--loci-usb DIR` (repeatable, 4 max) **or auto-detection** of media mounted under `/media/$USER` and `/run/media/$USER` at startup. Volume paths `1:`-`4:`. Label + size shown in the menu (`N: MSC x.x GB <label>`). No hot-plug: plug in before launching |
| **SD card** (raw image) | `--loci-sdimg PATH` (FAT16/32). NOTE: when active, this backend owns all file ops (USB sticks remain listed but cannot be browsed) |

The **device list** (menu selector) is served by
`opendir("")`: "0: Internal storage [15MB]", then one line per
USB device (the firmware's `usb_set_status` — the MSC stick, the picowifi
"CDC modem mounted"), then an empty name.

## Action button (F8)

Firmware behaviour (`ext.c`) reproduced:

- **Short press**: session snapshot (→ `<flash root>/loci_resume.ost`),
  IRQ trap `$03BA` (CLV; BVC -2; JMP ($FFFA)), then **boot into the LOCI menu**
  (`LOCIROM`/`locirom` from the flash root, fallback `roms/loci/locirom`). Like the
  real firmware, the FW version (0.3.1) and the timings (tmap/tior/tiow/tiod/
  tadr) are **patched into the ROM** at the placeholders `$FFF7-9` / `$FFEF-F3`.
  The menu's *resume* entry (`MIA_BOOT` + `LOCI_BOOT_RESUME`) swaps the previous
  ROM back in and restores the snapshot. Pressing F8 inside the menu is ignored
  (the session snapshot is preserved).
- **Long press (≥ 2 s)**: boots **Mike Brown's diag ROM**
  (`roms/loci/test108k.rom`, v1.08k, included in the real firmware builds
  with his permission — 60 Hz variant supplied). Step-by-step
  CPU/ULA/DRAM/VIA/PSG test.
- In `--control` mode: command `loci-button [long]`.
- F5 = MIA reset (registers/xstack/op) keeping the mounts, like the
  Pico's reset button.

## MIA bus timing

The MIA samples the 6502 bus through PIO at sub-cycle offsets set
by `MAP_TUNE_*`. A badly tuned `tior` corrupts the picowifi ACIA window —
a real hardware symptom, reproduced: `--loci-mia-window LO-HI` defines the
reliable range (default 0-31 = always reliable); outside the window, `$0380` reads
`$FF` and ignores writes.

`--loci-serve-timing SERVE[,TDSR]` replaces the window with the ns timeline of
`bus_timing.h` (the same one as `--loci-hw`, below): SERVE in LOCI core 1 cycles
(23 measured on hardware), TDSR in ns (100). `tior` and `tiod` enter into it;
`--loci-serve-jitter AMP[,SEED]` adds ± AMP cycles, seeded. Default boundary:
81/82 serve cycles. Details: [`architecture/phi2-bus-timing.md`](architecture/phi2-bus-timing.md).

### API call that replaces the served ROM (`--loci-hw`, 2.29.1)

When the menu ROM writes `MIA_OP` (`$03AF`), then does `JSR MIA_SPIN` (`$03B0`),
the firmware may replace the served ROM: `mia_api_boot` (ESC in the LOCI menu)
loads BASIC into the menu bank and moves gen8. On a real Oric, the 6502 is already
in the iopage loop at that point. Over USB, the reply to the write reports the new
generation before the emulated 6502 has read the `JSR`: so the ROM cache is only
invalidated when **`MIA_SPIN` (`$03B0`) is read** after the `$03AF` write — not on
the first `$03xx` access: the RETURN path (`call_loci_boot`) still reads
`MIA_XSTACK` (`$03AC`) before `PLP` / `JMP MIA_SPIN` (2.29.2). nROMDIS and
nRESET-edge invalidations stay immediate. Without this deferral, ESC froze the
screen on « Booting » (PC=`$0244`) and RETURN ended in a jam (`$B5AD`). With
`LOCI_HW_ROM_NOCACHE=1`, the ROM is read directly and the race remains.

### Warm MENU button (`--loci-hw`, 2.29.2)

With the machine running, pressing F8 is not a reset: the firmware points `$FFFE` at
its `$03BA` trap, pulses nIRQ and waits for the **running** 6502 to enter it to save
the RAM (otherwise, after 2 s, fallback reboot). So `--loci-hw` hands control back as
soon as it sees the nIRQ pulse without an nRESET edge, and delivers it to the 6502 at
the next drain; only a cold press waits for nRESET (at most 10 s). Saving then
restoring (« Return » in the menu) go through `$03A4`.

**Posted writes (2.29.3).** Writes to the XRAM ports `$03A4` (RW0) and `$03A8` (RW1)
return nothing to the 6502: `--loci-hw` buffers them and sends them in packets of 256
(`WRN`, done in order by the firmware). The buffer is flushed before any other bridge
request (`$03xx` read, write to another address, ROM, BAL, LINES, button, HID,
TIMING, reset, end of session) and after 2000 cycles without access: the order seen
by the firmware is unchanged. The nIRQ drain that follows each MIA write does not
flush the buffer (the firmware has not seen those writes yet). Measured on the
Feather: warm save = 17103 bytes in 85 WRN, « Return » menu in ~4 s instead of one to
two minutes. `LOCI_HW_NO_POST=1` goes back to one request per write.

**Grouped reads (2.30.0, firmware with `caps & 0x20` = `LUP_CAP_XSTREAM`).** On the 2nd
consecutive read of a port, `XPEEK` returns in advance the 256 bytes the next reads
would return, without changing anything on the firmware side; they are served without
a request. Before any other bridge request, when exhausted (immediately followed by a
new `XPEEK`) and after 2000 cycles without access, `XADV` replays on the firmware side
the k served reads: the cartridge is in the exact state of k reads of the 6502. No
buffer if STEP = 0 (HID keyboard window rewritten in the background); no new attempt
before a write to the `$03A4`-`$03AB` registers. The nIRQ drain does not settle a
stream in progress. Measured on the Feather (reflashed firmware, caps `3F`): RETURN →
BASIC intact in ~4 s (17075 bytes served in 82 `XPEEK`); full cycle BASIC → F8 → menu
→ RETURN → BASIC in under 10 s. Firmware without `XSTREAM` (caps `1F`): one request
per read, ~12 s. `LOCI_HW_NO_XSTREAM=1`: one request per read. Built against an older
loci-usb client (without `LUP_CAP_XSTREAM`), the code is simply absent.

### loci-fw BAL mailbox (`--loci-hw`)

With the **loci-fw** firmware (LOCI_USB variant, caps `FIRMWARE`), the 6502 calls
the API by writing to page `$FF` (`$FF00-$FFCF`). `--loci-hw` forwards these writes
(`WR`); a captured write comes back with `LUP_F_SERVED`, and the byte is copied into
the ROM cache without reloading it. A non-zero byte at `$FF00` launches a command:
Phosphoric re-reads `$FF00` (uncached `RD`) until it is 0, which avoids waiting for the
next `LINES` (10 s at most, `LOCI_HW_BAL_TIMEOUT_MS`). Exception: group 2
(Console) is never handled by the firmware, the 6502 kernel executes it
(loci-fw ADR-004); Phosphoric therefore does not wait for it. The dispatcher changes `gen8` when returning its results, and the ROM
cache is then reloaded. The old firmware never captures these writes: nothing
changes for it. End-of-session summary: captured writes, commands.

### Φ2 race on the LOCI-USB (`--loci-hw`)

With `--loci-hw`, every `$03xx` access is a USB round trip during which the
emulated 6502 is frozen: the Φ2 constraint of a real LOCI (the expansion port has
no RDY, the data must be driven before the 6502 captures it) disappears. When
the firmware advertises `caps & LUP_CAP_TIMING` (loci-usb, TIMING/RDT/WRT commands),
Phosphoric reconstructs it from the core 1 cycles measured with SysTick (`serve`,
`act`), on a ns timeline whose origin is the falling edge of Φ2:

| Step | Instant | Source |
|---|---|---|
| action word in the FIFO | (22 + tior) PIO ticks + 2 sys cycles | `mia.pio` counts (estimated) |
| data ready (DMA triggered) | + `LOCI_HW_POLL_NS` (0, not measured) + `serve` | SysTick |
| data on the bus | max(ready, Φ2 rise) + (3 + tiod) ticks | `mia_io_read` (estimated) |
| 6502 deadline | period − `LOCI_HW_TDSR_NS` (100) | 6502 datasheet at 1 MHz |

- **PIO tick** = 1 / (Φ2cfg × 30), where Φ2cfg is the firmware setting (**4000 kHz
  by default**, `cpu.c`): 8.33 ns. This is not the Oric's clock.
- **Oric Φ2**: period 1 / `LOCI_HW_PHI2_KHZ` (1000), high during the last third
  (`LOCI_HW_PHI2_HIGH_NS`, period / 3: the ULA gives 2/3 low, 1/3 high).
- **Late read**: the data arrives after the deadline. **Stale iopage**: a
  `$03xx` access arrives less than `act` 6502 cycles after the previous one (granularity:
  the instruction).

Detection only by default: log of the first 10 cases + end-of-session summary
(minimum margin in ns). `LOCI_HW_FAITHFUL=1` returns open-bus on a late
read. Measurements on the Feather 5723: serve 23 cycles → data at ≈ 708 ns, just
after the Φ2 rise, margin ≈ 190 ns; act 61-118 cycles for a read, 429 for a
RAMX write (3.6 µs: a `$03xx` access less than 4 6502 cycles later reads a stale
iopage). The PIO counts and the setup time of the Oric's 6502 at 2 MHz are
estimates, to be confirmed by a measurement on a real bus.

## Known discrepancies (out of scope)

- The LittleFS internal flash is not emulated as such (LFS files 19-20,
  directories 32+, errno `128-lfs_err`, real "0:" volume) — the flash root
  plays that role with FAT semantics.
- The firmware's dev directory "0", the real ULA pattern matcher, BUSY observable
  during a long op.
- USB detection at startup only (no hot-plug).
- The LOCI's `dsk_fdc` keeps fast FDC timing — faithful: on real
  hardware its "drive" is the RP2040 + SD, with no mechanics (the Microdisc,
  on the other hand, uses real mechanical timing by default).

## Going further

- **Popular-science article** — [Five experimental extensions for the LOCI
  board](articles/loci-extensions-tachibana.md) (*fastcall* ABI, MIA register map,
  diagrams): `$A9` coprocessor, reliable ACIA `$AA`, `$A7` bank, `$A8`
  streamer, *tearing* model. These extensions are **experimental, opt-in and not in
  `main`** (branch `experiment/loci-coproc-acia-reliable`).
- **Internal architecture** — [`architecture/loci-glue.md`](architecture/loci-glue.md),
  [`architecture/phi2-bus-timing.md`](architecture/phi2-bus-timing.md).
