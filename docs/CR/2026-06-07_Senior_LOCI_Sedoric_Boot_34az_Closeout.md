# Senior review closeout — Sedoric boot via LOCI resolved (sprint 34az)

**Date**: 2026-06-07
**Version delivered**: v1.16.54-alpha
**Upstream reference**: `2026-06-07_Senior_LOCI_Sedoric_Boot_34ay.md` + senior's reply

---

## 1. TL;DR

Sedoric V4.0 boots completely via LOCI. The DOS menu displayed is identical to
the native Microdisc boot, with the prompt « Faites votre choix : » ("Make your
choice:") ready to receive keyboard input. 484 tests PASS.

Bug found by the senior, fixed in 4 lines.

---

## 2. Follow-up on the senior's hypotheses

| Senior hypothesis | Status after verification |
|------------------|---------------------------|
| H1 missing Read Multiple: FALSE | ✅ Confirmed via `disk.c:292` + `169-184` |
| H3 naive interleave: FALSE | ✅ Confirmed via `sedoric.c:74-75` (de-interleave by address mark ID) |
| CTRL bridge does not propagate SIDE/DRIVE | ❌ Refuted for my 34ax code — propagation already present (`loci.c:1935-1939`) |
| Native Microdisc differential test | ✅ Run: **complete** boot in native mode → bug 100 % on the LOCI bridge side |

The senior had the **right diagnostic framework** (differential test + audit
of the CTRL callbacks), even though the precise delta was not SIDE/DRIVE.

---

## 3. Actual bug: ROMDIS hardcoded to true

### Faulty code (sprint 34ax, `loci.c:1944` before the fix)

```c
if (loci->dsk_sync_overlay) {
    loci->dsk_sync_overlay(loci->dsk_bus_ctx, true /* ← hardcoded */, diskrom);
}
```

### Native reference (`microdisc.c:127`)

```c
md->romdis = (value & MICRODISC_CTRL_ROMDIS) == 0;  /* Bit 1: 0=ROM disabled */
```

Then `main.c:657`:
```c
emu->memory.basic_rom_disabled = emu->microdisc.romdis;  /* dynamic */
```

### Why stage 2 was stuck

Sedoric V4.0, after loading its kernel into overlay RAM
($C000-$DFFF + $E000-$FFFF), writes `(value & 0x02) == 1` to CTRL
(ROMDIS deasserted) every time it wants to call a BASIC ROM routine
($C000-$DFFF) — for example for screen formatting or keyboard
handling. All while leaving EPROM=0 to keep the Microdisc ROM accessible
at $E000.

With my hardcoded `basic_rom_disabled=true`, the BASIC ROM **never**
came back: every JSR to $C0xx landed in overlay RAM (read before
any write, hence 0x00) → dead loop at $04F7.

### Fix delivered (`loci.c:1929-1944`)

```c
bool diskrom = (value & 0x80) == 0;   /* bit 7 active-low : EPROM */
bool romdis  = (value & 0x02) == 0;   /* bit 1 active-low : ROMDIS */
/* ... */
if (loci->dsk_sync_overlay) {
    loci->dsk_sync_overlay(loci->dsk_bus_ctx, romdis, diskrom);
}
```

4 clean lines, an exact mirror of the native code.

---

## 4. Sprint 34ax regression explained

The comment I had left in the 34ax code said:

> *"basic_rom_disabled stays persistently true since rom_swap_cb
> (otherwise the running Microdisc ROM disappears from the mapping)."*

That was a wrong conclusion: the Microdisc ROM at $E000-$FFFF is
controlled by **`overlay_active`** (EPROM bit), not by
`basic_rom_disabled` (which controls $C000-$DFFF). Sprint 34ax had been
done on a bad reproducer (probably at the moment T when Sedoric had
not yet switched its DOS into overlay). The dynamic 34az fix corrects both
cases because the EPROM bit and the ROMDIS bit are independent by construction.

---

## 5. E2E validation (committable differential test)

Native command (reference):
```bash
./oric1-emu -r roms/basic11b.rom --disk-rom roms/microdis.rom \
    -d /tmp/SEDO40U.DSK -c 40000000 \
    --dump-ram-at 35000000:/tmp/native.bin
```

LOCI command:
```bash
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --keyboard azerty \
    --type-keys '15000000:\p3a\p2 \p2 \p2\e\p9\p9\p9' \
    -c 40000000 --dump-ram-at 35000000:/tmp/sedoric.bin
```

Both RAM dumps show an identical $BB80 text screen (10 menu
entries, keyboard prompt). This is the natural candidate for a future
E2E `test-loci-sedoric` if the legal scope of the SEDO40U.DSK dump is
clarified, or via a clean-room mini boot disk (senior suggestion Q2).

---

## 6. Remaining open questions (following the senior's reply)

| Q | Position adopted |
|---|------------------|
| Q1 Switch to the full `microdisc_t` | **Postponed** — the option is architecturally sound (eliminates the whole class of CTRL bugs), but the current `fdc_t` bridge works end to end and the refactor would cost ~200 LOC. To be reconsidered if another CTRL bug shows up in disk-write mode. |
| Q2 Clean-room Sedoric loader for a committed E2E | **Noted** — to be done if we want a versioned test-loci-sedoric. Not urgent: the local fixture + differential command are enough for development. |
| Q3 Read Multiple in the core | Not applicable (already implemented). |

---

## 7. Meta-lesson

Differential test **before** heavy instrumentation: 1 command, 90
seconds of wall-clock time, and the cause is isolated. This is the most
cost-effective senior suggestion of this series. To be added to the LOCI E2E playbook
for future "boots stage 1 then hangs" bugs.

---

*Branch:* `sprint-34ay`
*Key commits:* `f967224` (34ay INTRQ) + the next one (34az dynamic ROMDIS)
*Differential test:* `/tmp/native.bin` vs `/tmp/sedoric.bin` at cycle 35M
