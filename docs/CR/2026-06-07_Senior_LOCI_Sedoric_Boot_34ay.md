# Senior Engineering Review — LOCI Sedoric boot stage 1 (sprints 34aw → 34ay)

**Date**: 2026-06-07
**Versions delivered**: v1.16.50 → v1.16.53-alpha (4 versions)
**Author**: bmarty
**Request**: architectural review of the LOCI DSK bus + diagnosis of the Sedoric stage 2 hang

---

## 1. TL;DR

This series fully rewires the DSK (Microdisc) bus onto LOCI:
MFM_DISK parsing, WD1793 driven by the existing cycle-timed `fdc_t`,
dynamically enabled Microdisc ROM overlay, and asynchronous IRQ propagated to the
CPU when the sodiumlb firmware polls it.

**E2E result**: starting from `roms/loci/locirom`, the complete pipeline
works:

```
LOCI MIA_BOOT(FDC) → swap ROM $C000 (basic11b) + overlay $E000 (microdis)
                  → re-reset CPU → boot Microdisc → Restore + Read Sector $01/Track 20
                  → Sedoric boot loader loaded into RAM $0400-$04FF
                  → JMP $0400, loader runs, screen shows « Booting. »
                  → STUCK at PC=$04F7 (loader stage 2)
```

Stage 1 (Microdisc ROM → Sedoric loader in RAM transfer) **succeeds**.
Stage 2 (loading the Sedoric kernel from the RAM loader) hangs; main
hypothesis: Read Multiple or DRQ pacing missing.

| PR | Sprint | Version | Files | LOC |
|----|--------|---------|-------|-----|
| 1 | 34aw | 1.16.50 | `fdc_t` rewired into loci, MFM_DISK parser | ~400 |
| 2 | 34ax | 1.16.52 | CTRL register semantics (INTENA/ROMDIS/EPROM) | ~200 |
| 3 | 34ay | 1.16.53 | asynchronous INTRQ → cpu_irq | ~30 |

484 tests pass (118 LOCI + 366 core), 0 regressions.

---

## 2. Upstream context

### Before this series

- Sprint 34av had delivered the `--type-keys loci-hid:` prefix: LOCI TUI
  navigation automatable through the synthetic USB HID stack. Selecting a
  disk in the TUI used `Space` (not `Return`, a documented user
  surprise).
- The LOCI ROM accepted `MIA_BOOT(FDC)` but the DSK bus was only a stub:
  CTRL/STATUS/SECTOR/DATA in RAM, no real Microdisc behind it. So
  Sedoric could not start.

### Key architectural decision

The initial question: *reimplement a WD1793 in `loci.c` or reuse
the existing `fdc_t` from `src/storage/disk.c`?*

I chose to **reuse**. Rationale:

| Argument | Choice made |
|----------|--------------|
| The `fdc_t` is already cycle-timed, level-triggered, 412 LOC battle-tested through `test-storage` | ✓ Avoids duplication; consistency with the native Microdisc |
| The disk format on the LOCI side is MFM_DISK (`MFM_DISK` header of 256 bytes + raw MFM tracks) — different from the flat image expected by `fdc_t` | Reuse the existing `sedoric_load()` to parse MFM_DISK → flat array, then inject it into `fdc_t` |
| LOCI has its own I/O layout ($0310-$031F shared between Microdisc/LOCI) | Bridge through the `dsk_cpu_irq_set/clr` + `dsk_sync_overlay` callbacks |

**Consequence**: the LOCI DSK bus is no longer a stub but a real WD1793.
Trade-off: strong coupling with `storage/disk.c`. Acceptable since the disk
format is fixed by the Microdisc ROM (a single standard to support).

---

## 3. Delivered architecture

### 3.1 Sprint 34aw — Rewiring `fdc_t`

```c
/* include/io/loci.h */
typedef struct loci_s {
    /* ... */
    fdc_t    dsk_fdc;                  /* WD1793, cycle-timed */
    uint8_t* dsk_image[4];             /* 4 drives, flat sector arrays */
    uint32_t dsk_image_size[4];
    uint8_t  dsk_tracks[4];
    uint8_t  dsk_sectors[4];
    uint8_t  dsk_intrq;                /* Active-low (0x00 asserted, 0x80 idle) */
    bool     dsk_intena;               /* CTRL bit 0 */
    /* ... */
} loci_t;
```

`dsk_open(drive, path)` reads the file, detects `MFM_DISK` (memcmp of 8 bytes),
calls `sedoric_load()` which returns a flat array of
`sides * tracks * sectors * 256`. SEDO40U.DSK = 696,320 bytes, 80 tracks,
17 sectors, 2 sides.

Reads of $0310-$0317 are routed to `fdc_read(&l->dsk_fdc, reg)`; writes
to `fdc_write()`. The `fdc_t`'s DRQ/INTRQ are bridged to `loci.dsk_drq`
and `loci.dsk_intrq` through callbacks.

**Active-low convention**: all flags exposed to the CPU are OR'd with
`0x7F` (DRQ/INTRQ only use bit 7, the 7 low bits are
floating on the real bus). This is what the Microdisc ROM expects.

### 3.2 Sprint 34ax — CTRL $0314 semantics

```
Bit 0 : INTENA   — enable IRQ to the CPU
Bit 1 : ROMDIS   — disable BASIC ROM ($C000-$DFFF)
Bit 3 : DENSITY  — single/double (informational, MFM only)
Bit 4 : SIDE     — head select
Bit 5-6 : DRIVE  — 0-3
Bit 7 : EPROM    — Microdisc overlay $E000 (toggled during boot)
```

A CTRL write synchronises immediately through 3 callbacks:

```c
void loci_set_dsk_bus_callbacks(loci_t* l,
    dsk_irq_cb irq_set,    /* cpu_irq_set(IRQF_DISK) */
    dsk_irq_cb irq_clr,    /* cpu_irq_clear(IRQF_DISK) */
    dsk_sync_cb sync_overlay,  /* memory.basic_rom_disabled + overlay_active */
    void* ctx);
```

**Pitfall avoided**: an initial attempt where `sync_overlay` set
`basic_rom_disabled = false` according to ROMDIS regressed by replacing the Microdisc
ROM with BASIC mid-execution → the "insert system disc" screen came
back. Fix: `basic_rom_disabled` stays persistently `true` after MIA_BOOT;
only `overlay_active` (EPROM bit) follows dynamically.

### 3.3 Sprint 34ay — Asynchronous IRQ

Diagnosis: after Restore then Read Sector, the `fdc_t` sets
`delayed_int = 20` (cycles). When it expires, `fdc_ticktock()` calls
`set_intrq()`. My first code only asserted `cpu_irq_set` in the
CTRL write handler — so the asynchronous IRQ fired by the FDC never reached
the CPU. The Microdisc ROM polls $0314 waiting for bit 7 = 0 and
looped forever.

```c
static void loci_fdc_set_intrq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (!l) return;
    l->dsk_intrq = 0x00;
    if (l->dsk_intena && l->dsk_cpu_irq_set) {
        l->dsk_cpu_irq_set(l->dsk_bus_ctx);
    }
}
static void loci_fdc_clr_intrq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (!l) return;
    l->dsk_intrq = 0x80;
    if (l->dsk_cpu_irq_clr) {
        l->dsk_cpu_irq_clr(l->dsk_bus_ctx);
    }
}
```

`IRQF_DISK` is level-triggered → symmetrical set/clr.

**Observed effect**: « Booting. » is displayed, the CPU leaves the Microdisc ROM and
executes from RAM (`PC=$04F7` at timeout). Stage 1 succeeds.

---

## 4. Current state — Stage 2 stuck

Reproducible E2E:

```bash
./oric1-emu -r roms/loci/locirom --loci \
    --loci-sdimg loci_demo.img --keyboard azerty \
    --type-keys '15000000:\p3a\p2 \p2 \p2\e\p9\p9\p9' \
    --dump-ram-at 56000000:/tmp/screen.bin
```

On the $BB80 text screen:
```
14: Booting.
```
And that's all. The CPU ends at `PC=$04F7 A:06 X:BA Y:04 SP:AB`.

### Hypotheses for the hang

1. **Read Multiple not implemented**: the Sedoric loader might issue
   $9C (Read Multiple) instead of $80 (Read Sector) to load the kernel.
   The `fdc_t` explicitly handles only Read Single. To be checked in the
   `src/storage/disk.c:fdc_write()` opcode dispatch.
2. **DRQ pacing too fast**: the current `fdc_t` offers the next byte
   immediately after DATA is read. Sedoric V4 might have a
   strict sequence waiting for an IRQ between bytes (unlikely but to be
   ruled out).
3. **Wrong sector layout**: MFM_DISK declares 17 sectors/track but
   Sedoric V4.0 may expect an interleaved layout that `sedoric_load`
   flattens naively. To be checked by dumping sector 1/track 20
   and comparing it with a known Sedoric boot loader.

### Suggested diagnostics

- Trace `--trace dsk.log` filtered on the FDC opcodes sent after PC=$0400
- Read `mem[$0400-$04FF]` at the moment of the stop: if it really is the Sedoric
  loader, disassemble it and identify the loop at $04F7
- Compare with Oricutron: boot the same `SEDO40U.DSK` flat (without LOCI)
  via `--disk-rom roms/microdis.rom -d SEDO40U.DSK`. If it boots there but
  not via LOCI, the LOCI→FDC bridge is what regresses.

---

## 5. Tests

| Suite | Before 34aw | After 34ay |
|-------|------------|------------|
| test-loci | 115 | 118 (+3 WD1793 dsk tests) |
| test-storage | 8 | 8 (regression test_dsk_drq updated for the `0x7F` mask) |
| Total | 481 | 484 |

No regressions apart from test_loci_dsk_drq_register (expectation updated
for the new active-low mask).

---

## 6. Open questions for the senior

1. **Stage 2 strategy**: go further with instrumentation (trace
   targeted at the Sedoric loader) or rewire LOCI onto the native Microdisc path
   (`microdisc_t`) rather than `fdc_t` directly? The native Microdisc has
   more plumbing (proper $0314 overlay register, IRQ withdrawn after RTI).
2. **Vendoring a reference Sedoric loader binary**: for deterministic E2E
   validation, dump a known-correct loader (from Oricutron for
   instance) and compare it with the 256 bytes of sector 1/track 20 read by LOCI.
   Is this within the project's legal scope?
3. **`fdc_t` Read Multiple**: if confirmed missing, add it to the
   `fdc_t` core (which also benefits the native Microdisc) or shim it in
   `loci.c`?

---

## 7. Proposed acceptance criteria for sprint 34az

- Sedoric V4.0 displays `SEDORIC V4.0 — (C) 1987` then the READY prompt
- `DIR` lists the contents of SEDO40U.DSK
- 484 tests + Sedoric V4 boot E2E tests (a new test-loci-sedoric)
- No regression of the native Microdisc (existing `./oric1-emu --disk-rom -d`)

---

*Branch:* `sprint-34ay`
*Key commits:* f967224 (34ay), earlier ones for 34aw/34ax
*Reproducible:* E2E command in section 4
