# Architecture — LOCI adapter (`src/io/loci_glue.c`)

Delivered by **Epic 9** (v1.99.5 → v1.99.8). Documents the adapter that connects the
LOCI core to the emulator, following the model of `io-bus.md`.

## 1. The problem

The LOCI (sodiumlb card: WiFi modem + SD storage + ROM swap, exposed as MIA
`$03A0-$03BF`) needs to reach **the whole** emulator: memory, CPU, video,
OSD, keyboard, disks, tape. But its core (`src/io/loci_*.c`) is deliberately
kept **pure** — it does not include `emulator.h`. The result was ~15 "glue"
`loci_*` functions piled up in `main.c` (~400 lines), mixed with the rest of the
assembly code.

## 2. The solution — a dedicated adapter

`src/io/loci_glue.c` is the **only** file that knows both the LOCI and
`emulator_t` — exactly the adapter role of `io_bus.c`. It gathers the
16 glue functions; `main.c` keeps only the **wiring**.

## 3. The contract (the seam)

The LOCI core exposes a callback-registration API taking an opaque
`void* ctx`:

```c
loci_set_rom_swap_callback(&emu.loci, loci_rom_swap_cb, &emu);
loci_set_dsk_bus_callbacks(&emu.loci, loci_dsk_cpu_irq_set, ..., &emu);
loci_set_action_callbacks(&emu.loci, loci_action_install_irq_trap, ...);
```

Each callback does `emulator_t* emu = (emulator_t*)ctx;`. This seam makes
the extraction **mechanically trivial**: move the body, leave the
registration in `main.c`. That is why the whole Epic stayed
byte-identical.

## 4. Split

| In `loci_glue.c` (the adapter) | Stays in `main.c` (the wiring) |
|-----------------------------------|----------------------------------|
| The 16 `loci_*` functions (bus/IRQ callbacks, ROM/tape/resume, USB, IRQ trap, SDL keyboard sync) | `loci_init(&emu.loci)`, the `loci_set_*_callback(...)` calls, the setup calls (`loci_attach_usb_dir`, `loci_scan_host_usb`), and the `loci_sync_kbd_from_sdl` calls in the SDL event loop |

After the Epic, **`main.c` no longer contains any `loci_*` function
*definition*** — only calls.

## 5. The extraction (US1 → US5)

Carried out in batches of increasing risk, each verified byte-identical.

- **US1 — mapping + oracle.** Analysis by function boundaries: a **single**
  blocker (see §6), a **single** SDL function, clean seam. Determinism oracle
  established (§7). See `loci-glue-carto.md`.
- **US2 — bus/IRQ callbacks.** `loci_dsk_cpu_irq_set/_clr`,
  `loci_dsk_sync_overlay`, `loci_rom_poke_hook`.
- **US3 — ROM/tape/resume.** 7 functions + unblocking `get_rom_patches` (§6).
- **US4 — USB + IRQ trap.** `loci_attach_usb_dir`, `loci_scan_host_usb`,
  `loci_action_install/release_irq_trap`.
- **US5 — SDL glue.** `loci_sync_kbd_from_sdl` under `#ifdef HAS_SDL2`.

## 6. The two decoupling points

**`get_rom_patches` (US3).** `loci_rom_swap_cb` re-selects the ROM patches
after a swap via `get_rom_patches`, which until then was static in `main.c`. Since
`loci_glue.c` is part of `LIB_OBJECTS` (linked by test binaries **without
`main.o`**), a mere declaration is not enough: the **definition** must live
in LIB. The tables `rom_patches_basic10/11` + `detect_rom_version` +
`get_rom_patches` were therefore extracted into a new module
`src/rom_patches.{c,h}`. `include/emulator.h` **was not modified** (the
declaration lives in `rom_patches.h`) → no impact on projects that
embed the emulator header.

**The SDL layer (US5).** A single function (`loci_sync_kbd_from_sdl`) depends on
SDL. It is isolated under `#ifdef HAS_SDL2` with the `#include <SDL2/SDL.h>`
**inside** the guard, so that **`loci_glue.c` also compiles with `SDL2=0`**.

## 7. The non-regression oracle

The headless LOCI boot is **byte-deterministic**. Test `test-loci-golden`
(`tests/integration/test_loci_golden.sh`, in `make tests`): two boots →
byte-identical RAM dumps + non-empty screen. Frozen reference to compare
before/after each batch:

```
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --headless -c 40000000 --dump-ram-at 39000000:REF.bin
# md5(REF.bin) = bf4dff781e05175ad8050815eccd6a42
```

This md5 stayed **unchanged** from US1 to US5 — proof that the extraction altered
nothing. (Distinct from `test-loci-e2e`, a *comparative* test of LOCI vs native
Microdisc with a pre-existing Sedoric rendering difference, outside `make tests`.)

## 8. Outcome

- `main.c`: **4988 → 3744 lines** over the whole slimming effort
  (of which ~400 lines for the LOCI glue).
- New modules: `src/io/loci_glue.{c,h}`, `src/rom_patches.{c,h}`.
- `SDL2=0`, `SDL2=1`, `SDL2=1 HTTPAPI=1` builds OK; full suite green;
  byte-identical binary → no impact on projects using the emulator.

## 9. References

- `docs/architecture/loci-glue-carto.md` — US1 mapping (table of the 16 functions).
- `docs/architecture/io-bus.md` — the twin adapter (`io_device_t`).
- `include/io/loci_glue.h`, `src/io/loci_glue.c` — the adapter.
- `tests/integration/test_loci_golden.sh` — the oracle.
