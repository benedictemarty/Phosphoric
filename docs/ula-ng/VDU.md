# ULA-NG — Built-in VDU (v0.1)

> **Status: v0.1 + v0.2 (graphics) + v0.3 (upload) IMPLEMENTED AND VALIDATED.**
> VDU-style command port exposed by the ULA-NG (`NG_VDU` $0357), whose
> interpreter **and VRAM** live **inside the ULA-NG** — not in the 6502's
> 64 KB. Interpreter in `src/io/ula_ng.c` (stand-in for the FPGA soft-core firmware);
> tests `test-ula-ng`; demos `ng_vdu.s` (mosaic), `ng_vdu_gfx.s` (line
> drawing), `ng_vdu_spr.s` (sprite defined through the stream) — driven **only by the
> stream**, with no 6502 driver. A transparent OSWRCH hook was investigated and ruled out
> (§4: the Oric has no per-character output vector; "1.2" ROM avenue in the
> `feature/ula-ng-rom12-vdu` branch). Remaining: font/bitmap upload, extended PLOT.

## 1. Goal

Drive the ULA-NG with a **stream of command bytes** (as with `PRINT CHR$(…)`)
instead of raw register writes, and host the **interpreter + buffers**
in the ULA-NG's memory. The 6502 carries **no driver code**: it *streams*
bytes, the ULA-NG interprets them.

## 2. State of the art (design reference, ~2026)

This "display coprocessor driven by a VDU stream, with its own memory" model is
the dominant trend in modern 8-bit machines and FPGA re-implementations:

- **Agon Light — VDP**: the CPU (eZ80) sends a **VDU** stream (BBC BASIC lineage)
  to an ESP32 running video firmware with **its own memory**; modes,
  palette, sprites, bitmaps, audio. Key notion adopted here: **"buffered
  commands"** (uploading data into the VDP's memory, reusable by
  ID). → *the direct reference for this document*.
- **VERA (Commander X16)**: modern FPGA video chip, **registers +
  auto-increment data port** interface, sprites, layers, palette. → validates the
  data-port idiom the ULA-NG already uses (streamed attributes/sprites).
- **OCULA-GPU (this project)**: command window `$03E8-$03EF` (INFO, FILL,
  COPY, SCROLL, WAIT_VBL) interpreted **inside the ULA**. → direct in-house precedent
  for the "command port + interpreter" pattern.
- **Copper / display list** (Amiga heritage): already present (ULA-NG §5.4).

**Resulting architecture choice** (FPGA target Tang Primer 20K / GW2A-18):
*hard/soft split*. The **real-time datapath** (scanline fetch, palette, sprites,
copper) stays in **hard RTL**; the **VDU interpreter** runs as **firmware on a
RISC-V soft-core** (the GW2A can host one) or on the companion MCU — as
OCULA does with its RP2350. A VDU interpreter is not hard-wired as a pure FSM.

> Knowledge frozen at early 2026 — very recent releases may be missing.

## 3. The `NG_VDU` port

| Addr | Name | R/W | Role |
|---|---|---|---|
| `$0357` | `NG_VDU` | W | Write a byte to the VDU command stream. |

Requires the ULA-NG to be **unlocked** (like the whole `$0340-$035F` window).
From BASIC: `POKE#357,byte`; in machine code: `STA $0357`; the stream is
"streamed" byte by byte, exactly like an `OSWRCH`/VDU.

## 4. v0.1 command set (minimal, to be validated)

Vocabulary aligned with the BBC/Agon VDU where it makes sense, translated into ULA-NG
actions. Each command = a **code** + N **parameter** bytes.

| Code | Params | ULA-NG action | BBC/Agon analogue |
|---|---|---|---|
| `20` | — | **Reset**: `NG_MODE=0` → back to normal rendering. | `VDU 20` (restore) |
| `22 n` | 1 | **MODE**: `n`=0 std, 1 chunky (320), 2 text 80col → `NG_MODE`. | `VDU 22,n` (MODE) |
| `19 l r g b` | 4 | **Palette**: LUT[`l`] = RGB444 (`r,g,b` on 4 bits). | `VDU 19` (palette) |
| `18 a` | 1 | **Per-cell background colour**: enables the // attributes and fills the plane with `a`=`(paper<<3)\|ink`. | `VDU 18` (GCOL) |
| `31 col row a` | 3 | **Colour a cell** (`col` 0-39, `row` 0-24) with attribute `a` — without colour clash. | `VDU 31` (TAB) |

### v0.2 graphics (chunky VRAM carried by the ULA-NG)

| Code | Params | ULA-NG action | BBC/Agon analogue |
|---|---|---|---|
| `16` | — | **CLG**: enables chunky mode + the **ULA-NG VRAM**, clears it. | `VDU 16` (CLG) |
| `17 c` | 1 | Current **drawing colour** (`c` 0-15, LUT index). | `VDU 18`/GCOL |
| `25 x y` | 2 | **PLOT** a point (`x` 0-159, `y` 0-223) — *simplified*: single point, 8-bit coords (the full BBC `VDU 25` handles modes/lines/16-bit coords). | `VDU 25` (PLOT) |
| `26 x0 y0 x1 y1` | 4 | **DRAW** a line (Bresenham) into the VRAM. | `VDU 25` DRAW |

Unknown codes: ignored (0 parameters) — like a VDU that swallows the unknown.

### v0.3 upload (sprites via the stream) — see §5

`23 id` (begin sprite pattern upload) + `24 id x y f` (position + enable).

**Planned next** (outside the current scope):
- chunky fonts / bitmaps through the same upload protocol (§5);
- windows (`28`) → `NG_SCRSTART`/scroll; extended PLOT (triangles, 16-bit coords).

### Input ergonomics: conclusion on the OSWRCH hook (investigated)

Intended goal: `PRINT CHR$(…)` feeds `NG_VDU` **without POKE**, BBC-style.
Investigation carried out (disassembly of the Atmos ROM `basic11b.rom` + tests):

- **No proper per-character OSWRCH vector on the Oric.** The BASIC character output
  (`$CCD9`) does contain a hook `BIT $02F1 / JSR ($023E)`, but `$02F1`
  bit7 (redirection) is **continuously reset to 0 by the 50 Hz IRQ**; neither `PRINT`
  (forced flag) nor `LPRINT` called a wedge installed at `$023E` (counter
  stayed at 0 — the emulator's printer captures at the Centronics hardware level,
  not through this vector). Avenue **abandoned** (no broken code shipped).
- **Speed problem**: redirecting through the printer port (Centronics, handshake)
  is ~1000× slower than a direct `STA $0357` → unusable for
  bulk data (256-byte sprites, 16,000-byte images). Only good for
  small commands.

**Conclusion**: the **fast and reliable path remains direct driving**
(`STA $0357` in ML, `POKE #357` in BASIC), already in place. The "BBC" comfort
(re-vectorable RAM output vector, full speed) requires **patching a
"BASIC 1.2" ROM** that *adds* this vector (the Oric does not have one). It is a real
but large avenue (6502 stitching into the ROM, copyrighted derivative work)
→ **explored outside `main`, in the `feature/ula-ng-rom12-vdu` branch**
(design: `docs/ula-ng/ROM12-VDU.md`).

## 5. Upload protocol ("buffered commands", v0.3 — sprites)

For large data (256-byte sprite patterns, later bitmaps/fonts),
we adopt the Agon model, modelled on the ULA-NG's **auto-increment streaming**.
**Implemented for sprites**:

| Code | Params | Action |
|---|---|---|
| `23 id` | 1 | **SELECT + BEGIN UPLOAD**: sprite `id` (0-15); the **next 256 bytes** are streamed into its pattern (1 byte/px, 0-7). |
| `24 id x y f` | 4 | **USE**: position (`x`,`y`) + `f` b0 = visible; also enables sprites globally. |

Sequence: `VDU 23,id` → 256 pattern bytes → `VDU 24,id,x,y,1`. The interpreter
keeps a `vdu_upload` counter: while it is non-zero, each received byte goes into
the pattern (not interpreted as a command). Extensible later to fonts/bitmaps
(same mechanism, different target).

## 6. Memory & VRAM

- v0.1 drives the **existing state** of the ULA-NG (mode, palette, attribute plane) —
  no new memory.
- **v0.2 (done)**: `ula_ng.vram` (160×224 4bpp = 17920 bytes, 2 px/byte) — the real
  "the VDU owns its pixels" step. When `vram_active` (set by `CLG`), the
  chunky mode reads **this VRAM** instead of the CPU RAM (`NG_SCRSTART`). `PLOT`/
  `DRAW` write into it (Bresenham). `reset`/`20` disables it. On the FPGA side:
  this buffer lives in the ULA-NG's DDR3/BSRAM.

## 7. Interpreter (FSM)

Minimal state, no allocation: `vdu_cmd`, `vdu_params[ULA_NG_VDU_MAXPARAMS]`
(= `vdu_params[4]`), `vdu_need`,
`vdu_got`. When `vdu_need==0` the received byte is a **code** (its number of
parameters is looked up); otherwise it is a **parameter**; when `got==need` we **execute** and then
go back to waiting for a code. Execution simply calls the **already existing**
register logic (`ula_ng_write` on `NG_MODE`, `NG_PAL_*`,
`NG_ATTR_*`) → minimal integration, zero duplication.

## 8. Emulator modelling

The interpreter in **C in `ula_ng.c`** faithfully represents the **soft-core
firmware** of the FPGA target. The emulated 6502 runs no driver: it writes
bytes to `$0357`. It is the same abstraction as the one used to model
the OCULA-GPU.

## 9. Scope & risks (honesty)

- Adopting the full VDU model (Agon VDP style) = **a real subsystem**:
  we **start minimal** (v0.1 above) and only extend when needed.
- The ULA-NG's additional memory **is not addressable by the CPU**
  (streaming only): consistent with a command port, but PLOT/VRAM (v0.2)
  will need a dedicated buffer.
- This port **expands the role** of the ULA-NG (from "ULA" to "ULA + VDU") —
  a deliberate architecture choice, in line with the OCULA-GPU.

## 10. Proposed implementation plan (v0.1)

1. `NG_VDU` register ($0357) + FSM in `ula_ng.c` (calls `ula_ng_write`).
2. Unit tests (MODE, palette, fill, cell, reset, unknown command).
3. Demo `ng_vdu.s`: unlocks then **streams a "VDU program"** (palette +
   per-cell colour mosaic) — no interpretation logic on the 6502 side.
4. Visible guard + doc `README-ULA-NG.md` §VDU + spec §5.9.

## 11. References

- Agon Light — VDP / VDU documentation (Console8/Quark firmware).
- Commander X16 — VERA programmer's reference.
- `docs/ocula_extensions.md` — OCULA-GPU (in-house precedent).
- BBC Micro — VDU drivers (MOS), RISC OS VDU.
- `docs/ula-ng/ULA-NG-SPEC.md` (§5), `README-ULA-NG.md`.
