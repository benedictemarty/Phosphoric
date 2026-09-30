# LOCI glue mapping (Epic 9 / US1)

US1 deliverable: prepare the extraction of the `loci_*` functions from `main.c`
into a dedicated adapter `src/io/loci_glue.c` (same role as `io_bus.c`). This
document lists the functions, their blocking dependencies, the SDL layer, and
sets the non-regression oracle.

## 1. Inventory of the `loci_*` functions in `main.c`

Measured on `src/main.c` (analysis by function boundaries + intersection of the calls
with the statics of `main.c`).

| Function | ~L | SDL | Non-`loci_` `main.c` statics called | Batch |
|----------|----|-----|--------------------------------------|-----|
| `loci_dsk_cpu_irq_set`        |  4 | — | — | US2 |
| `loci_dsk_cpu_irq_clr`        |  4 | — | — | US2 |
| `loci_dsk_sync_overlay`       |  5 | — | — | US2 |
| `loci_rom_poke_hook`          |  5 | — | — | US2 |
| `loci_resume_snapshot_path`   |  4 | — | — | US3 |
| `loci_find_rom_file`          | 16 | — | — | US3 |
| `loci_find_menu_rom`          |  4 | — | — | US3 |
| `loci_patch_rom_info`         | 20 | — | — | US3 |
| `loci_tape_mount_cb`          | 35 | — | — | US3 |
| `loci_rom_swap_cb`            | ~101 | — | **`get_rom_patches`** | US3 |
| `loci_resume_session_cb`      | 13 | — | — | US3 |
| `loci_attach_usb_dir`         | 23 | — | — | US4 |
| `loci_scan_host_usb`          | 22 | — | — | US4 |
| `loci_action_install_irq_trap`| 21 | — | — | US4 |
| `loci_action_release_irq_trap`| 45 | — | — | US4 |
| `loci_sync_kbd_from_sdl`      | 30 | **SDL** | — | US5 |

Cross-calls `loci_*` → `loci_*` (e.g. `loci_scan_host_usb` → `loci_attach_usb_dir`,
`loci_action_release_irq_trap` → `loci_rom_swap_cb`/`loci_find_*`) are **not**
blockers: these functions all migrate **together** into `loci_glue.c`.

## 2. The only real blocker: `get_rom_patches`

**Decisive result**: of the 16 functions, **only one** calls a `main.c` static
that is not itself a `loci_*`: `loci_rom_swap_cb` calls
**`get_rom_patches(oric_model_t)`** (a pure selector over the `rom_patches_basic10/11` tables,
~7 lines) to re-select the ROM patches after a swap.

→ **Unblocking action (US3)**: make `get_rom_patches` non-static and declare it
in a shared header (the `rom_patches_basic*` tables stay static in
`main.c` — only the selector is exposed). No other decoupling work is
required: the registration seam (`loci_set_*_callback(&emu.loci, cb, &emu)` +
`void* ctx`) makes the extraction mechanically identical to `tape_patches` /
`serial_transport_create`.

## 3. Isolated SDL layer (US5)

**Only one** function depends on SDL: `loci_sync_kbd_from_sdl`
(`SDL_GetKeyboardState`, `SDL_GetModState`, `SDL_SCANCODE_*`). It will be placed
under `#ifdef HAS_SDL2` in `loci_glue.c`, which **must build with `SDL2=0`**.

## 4. Non-regression oracle (golden)

The comparative test `test_loci_sedoric_e2e.sh` (`make test-loci-e2e`, **not in**
`make tests`) checks LOCI *correctness* **vs** native Microdisc; its 5
"screen text" failures are a **pre-existing, stable Sedoric-via-LOCI rendering discrepancy**
(unrelated to this refactor) — it is **not** the right oracle here.

For the glue extraction, the oracle is **self-referential**: a headless LOCI boot
is **byte-deterministic**. New test `test-loci-golden`
(`tests/integration/test_loci_golden.sh`, **in `make tests`**): two LOCI
boots → byte-identical RAM dumps + non-empty screen.

Frozen reference to compare before/after each US (media
`roms/loci/locirom` + `loci_demo.img`):

```
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --headless -c 40000000 --dump-ram-at 39000000:REF.bin
# md5(REF.bin) = bf4dff781e05175ad8050815eccd6a42   (pre-Epic-9 binary)
```

US2-US6 procedure: after each batch, regenerate this dump and check that the `md5` is unchanged.

## 5. Confirmed breakdown

- **US2** (safe batch, bounded coupling): `loci_dsk_cpu_irq_set/_clr`,
  `loci_dsk_sync_overlay`, `loci_rom_poke_hook`.
- **US3** (ROM/tape/resume): `loci_tape_mount_cb`, `loci_rom_swap_cb`
  (+ expose `get_rom_patches`), `loci_resume_session_cb`, `loci_patch_rom_info`,
  `loci_find_rom_file`/`_menu_rom`, `loci_resume_snapshot_path`.
- **US4** (USB + IRQ trap): `loci_attach_usb_dir`, `loci_scan_host_usb`,
  `loci_action_install_irq_trap`, `loci_action_release_irq_trap`.
- **US5** (SDL): `loci_sync_kbd_from_sdl` under `#ifdef HAS_SDL2`.
- **US6**: `main.c` keeps only the `loci_set_*_callback(...)` calls.

**US1 conclusion**: the Epic is feasible without surprises — a single
decoupling point (`get_rom_patches`), a single SDL function, a deterministic oracle
in place. No blocking `main.c` static beyond `get_rom_patches`.
