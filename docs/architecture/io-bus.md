# Architecture — I/O bus & peripherals (`io_device_t`)

> **Branch `feature/io-device-bus`.** Moves out of `main.c` the cascade of `if
> (has_X && X_addr_in_range(addr)) return X_read(...)` (the hard-wired page 3
> dispatch, ~25 peripherals) in favour of a **peripheral table**. Goal:
> adding/removing a peripheral = registering/removing **one entry**, without
> touching the core. Removing OCULA showed the cost of lacking this
> abstraction (code sprinkled across main/memory/video/savestate).

## 1. The problem

`main.c` (~4600 lines) is a god object: every peripheral is re-wired into it by
hand in 5+ places (CLI, init, **I/O dispatch**, main loop,
savestate, glue). `io_read_callback`/`io_write_callback` are two cascades of
`if`s with priorities and **cross-dependencies** (ACIA↔Microdisc, ORICON window
overlapping the Microdisc, LOCI MIA reliability for the ACIA at $0380…).

## 2. What is a "bus peripheral" (and what is not)

Criterion: **claims an address range in page 3** (± overlay ROM).

- **Core** (do NOT treat as a peripheral): 6502, memory, **VIA**, ULA,
  PSG, keyboard. Soldered in, they are the Oric.
- **Bus devices** (target of `io_device_t`): **Microdisc** ($0310-$031F),
  **ACIA 6551** ($031C-$031F), **ULA-NG** ($0340-$035F), **LOCI**
  ($03A0-$03BF + TAP/DSK), **DTL 2000** ($03F8-$03FD), **Mageco/ORICON**
  ($03FE-$03FF / $031C-$031E).
- **Port-attached** (peripherals, but NOT bus devices): joystick
  (PSG Port A), printer/MCP-40 (VIA Port A + CA2), cassette (VIA CB1). They
  are driven through the existing port callbacks — **outside** this abstraction.

## 3. The contract

`include/io/io_device.h`:

```c
typedef struct io_device_s {
    const char* name;
    bool    (*claims)(struct emulator_s* emu, uint16_t addr);        /* READ claim (+ write by default) */
    uint8_t (*read)(struct emulator_s* emu, uint16_t addr);
    bool    (*write)(struct emulator_s* emu, uint16_t addr, uint8_t value); /* true = consumed, false = VIA fallback */
    bool    (*claims_write)(struct emulator_s* emu, uint16_t addr);  /* optional (NULL → claims) */
    const char* save_tag;                                            /* .ost section (NULL → none) */
    bool    (*save)(struct emulator_s* emu, FILE* fp);               /* false → no section */
    void    (*load)(struct emulator_s* emu, FILE* fp, uint32_t size);
    uint8_t (*peek)(struct emulator_s* emu, uint16_t addr);          /* side-effect-free read */
    size_t  present_off;                                             /* offsetof(emulator_t, has_X) */
    void    (*tick)(struct emulator_s* emu, int cycles);             /* advancing time (optional) */
} io_device_t;
```

The `io_bus[]` table (`src/io/io_bus.c`) is written with **designated initialisers**
indexed by an `enum` (`DEV_LOCI`, `DEV_ACIA`…): the dispatch order can be read from
the `enum`, and the tick order (`io_bus_tick_order[]`) refers to the entries by
that same name.

**Why `emulator_t*` rather than a plain `self`?** Because the `claims`
are conditional/cross-dependent: `microdisc.claims` must know whether the ACIA is
present; the ACIA at $0380 must consult the LOCI MIA reliability. The full
context is required. (A `self` alone would do for an isolated device, but
not for the real priority graph.)

**`write` returns "consumed"**, with a separate `claims_write`.** A write can
**decline** (return `false`) to fall back to the VIA — essential for the
ULA-NG, which must *see* writes to its window even while locked (to
watch for the 'N','G' unlock sequence) while letting neutral
bytes through exactly as the VIA would. Since its write claim (window only)
differs from its read claim (unlocked + window), an optional `claims_write`
is added (NULL → reuses `claims`). Peripherals with an exclusive range
leave `claims_write` NULL and always return `true`.

**Dispatch** (`main.c`): an `io_bus[]` table. On read, `io_bus_find(emu, addr)`
returns the **first** device whose `claims()` matches; on write, `io_bus_find_write`
uses `claims_write ?: claims`, then the dispatch honours the verdict of `write`
(false → fall back to VIA). Table order = priority. `io_read/write_callback`
shrink to **the bus loop + the VIA fallback** — all range-based peripherals are
now in the table (**strangler pattern** carried through to the end).

## 4. Step 1 done — proof of the model

**Digitelec DTL 2000** ($03F8-$03FD, **exclusive range** → behaviour-identical
migration, no priority risk) migrated behind `io_device_t`:
wrappers `dtl2000_dev_{claims,read,write}`, entry in `io_bus[]`, hard-wired `if`s
removed from the 2 callbacks. **Full suite green** (test-dtl2000 15/15 + integration).

## 5. Migration order

1. ✅ **DTL 2000** (done — exclusive range, validates the contract).
2. ✅ **ACIA 6551**, **Mageco / ORICON**: small ranges overlapping the Microdisc
   → priority encoded in the table order + the `claims` (the ACIA owns
   $031C-$031F if present; the ACIA at $0380 consults the LOCI MIA reliability).
3. ✅ **Microdisc**: `claims` = `has_microdisc && 0x0310-0x031F` (the ACIA, placed
   earlier, already owns $031C-$031F if present). The wrapper keeps `fdc_trace`
   and the synchronisation of the overlay flags (`basic_rom_disabled`/`overlay_active`).
4. ✅ **LOCI** (MIA $03A0-$03BF + TAP $0315-$0317 + DSK $0310-$0314/$0318-$0319,
   3 **disjoint** sub-ranges): a single `io_device_t` **at the head of the table** that
   dispatches internally. Its `claims` encodes the priority (TAP always overlaps the
   Microdisc; DSK only when `!has_microdisc`). The VIA ORB $0300 snoop
   (`loci_tap_motor`, cassette motor line) is **not** a claim → it stays in
   the VIA path.
5. ✅ **ULA-NG**: migrated thanks to the contract extension (`write` returning
   "consumed" + separate `claims_write`). Read: `claims` = unlocked &&
   in window. Write: `claims_write` = in window (always); `ula_ng_dev_write`
   returns the verdict of `ula_ng_write` (false when locked → fall back to VIA) and
   synchronises the raster IRQ when the write is consumed. Placed **last**
   in the table (fallback before the VIA). Non-regression: unlock+palette boots
   byte-identical to pre-migration, `test-ula-ng` 60/60, visible guard 2/2.

Today, `io_read/write_callback` = **the bus loop + the VIA fallback**. All
range-based peripherals (LOCI and ULA-NG included) are on `io_device_t`.

## 6. Next steps (beyond I/O dispatch)

The same principle extends to what makes main.c monolithic:

- **savestate**: ✅ *hook in place*. The contract carries `save_tag` + `save(emu,fp)`
  + `load(emu,fp,size)`. `savestate.c` receives the table via
  `savestate_set_io_devices()` (avoiding coupling), writes one section per device that
  provides a hook, and on load routes unknown tags to the `load` of the
  matching device. `save` may return **false → no section** (default
  state → byte-identical `.ost`, zero regression). **First device migrated:
  the ULA-NG** ("UNG" section) — fills a real gap (its state was not
  persisted). Serialised as a **blob** (pointer-free POD) with a **size
  guard** on load ⇒ *same-build* savestate (the quicksave/load case; a
  `.ost` from another build/arch is ignored, never corrupted). Still to migrate to
  this model: DTL2000, Mageco (small register sets); **LOCI has a real
  caveat** — its OS file handles cannot be serialised as they are.
  Eventually the hard-coded sections can be removed (the OCB/OGP pain).
  **DTL2000 and Mageco migrated (Epic 7/US4)**: "DTL"/"MAG" sections, emulated
  state as a blob + **host pointers preserved** on load (non-serialisable
  backend/trace/callbacks); live transport not restored (same-build). Remaining:
  **LOCI**: real caveat (OS mounts/descriptors).
- **tick** (Epic 7/US5, then sprint D 2.4.0): ✅ *in the contract*. Each
  device provides `tick` and `present_off`; `io_bus_tick()` walks
  `io_bus_tick_order[]`, an **explicit order, distinct from the dispatch order**
  and identical to the historical order (microdisc → jasmin → loci → acia → dtl →
  mageco → sp0256 → mea8000) → identical behaviour by construction
  (`tools/cli_golden.sh`: 0 differences). Cost measured, since the function runs on
  **every cycle**: naive loop +22 % per frame, unrolled +7 %, unrolled with
  `__builtin_expect(présent, 0)` → within noise (the compiler folds it into
  flag tests and direct calls, the same layout as the old code).
- **savestate, sprint D (2.4.0)**: sections **"JAS"** (Jasmin: FDC state,
  latches, bad-sector maps; the images go through "DSK",
  now also written for the Jasmin), **"SPO"** (SP0256: sequencer,
  LPC-12 filter, pending samples) and **"MEA"** (MEA8000: sequencer,
  4 formants, audio ring). Serialisation **field by field, little-endian,
  versioned** (`include/utils/binio.h`) — no blob: no ROM, no recomputable
  tables (~92 KB for the MEA8000), no host pointers in the file;
  a section with an unexpected version or size is ignored. The WD1793 state is
  factored out (`fdc_state_save/load`, format **v2**: + DRQ age and format-track
  parser, a v1 `.ost` still loads) and `fdc_state_resume()` recomputes the
  pointer to the current sector: resuming **in the middle of a disk transfer** read
  "Record Not Found" (an old defect, Microdisc included), now fixed and covered by
  `test-savestate-determinism` (Jasmin saved mid-way through the TDOS boot).
- **Still outside savestate**: **LOCI** (OS file handles, mounts) and
  the ACIA, whose "SER" section is still hard-coded in `savestate.c`
  (registers only, the host transport is not restored).
- **init / reset / cleanup**: hooks *not added to the contract*. Honest reason:
  reset is **not uniform** (the F5 warm reset only resets CPU + LOCI, the
  latter "keeping its mounts") → a generic reset loop would change
  behaviour. Deciding which peripherals see the /RESET line is a matter of
  hardware fidelity (backed by schematics), not of a refactor.

## 6 bis. Definition of "done" for a new bus peripheral

1. an entry in `io_bus[]` (and in the `DEV_*` `enum`) with `claims`/`read`/`write`,
   `present_off` and, if it advances in time, a `tick` placed at its position in
   `io_bus_tick_order[]`;
2. an `.ost` section (`save_tag`/`save`/`load`) serialising **the emulated state
   only**, versioned, which ignores an unknown section;
3. a dedicated unit test, including an "every field is restored" test (source
   state filled with distinctive values → reloaded → saved again → same bytes);
4. a scenario in `tests/integration/test_savestate_determinism.py` if the
   peripheral affects 6502 execution;
5. a case in `tests/cli_golden/cases.txt` for its options.

## 7. Operational constraint

The `oric1-emu` executable is used by other programs: **never
`make clean`** during this work (it deletes the binary); **incremental** builds
only (the binary is only replaced on a successful link); each step
leaves a working, **behaviour-identical** `oric1-emu`.

## 8. References
- `include/io/io_device.h`, `src/main.c` (`io_bus[]`, `io_bus_find`).
- Original symptom: `docs/ocula/CODE-MAP.md` (OCULA removal, same problem).
