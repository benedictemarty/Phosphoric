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
