# ULA-NG — Implementation specification

**Project:** add a "next-gen" ULA (ULA-NG) to the **Phosphoric** emulator (benedictemarty's Oric-1 / Atmos emulator).
**Final target:** this software implementation is the *reference* for a future Verilog implementation on FPGA (**Sipeed Tang Primer 20K**, Gowin **GW2A-18**). The behaviour defined here must be reproducible bit for bit in HDL. Every choice must therefore remain **synthesisable in spirit**: no floating point in the video path, no structure that cannot be wired.

> **Confirmed target (product photo + Sipeed wiki)**: **20K Core Board +
> 20K Dock ext-board** bundle, FPGA **GW2A-LV18PG256C8/I7** (chip marking checked) —
> 20,736 LUT4, 15,552 FF, **BSRAM 828 Kbit** (46 blocks), **DDR3 128 Mbit** + 32 Mbit NOR
> flash on the core board, 204-pin SODIMM form factor.
> **Target memory split** — to be respected from the software model onwards:
> - **DDR3 128 Mbit on the core board** (the GW2A has no in-package RAM): parallel attribute plane (§5.6), VRAM banks, sprite tables — *any* large memory, outside the 6502's 64 KB.
> - **Block RAM (BSRAM ~828 Kbit, tight budget)**: composition line buffers, palette LUT (16×12 b), charset, small sprite caches — *never* a full framebuffer.
> - **Video output: HDMI available through the dock ext-board** (also RGB565 FPC, Ethernet, USB-OTG/JTAG, GPIO). Pixel timing will target the chosen HDMI mode; no impact on the reference behaviour defined here.

---

## Post-audit revisions (frozen decisions)

These decisions follow from the `AUDIT.md` audit and **take precedence** over the original text below in case of discrepancy:

1. **Robust detection** (§3): no "≠ 0" test (the `$0340` window overlaps the VIA mirror → false positive). Exact handshake `NG_ID == version` **+** complement register `NG_IDCHK` (`$034F`) = `~NG_ID`.
2. **Raster lines** (§5.2): `ula_ng_scanline` driven over the **full frame line range 0-311** (decoupled from the visible rendering 0-223). The 8-bit `NG_RASTERLINE` covers 0-255 (visible + top of vblank); a 9th bit reserved in `NG_MODE` if needed in the future.
3. **Palette LUT** (§5.1): **16 entries × 12 bits** (RGB444), RGB444→888 expansion by nibble replication (`c8 = c4*0x11`). Covers standard (8) and chunky (16).
4. **Register semantics** (§9): every register with a visual effect applies on the **next line** (latched in hblank); **exception** `NG_STATUS` (IRQ acknowledge) = immediate. Detailed table in §9.
5. **Attribute plane / banks / sprites** (§5.6, §5.7): **additional memory carried by `ula_ng`** (mirror of the external SDRAM), outside the 6502's 64 KB.

---

## 0. First of all — code audit (mandatory first task)

This spec contains `[TO BE CONFIRMED]` markers. **Do not write functional code before filling these gaps.** First produce a short `AUDIT.md` report answering:

1. **Language and build.** Which language (C / C++ / Rust / other)? How is the emulator built and launched? Are there already tests?
2. **Video rendering granularity.** Is rendering:
   - (a) **frame-based**: the VRAM is read in one block at the end of the frame;
   - (b) **scanline-based**: a line is composed for each scanned line;
   - (c) **cycle-exact**: the ULA reads memory as the 6502's cycles go by.
   Locate the relevant function (name + file + lines).
3. **Intercepting memory accesses.** How are CPU reads/writes routed? Is there a dispatch on the address (switch, handler table) or a flat memory array? Where exactly is page 3 (`#0300`-`#03FF`) handled (the VIA, in particular)?
4. **Colour structure.** Where are the 8 Oric colours converted into output pixels (RGB)? Hard-coded palette?
5. **Clock / IRQ.** How does the emulated 6502 receive an IRQ? What is the time source (cycle counter, scanline counter) accessible from the video code?

The rest of the spec assumes rendering can be made **at least scanline-based** (b). If Phosphoric is frame-based (a), **the first implementation step is to convert the video pipeline to scanline** — report it in `AUDIT.md` as a prerequisite, because the raster functions (line IRQ, per-line palette) depend on it.

---

## 1. Guiding principles

1. **Compatibility first.** At reset, the ULA-NG is indistinguishable from an HCS10017: serial attributes, 8 colours, 50 Hz, quirks included. No existing program must see any difference.
2. **Explicit activation.** The extensions only become active after an **unlock sequence** (see §3). Goal: no software scanning page 3 should trigger an extension by accident.
3. **FPGA-mirror modularity.** All the new code lives in an isolated `ula_ng` module (dedicated file(s)), with boundaries that match what the boundaries of the Verilog module will be. Only three interfaces:
   - `ula_ng_write(addr, value)` / `ula_ng_read(addr)` — register access (page 3);
   - `ula_ng_scanline(line_number)` — called by the video loop on every line;
   - an **IRQ line** output to the 6502 core.
4. **Testability.** Every observable behaviour must be capturable as a trace (see §6) to serve as a golden reference for the Verilog test bench.

---

## 2. Register map

Proposed window: **`#0340`-`#035F`** in page 3 (free in the community address map: VIA `#0300`-`#030F`, Microdisc `#0310`-`#031F`, ACIA `#031C`, Jasmin `#03F4`+).
**`[TO BE CONFIRMED]`**: check that no expansion emulated by Phosphoric already occupies `#0340`-`#035F`. In case of conflict, shift the window and update this table.

| Address | Name | Access | Role |
|---|---|---|---|
| `#0340` | `NG_LOCK` / `NG_ID` | W / R | Write: unlock sequence. Read: **locked → VIA passthrough** (indistinguishable); unlocked → version byte (**`0x1E` = v1.0**). |
| `#034F` | `NG_IDCHK` | R | Complement of `NG_ID`: `~NG_ID` (`0xE1` unlocked, `0x00`→`0xFF` locked). Anti-false-positive handshake (see §3). |
| `#0341` | `NG_MODE` | R/W | Mode bits: b0 = extensions active, b1 = parallel attribute mode, b2-3 = video mode (00 = std, 01 = chunky 4bpp, 10 = 80-col text), b4-5 = VRAM bank, b6 = 50/60 Hz, b7 = reserved. |
| `#0342`-`#0343` | `NG_SCRSTART` | R/W | Screen start address (16 bits, LSB then MSB). |
| `#0344` | `NG_SCROLLX` | R/W | Fine X offset (0-5 pixels). |
| `#0345` | `NG_SCROLLY` | R/W | Fine Y offset (0-7 pixels). |
| `#0346` | `NG_RASTERLINE` | R/W | Line number that triggers the raster IRQ. |
| `#0347` | `NG_STATUS` | R/W | Read: b7 = raster IRQ pending. Write: **acknowledge** (clear b7) **+ b0 = raster IRQ enable** (persistent). |
| `#0348` | `NG_PAL_IDX` | R/W | Palette index to program (**0-15**, 16-entry LUT), optional auto-increment. |
| `#0349`-`#034A` | `NG_PAL_DATA` | R/W | 12-bit colour (4096 shades): `#0349` = `0000RRRR`, `#034A` = `GGGGBBBB`. |
| `#034B` | `NG_COP_CTRL` | W | Copper (§5.4): write = reset of the stream pointer (empties the list). |
| `#034C` | `NG_COP_DATA` | W | Copper: 3-byte-per-entry stream — `line`, `(index<<4)|R`, `(G<<4)|B` (64 entries max). |
| `#034D` | `NG_ATTR_FILL` | W | Parallel attributes (§5.6): fills the whole 8 KB plane with the written byte `(paper<<3)|ink` + resets the stream pointer. |
| `#034E` | `NG_ATTR_DATA` | W | Parallel attributes: writes one cell at the pointer `(paper<<3)|ink` then auto-increments (modulo 8192). |
| `#0350` | `NG_SPR_CTRL` | W | Sprites (§5.7): b0 = global enable. |
| `#0351` | `NG_SPR_SEL` | W | Sprite selected for programming (0-15) + reset of the pattern pointer. |
| `#0352` | `NG_SPR_X` | W | X position (0-255) of the selected sprite. |
| `#0353` | `NG_SPR_Y` | W | Y position (0-255). |
| `#0354` | `NG_SPR_ATTR` | W | b0 = sprite visible. |
| `#0355` | `NG_SPR_DATA` | W | Pattern stream: 1 byte/pixel (`0` = transparent, `1`-`7` = palette index), auto-increment (mod 256). |
| `#0356` | `NG_SPR_STATUS` | R | b7 = sprite-sprite collision since the last read (clear on read). |

Any unlisted address in the window: read `0xFF`, write ignored (but reserved for extension).

---

## 3. Unlock sequence (`NG_LOCK`)

Goal: a signature that is unlikely in normal operation.

- Write `0x4E` ('N') then `0x47` ('G') to `#0340` in succession, **with no other write to the `#0340`-`#035F` window in between**.
- On the right sequence: `NG_ID` (`$0340`) returns the version (`0x1E`), `NG_IDCHK` (`$034F`) returns its complement (`0xE1`), and `NG_MODE.b0` becomes writable. While locked, writing to `#0341`-`#035F` has **no effect**.
- A reset re-locks everything and sets all registers back to 0 (HCS10017 state).

> **Locked passthrough (implementation decision, step 1).** For **bit-for-bit**
> non-regression, in the locked state the ULA-NG **does not drive** the
> `$0340-$035F` window: reads **fall through to the VIA** and so do writes
> (the module only watches `$0340` for the sequence). It
> only "owns" the window after unlocking. On the FPGA side = tristate
> (`drive_bus = unlocked && addr_in_window`). Consequence: the detection below
> works identically (before unlocking, `LDA $0340` reads the VIA → `≠ 0x1E` →
> `no_ng`; afterwards, `0x1E`).

> **Robust detection (post-audit revision).** `$0340` overlaps the **VIA mirror**
> (the VIA answers as fallback over all of `$0300-$03FF`). On a machine **without**
> ULA-NG, `LDA $0340` reads the VIA's ORB (keyboard column latch) → non-zero value
> → a "≠ 0" test would give a **false positive**. It is therefore necessary to (a) compare with the
> **exact value** `0x1E`, **and** (b) check the consistency `NG_ID XOR NG_IDCHK
> == 0xFF`, which the VIA cannot produce on two addresses. The
> `'N'/'G'` writes hit the VIA's ORB on a bare machine (harmless: column latch).

Detection on the Oric software side (to be documented for coders):
```asm
    LDA #$4E : STA $0340        ; 'N'
    LDA #$47 : STA $0340        ; 'G'  (no other write to $0340-$035F in between)
    LDA $0340 : CMP #$1E : BNE no_ng     ; exact version?
    LDA $034F : EOR $0340 : CMP #$FF : BNE no_ng   ; NG_ID XOR NG_IDCHK = $FF?
    ; ULA-NG present and unlocked
no_ng:
```

---

## 4. Implementation order

Implement and **validate one feature before moving on to the next** (each step has a visible test):

1. `NG_LOCK` / `NG_ID` + page 3 interception plumbing.
2. **Palette indirection** (§5.1) — the 8 colours go through a LUT.
3. **Raster IRQ** (§5.2) — the most requested feature.
4. **Start address** (§5.3) — double buffering / coarse scroll.
5. Per-scanline palette (§5.4).
6. Fine X/Y scroll (§5.5).
7. Parallel attributes (§5.6).
8. Sprites (§5.7).
9. Chunky / 80-column modes (§5.8).

---

## 5. Feature specification

### 5.1 Palette indirection
Logical colours become indices into a **LUT of 16 entries × 12 bits** (RGB444) — 8 are enough for the standard mode, 16 are used by chunky 4bpp (§5.8). At reset, the first 8 entries hold the 8 original Oric colours (identity mapping → compatibility). The rendering path replaces every direct "colour → RGB" access with "colour → LUT → RGB". **RGB444 → RGB888 expansion by nibble replication** (`c8 = c4 * 0x11`), no interpolation (synthesisable).

**Conversion point (resolved by the audit):** `get_rgb()` in `src/video/video.c` (≈ l.109-113) already reads `vid->pal_rgb[c][3]` — **it is the only place to point at the NG LUT**. `pal_rgb` IS the LUT; it only needs to be fed from `NG_PAL_*` (16×12 bits) instead of the fixed table when the extensions are active.

### 5.2 Raster IRQ
`NG_RASTERLINE` sets a line; when `ula_ng_scanline(n)` receives `n == NG_RASTERLINE`, raise `NG_STATUS.b7` and assert the IRQ line to the 6502. The IRQ stays active until an acknowledge write to `NG_STATUS`.

**Numbering (post-audit revision):** `ula_ng_scanline` is driven over the **full frame line range 0-311** (`frame_cycles / 64`), decoupled from the visible rendering 0-223, so as to allow IRQs in the vblank area too. The 8-bit `NG_RASTERLINE` covers 0-255 (all of the visible area + top of vblank); a 9th bit remains reserved in `NG_MODE` for 256-311 if needed.

**IRQ hook-up (resolved by the audit):** 6502 IRQ = **level-triggered, wired-OR** bitfield (`cpu.irq`, `include/cpu/cpu6502.h`). Add `IRQF_ULANG = 0x20` (next free bit) and assert/acknowledge via `cpu_irq_set/clear(&emu->cpu, IRQF_ULANG)`. Each source keeps its own bit → **we combine, we do not overwrite** (requirement met by construction).

**Enable (implementation decision, step 3):** an enable bit avoids spurious IRQs (at reset `NG_RASTERLINE`=0). **`NG_STATUS.b0` (write) = enable** (persistent); any write to `NG_STATUS` also acknowledges (clear b7). The IRQ is only raised if **unlocked && NG_MODE.b0 && enable**. In bare BASIC (IRQ vector in ROM), arming the IRQ without an ISR freezes the machine (unacknowledged IRQ loop) — this is the expected behaviour; a clean raster bar requires an ISR (vector redirection under Sedoric/overlay).

**Implemented (step 3, validated)**: `ula_ng_scanline(line)` driven by the video loop over frame lines 0-311, `NG_STATUS.b7`/acknowledge, `IRQF_ULANG`. Unit tests + observable end to end (BASIC frozen when armed without an ISR).

### 5.3 Start address
`NG_SCRSTART` replaces the fixed base address of the video fetch (`#A000` in HIRES, `#BB80` in TEXT). Default = original value. Allows double buffering (switching between two buffers) and coarse vertical scrolling (increment per line).

**Implemented (step 4, validated)**: `NG_SCRSTART` (`$0342` LSB / `$0343` MSB, 16 bits) is applied to the **fetch of the main area** (lines 0-199: `$A000`+y·40 in HIRES, `$BB80`+row·40 in TEXT) when active (`unlocked && NG_MODE.b0`). **`$0000` = default base of the mode** (compatibility, no blank screen at reset; `$0000` is never a valid screen). The 3 status rows (lines 200-223) stay fixed at `$BB80`. Wired into `video_t` through a pointer (`ng_scrstart`). Visible test: `NG_SCRSTART = $BB80+40` → the screen moves up by one row (pixel-exact comparison of the framebuffer).

### 5.4 Per-scanline palette
A small list of palette instructions (mini-copper) in RAM, applied during hblank by `ula_ng_scanline`. Format to be kept simple: a sorted table of (line, index, colour). Keep it trivially synthesisable (a FIFO re-read per line).

**Implemented (step 5, validated)**: copper list of 64 entries max, each entry = `(line, LUT index, RGB444 colour)`. Programming by stream: a write to `NG_COP_CTRL` (`$034B`) resets the list; `NG_COP_DATA` (`$034C`) receives **3 bytes per entry**: `[0]=line`, `[1]=(index<<4)|R`, `[2]=(G<<4)|B` (committed on every 3rd byte). `ula_ng_scanline(line)` applies to the LUT (`u->pal[index]`) every entry whose `line == line` (active if `unlocked && NG_MODE.b0`). The video hook re-reads the LUT → effect on the **next line** (consistent with §9). Provide a "line 0" entry to set the base on every frame. Visible test: colour 7 red (line 0) then blue (line 30) → vertical colour bands (framebuffer).

### 5.5 Fine X/Y scroll
`NG_SCROLLX` (0-5) and `NG_SCROLLY` (0-7) shift the fetch pipeline at pixel level. In practice: an offset applied when composing the line.

**Implemented (step 6, validated)**: `NG_SCROLLX` (`$0344`, clamped 0-5 = cell width 6 px), `NG_SCROLLY` (`$0345`, masked 0-7 = height 8 px). Applied when composing the main area (0-199) when active (`unlocked && NG_MODE.b0`): **Y** shifts the source line (`src_y = y + scrolly`, content moves up); **X** shifts the display (`px = col·6 - scrollx`, `set_pixel` clips off-screen, one extra cell fetched to fill the right edge). Combined with coarse scroll (§5.3) → smooth scrolling. Inactive / 0 → rendering bit-for-bit unchanged. Visible test: `NG_SCROLLY=4` / `NG_SCROLLX=3` → pixel-exact shift checked on the framebuffer.

### 5.6 Parallel attributes
A second 8 KB memory plane (bank selected by `NG_MODE.b4-5`) provides ink+paper per 8×1 cell **without consuming pixel bytes in the stream** — removes serial colour clash. Only active if `NG_MODE.b1`.

**Memory location (post-audit revision):** this plane (as well as the VRAM banks and the sprite tables §5.7) lives in **additional memory carried by the `ula_ng` module** — a mirror of the **external DDR3** (128 Mbit) of the Tang Primer 20K core board — **outside the 64 KB** addressable by the 6502. Accessed through a window of NG registers (address + auto-increment), never mapped into the CPU space. Emulator side: a buffer in `ula_ng_t`; FPGA side: the DDR3.

**Implemented (step 7, validated)**: plane `ula_ng.attr[8192]` (one byte per cell, `(paper<<3)|ink`, 3 bits each), indexed `y·40 + col` (0-7999 out of 8192). Programming by stream: `NG_ATTR_FILL` (`$034D`) fills the whole plane with a uniform byte + resets the pointer to 0 (one write = one full background, handy for demos); `NG_ATTR_DATA` (`$034E`) writes the current cell and auto-increments (modulo 8192). Only active if `unlocked && NG_MODE.b1` (`ula_ng.attr_active`). When active, composing the main area (0-199) takes ink+paper **from the plane** instead of the serial attributes (bytes `#00-#1F` are no longer interpreted as attributes → **no more colour clash**). Inactive → rendering bit-for-bit unchanged. Visible test: `NG_MODE.b1` + `NG_ATTR_FILL=$21` (blue paper 4, red ink 1) → main area entirely blue+red (no other colour), checked on the framebuffer.

### 5.7 Hardware sprites
Up to 16 sprites of 16×16, 3 bpp (palette index), with priority and collision detection. Sprite table in RAM pointed to by a register in `#0350`-`#035F`. Composition in the output pipeline (after the background, before RGB conversion) — invisible to the VRAM. Collision bit readable in the `#0350`-`#035F` area.

**Implemented (step 8, validated)**: 16 sprites of 16×16 in the additional memory `ula_ng.sprites[16]` (pattern 1 byte/px, `0` = transparent, `1`-`7` = palette LUT index; outside the 6502's 64 KB, DDR3 mirror). Programming by stream through the window: `NG_SPR_CTRL` ($0350, b0 = global enable), `NG_SPR_SEL` ($0351, current sprite + pattern pointer reset), `NG_SPR_X`/`NG_SPR_Y` ($0352/$0353, screen position), `NG_SPR_ATTR` ($0354, b0 = visible), `NG_SPR_DATA` ($0355, auto-incremented pattern stream). Composition in `ula_ng_composite_scanline()`, called by the video hook after rendering the background of each scanline (colour = NG palette LUT). **Priority by index**: sprites are composed 15→0, so sprite 0 ends up on top. Sprite-sprite **collision detection** (overlap of opaque pixels on a scanline) → `NG_SPR_STATUS` ($0356, b7, clear on read). Gate `unlocked && NG_SPR_CTRL.b0` (`spr_active`); inactive → rendering unchanged. Visible test: sprite 0 (16×16 filled with index 1 = red) at (100,100) → pixel-exact 256-px red block on the framebuffer. *Not implemented yet (future refinement)*: priority relative to the background (the "behind background" bit requires a background index buffer; sprites are always in the foreground for now), compact 3 bpp on the FPGA side (emulator = 1 byte/px).

### 5.8 Chunky / 80-column modes
- **Chunky 4bpp**: 160×200, 16 colours out of 4096 (via an extended LUT if needed).
- **80-column text**: font redefinable in RAM. Useful for Sedoric.
Selected by `NG_MODE.b2-3`.

**Implemented (step 9, validated)**: selected by `NG_MODE.b2-3` (`01` = chunky, `10` = 80 col; caches `ula_ng.chunky_active` / `text80_active` = `active && vidmode`, hence gate `unlocked && NG_MODE.b0`). The modes are **latched at the start of the frame** (like the OCULA modes): the framebuffer width stays stable for a whole frame. Data read from `NG_SCRSTART` (§5.3, default `$A000`).
- **Chunky 4bpp**: 160 px/row, each 4 bits = index into the **16-entry NG palette LUT** (RGB888, §5.1). 80 bytes/row (high nibble = left pixel). Each chunky pixel takes 2 framebuffer px → **320 px** wide (`VIDEO_WIDE_W`). **Full screen**: the mode covers the whole visible height (rows 0-223 read from the buffer), **with no 40-column text footer** — unlike the modes derived from HIRES, a modern bitmap mode has no status band (which avoided a black hole 240-319 at the bottom). Visible test: `NG_MODE=$05` + palette index 0 = magenta → **320×224** framebuffer, dominant magenta (zeroed areas) + other colours from the data = 16 colours.
- **80-column text**: 80 characters × 6 px = **480 px** (`VIDEO_MAX_W`). Redefinable RAM charset (`$B400`/`$B800` via `get_charset_byte`, the native Oric mechanism — no invented font); serial attributes and colours (NG LUT 0-7) as in standard text. Visible test: `NG_MODE=$09` → **480×224** framebuffer, characters rendered from `$A000`.

Parallel composition with sprites (§5.7): `ula_ng_composite_scanline` is called after rendering the background in both modes. Inactive → rendering bit-for-bit unchanged (width 240). *Not implemented (future refinement)*: the 160×200 variant "stretched" to 240 px (choice: 2× to 320 px, exact integer mapping); 3 bpp/4096 shades assumes an extended LUT on the FPGA side (emulator = 4 bits/px + 16-entry LUT).

---

## 6. Traces ("golden reference")

Add a debug mode (CLI flag, e.g. `--ula-trace=fichier`) which, **per frame**, serialises:
- the state of registers `#0340`-`#035F`;
- the RGB output **line by line** (or a hash per line to keep it light);
- the cycles/lines at which the raster IRQ is asserted.

Simple, deterministic, diffable text format. These traces will serve as the reference: the future Verilog test bench will replay the same programs and compare. Any divergence pinpoints the bug to the exact line.

---

## 7. Compatibility — non-regression

Before considering a step finished, check that **locked mode (extensions off) is bit-for-bit identical** to the behaviour before the change. Ideally:
- capture reference traces of a few programs (demos using attributes, double height, lores) **before** any change;
- replay after each step; classic mode must produce identical traces.
Suggested test programs: the Atmos demonstration tape (lots of attributes, double size, lores 1), and a game using double-height characters.

---

## 8. Expected deliverables

1. `AUDIT.md` — answers to §0.
2. `ula_ng` module (separate file(s)) with the 3 interfaces of §1.3.
3. Minimal changes to the video pipeline and the page 3 dispatch, clearly isolated and commented.
4. The trace mode of §6.
5. User-facing `README-ULA-NG.md`: unlock sequence, register map, 3 commented assembly examples (enable the mode, load a palette, set up a raster IRQ).
6. Test games/snippets for each feature.

---

## 9. "FPGA thinking" constraints (to respect from the software stage)

- No floating point in the video path; colours and offsets as integers.
- Every data structure of the `ula_ng` module must have an obvious hardware equivalent (register, small RAM, LUT, FIFO). Avoid dynamic allocation in the real-time path.
- Control signals such as `#0340`-`#035F` are sampled deterministically (once per access / per line), never "whenever it happens".
- Document, for each register, whether it is read combinationally (within the current line) or synchronously (effect on the next line/frame) — this detail determines FPGA fidelity.

### Per-register semantics (post-audit decision)

Rule: **every register with a visual effect applies on the NEXT line** (latched in hblank), consistent with scanline rendering, which freezes the VRAM at the CPU instant. Immediate (combinational) **exceptions**: IRQ acknowledge and unlocking.

| Register | Effect | Sampling |
|---|---|---|
| `NG_LOCK`/`NG_ID`/`NG_IDCHK` | lock / identity | **combinational** (immediate) |
| `NG_MODE` | video mode, banks | **synchronous** — next line (b0 activation: immediate) |
| `NG_SCRSTART` | fetch base | **synchronous** — next frame (double buffering) |
| `NG_SCROLLX/Y` | pixel offset | **synchronous** — next line |
| `NG_RASTERLINE` | IRQ line | **synchronous** — compared by `ula_ng_scanline` |
| `NG_STATUS` (write) | IRQ acknowledge | **combinational** (immediate) |
| `NG_PAL_IDX`/`NG_PAL_DATA` | palette LUT | **synchronous** — next line (allows raster splits) |
| `NG_SPRITE_*` | sprite control | **synchronous** — next line; `collision` on read = **combinational** |
