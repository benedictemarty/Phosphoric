# AUDIT.md — ULA-NG: Phosphoric architecture audit (prerequisite §0)

> Answers to the 5 questions of §0 of `ULA-NG-SPEC.md` + resolution of the
> `[TO BE CONFIRMED]` markers. All `file:line` citations were checked against the
> code at the time of the audit (v1.65.0-alpha). No functional code was written.

## Feasibility verdict (summary)

| Spec point | Result | Consequence |
|---|---|---|
| Video granularity | **scanline-based (b)** — already in place | **No pipeline conversion**: the §0 prerequisite is met. The raster/per-line palette features can be plugged in directly. |
| Register window `$0340-$035F` | **FREE** (exhaustive grep: 0 occurrences) | Adopted as is, no shift. |
| Palette indirection point (§5.1) | Single `get_rgb()` → `vid->pal_rgb[8][3]` | Only one place to route through the NG LUT. |
| IRQ line (§5.2) | Level-triggered, wired-OR bitfield | Add an `IRQF_ULANG = 0x20` source (next free bit), combined, not overwritten. |
| Time source | `emu->frame_cycles`; line = `frame_cycles / 64` | `ula_ng_scanline(line)` can be called from the existing render loop. |
| Video precedent | OCULA (raster sync `$03EC-$03ED`, redefinable palette) | Proven integration model to follow. |

---

## 0.1 — Language, build, tests

- **Language**: C11 (`-std=c11`), `Makefile:7` (`-Wall -Wextra -Wpedantic`).
- **Build**: `make` (headless) / `make SDL2=1` (display); `CMakeLists.txt` as a secondary option (removed in 2.1.3: the `Makefile` is now the only build).
- **Tests**: home-grown suite (macros `TEST()/RUN()/ASSERT_*`, no dependency). Video:
  `make test-video` (`tests/unit/test_video.c`). `make tests` = full suite (987 tests).

## 0.2 — Video rendering granularity → **(b) scanline-based**

Rendering is **scanline-accurate**: a line is composed for each scanned
line, from the VRAM frozen at the current CPU instant.

- Per-frame loop: `src/main.c` while `frame_cycles < CYCLES_PER_FRAME`
  (~`main.c:2272`). After each `cpu_step`, `emu->frame_cycles` is updated.
- Scanline catch-up: `src/main.c:2389-2392`
  ```c
  int target_scanline = frame_cycles / PAL_CYCLES_PER_LINE;   /* 64 cyc/line */
  while (rendered_scanlines < target_scanline && rendered_scanlines < 224) {
      video_render_scanline(&emu->video, emu->memory.ram, rendered_scanlines);
      rendered_scanlines++;
  }
  ```
  (same for the end-of-frame catch-up `main.c:2397-2399`, and a stall path `2325-2327`).
- Rendering function: `void video_render_scanline(video_t* vid, const uint8_t* memory, int y)`
  — `src/video/video.c:437` (prototype `include/video/video.h:176`). Reads the VRAM
  (`base = hires ? 0xA000 + y*40 : 0xBB80 + row*40 ; byte = memory[base+col]`),
  composes 6 pixels/byte, writes RGB888 via `set_pixel()`.
- **Per-line latches already present**: `palette_latch()` (video.c:480) and
  `border_latch()` (video.c:484) are re-read **on every scanline** → natural
  hook for the per-line palette (§5.4) and the raster IRQ (§5.2).

> **Spec consequence**: the frame→scanline conversion conditionally required
> by §0 **is not needed**. `ula_ng_scanline(line_number)` slots in
> right before/at the `video_render_scanline` call in the `main.c` loop.

## 0.3 — Intercepting memory accesses (page 3)

- The whole of page 3 `$0300-$03FF` is routed to callbacks:
  `src/memory/memory.c:209-216` (read) / `275-281` (write) →
  `mem->io_read/io_write`.
- Callbacks registered in `src/main.c:1577`
  (`memory_set_io_callbacks(..., io_read_callback, io_write_callback, emu)`).
- **Read** dispatch: `io_read_callback()` `src/main.c:919-996`; **write**:
  `io_write_callback()` `src/main.c:1112-1227`. "First match by
  address" cascade, **VIA `$0300-$030F` as the final fallback** (mirrored over all of page 3).
  Order: LOCI ($03A0-$03BF, $0315-$0319), OCULA ($03E0-$03EF), ACIA
  ($031C-$031F, base `--acia-addr`), Mageco, Microdisc ($0310-$031F), DTL2000
  ($03F8-$03FD), then VIA.
- **ULA-NG insertion point**: an `if (ula_ng_addr_in_window(address))
  return ula_ng_read(...)` test to be added **before the VIA fallback** in both
  dispatchers (symmetrical to OCULA).

### 0.3b — Address conflict `$0340-$035F` → **NONE**

`grep -niE '0x034[0-9a-f]|0x035[0-9a-f]'` over `src/` + `include/`: **0 occurrences**.
The window falls between Microdisc/LOCI ($0310-$031F) and OCULA ($03E0-$03EF), and
is not touched by any expansion (including configurable bases). **Window
`$0340-$035F` adopted without shift.**

## 0.4 — Colour structure

- Hard-coded 8-colour RGB888 palette: `src/video/video.c:17-20`
  ```c
  static const uint8_t palette[8][3] = { {0,0,0},{FF,0,0},{0,FF,0},{FF,FF,0},
                                         {0,0,FF},{FF,0,FF},{0,FF,FF},{FF,FF,FF} };
  ```
- **Single conversion point**: `get_rgb()` `src/video/video.c:109-113` →
  `c = oric_color & 7 ; *r/g/b = vid->pal_rgb[c][...]`. The **active** palette is
  already a `uint8_t pal_rgb[8][3]` LUT (`include/video/video.h:149`), reset
  by `palette_reset()` (video.c:31-33) and possibly redefined by OCULA
  (`palette_latch()`, video.c:67-83, RGB332 → RGB888).

> **Spec consequence (§5.1)**: `pal_rgb[8][3]` IS already the requested LUT. The
> NG palette indirection = extend each entry to 12 bits (4096 shades) and
> feed `pal_rgb` (or an RGB444→888 variant) from the NG registers instead
> of the fixed table. Only one point (`get_rgb`) needs touching.

## 0.5 — Clock and IRQ

- **Time source accessible to the video code**: `emu->frame_cycles`
  (`include/emulator.h:215`, `int` 0-19967), updated per instruction
  (`main.c` ~2343). Line number = `frame_cycles / PAL_CYCLES_PER_LINE` (64).
  Constants `PAL_LINES_PER_FRAME=312`, `CYCLES_PER_FRAME=19968` (emulator.h:106-111).
- **6502 IRQ = level-triggered, wired-OR bitfield**: `cpu.irq` (uint8_t);
  sources `cpu_irq_source_t` `include/cpu/cpu6502.h:44-48`:
  `IRQF_VIA=0x01, IRQF_DISK=0x02, IRQF_SERIAL=0x04, IRQF_DTL2000=0x08,
  IRQF_MAGECO=0x10`. **Next free bit = `0x20`.**
- API: `cpu_irq_set(cpu, source)` / `cpu_irq_clear(cpu, source)`
  (cpu6502.h:161/172). The IRQ is taken if `cpu.irq != 0` and the I flag is clear.
  Each source maintains its bit independently → **we combine, we do not overwrite**
  (§5.2 requirement met by construction).
- Existing hook-up example (VIA): `irq_callback()` `main.c:1237-1244`
  (`state ? cpu_irq_set(IRQF_VIA) : cpu_irq_clear(IRQF_VIA)`). Microdisc/ACIA/
  DTL2000/Mageco follow the same pattern (`main.c:1269-1320`).

> **Spec consequence (§5.2)**: add `IRQF_ULANG = 0x20` to the enum, and
> assert/acknowledge via `cpu_irq_set/clear(&emu->cpu, IRQF_ULANG)` from the
> NG raster logic. Zero impact on the existing IRQs.

---

## Proposed integration plan (FPGA mirror, §1.3)

Isolated module `src/io/ula_ng.c` + `include/io/ula_ng.h`, 3 interfaces:

1. `uint8_t ula_ng_read(ula_ng_t*, uint16_t addr)` / `void ula_ng_write(ula_ng_t*, uint16_t addr, uint8_t val)`
   — plugged into `io_read_callback`/`io_write_callback` (main.c) **before the
   VIA fallback**, guarded by `ula_ng_addr_in_window(addr)` (`$0340-$035F`).
2. `void ula_ng_scanline(ula_ng_t*, int line)` — called in the `main.c` render
   loop at scanline catch-up time (next to `video_render_scanline`).
   Put there: raster IRQ test (`line == NG_RASTERLINE` → `NG_STATUS.b7` +
   `cpu_irq_set(IRQF_ULANG)`), per-line palette application, start-address latch.
3. IRQ output → `cpu_irq_set/clear(&emu->cpu, IRQF_ULANG)` (new bit `0x20`).

Locked state (reset): registers at 0, `ula_ng_read` inert, `get_rgb`
unchanged → **bit-for-bit identical to the current behaviour** (non-regression §7). Unlocking
(`$0340` ← 'N','G') is the only thing that enables `NG_MODE.b0`.

### Implementation order (each step tested, see §4 of the spec)
1. `NG_LOCK`/`NG_ID` + page 3 plumbing (module + dispatch + lock).
2. Palette indirection (§5.1) — hook `pal_rgb` to the 12-bit NG LUT.
3. Raster IRQ (§5.2) — `IRQF_ULANG`, `ula_ng_scanline`.
4. Start address (§5.3), 5. per-scanline palette (§5.4), 6. fine scroll (§5.5),
   7. parallel attributes (§5.6), 8. sprites (§5.7), 9. chunky/80col (§5.8).
5. Trace mode `--ula-trace=FILE` (§6) + `README-ULA-NG.md` (§8).

### FPGA target (frozen decision, product photo + Sipeed wiki)
**Sipeed Tang Primer 20K** — **Core Board + Dock ext-board** bundle, Gowin
**GW2A-LV18PG256C8/I7**: 20,736 LUT4, 15,552 FF, **BSRAM 828 Kbit** (46 blocks),
**DDR3 128 Mbit** + 32 Mbit NOR flash on the core board. Memory split:
- **DDR3 128 Mbit (core board)** — no in-package RAM on the GW2A:
  parallel attribute plane (§5.6), VRAM banks, sprite tables — outside the 6502's 64 KB.
- **BSRAM ~828 Kbit (tight budget)**: composition line buffers, palette LUT (16×12 b),
  charset, small sprite caches — never a full framebuffer.
- **HDMI video output available through the dock** (+ RGB565 FPC, Ethernet, USB-OTG/JTAG,
  GPIO) — output target of the future HDL; no impact on the reference behaviour.

### "FPGA thinking" points already favourable
- No floating point in the video path (integer RGB888, LUT palette). ✓
- `pal_rgb`, NG registers = small tables/registers → direct HDL equivalent. ✓
- Deterministic sampling: the latches are **already per scanline**
  (palette_latch/border_latch) → NG registers read "once per line". ✓

> To be documented per register (§9 requirement): combinational read (current line)
> vs synchronous (next line/frame). Recommendation: effects applied **on the
> next line** (like the current serial attributes, cf. video_render_scanline,
> which freezes the VRAM at the CPU instant) to stay faithful to the scan.
