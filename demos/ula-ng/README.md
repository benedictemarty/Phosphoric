# ULA-NG demos

Small demos showing off the capabilities of the **ULA-NG** — Phosphoric's
"next-generation" ULA, the software reference for a future Verilog port to the
Sipeed Tang Primer 20K (GOWIN GW2A). See the spec
`docs/ula-ng/ULA-NG-SPEC.md`.

The ULA-NG is **inert at reset** (indistinguishable from an original HCS10017). Each
demo **unlocks** it itself by writing the sequence `'N','G'` (`$4E`,`$47`)
to `$0340`, then programs the registers `$0340-$035F`. No particular emulator
option is required — a plain `-t <demo>.tap -f` is enough.

> **Display note**: on some GPU/driver configurations, the accelerated SDL
> renderer gives a black window. The examples below use
> `--render-software`.

## Interactive menu

```bash
make SDL2=1            # build the emulator if needed
demos/ula-ng/menu.sh
```

Options: `--scale N` (SDL scale, default 2), `--rom PATH` (default
`roms/basic11b.rom`), `--accel` (accelerated renderer instead of the software one).
`Ctrl+C` in a demo's window goes back to the menu; `q` quits.

## Running a demo manually

```bash
# Chunky 4bpp 320x200, 16 colours, animated palette (machine code)
./oric1-emu -r roms/basic11b.rom -t demos/ula-ng/ng_chunky.tap -f --render-software --scale 2

# 80-column text (wait ~30 s: filling the $A000 screen is done in BASIC)
./oric1-emu -r roms/basic11b.rom -t demos/ula-ng/ng_text80.tap -f --render-software --scale 2

# Parallel attributes: colour mosaic PER CELL (impossible without colour clash)
./oric1-emu -r roms/basic11b.rom -t demos/ula-ng/ng_attributes.tap -f --render-software --scale 2

# Copper / palette re-latched per scanline: rainbow raster bars
./oric1-emu -r roms/basic11b.rom -t demos/ula-ng/ng_copper.tap -f --render-software --scale 2

# 16x16 hardware sprite: bouncing diamond
./oric1-emu -r roms/basic11b.rom -t demos/ula-ng/ng_sprite.tap -f --render-software --scale 2
```

## The demos

| File | Feature (spec) | What you see | Source |
|---|---|---|---|
| `ng_chunky`     | Chunky 4bpp §5.8       | **Full-screen 320×224** 16-colour image (diagonal gradient), palette animated in waves | machine code |
| `ng_text80`     | 80-column text §5.8 | 80 characters per line (480 px), RAM charset `$B400`                   | BASIC |
| `ng_attributes` | Parallel attributes §5.6 | **Per-cell colour mosaic** (8-colour diagonal gradient) — a different background colour in **every** cell, impossible on the original ULA (colour clash) | machine code |
| `ng_copper`     | Copper / scanline §5.4 | Palette re-latched per line → rainbow raster bars                | BASIC |
| `ng_sprite`     | Hardware sprite §5.7   | 16×16 sprite (diamond) composited over the background, bouncing               | BASIC |
| `ng_vdu`        | **Built-in VDU** ([VDU.md](../../docs/ula-ng/VDU.md)) | Per-cell colour mosaic driven **entirely by a command stream** written to `NG_VDU` ($0357) — no 6502 driver | machine code |
| `ng_vdu_gfx`    | **Graphics VDU** (v0.2) | Sunburst drawn with VDU commands (`CLG`/`DRAW`) into the **chunky VRAM held by the ULA-NG** — the VDU owns its pixels | machine code |
| `ng_vdu_spr`    | **VDU upload** (v0.3) | 16×16 sprite (diamond) **defined by a 256-byte stream** (`VDU 23`) then positioned (`VDU 24`) — "buffered commands" protocol | machine code |

## Driving/injecting without BASIC

To test a feature without writing a program, the emulator accepts
`--ula-ng-poke "AAA=VV,..."`: a sequence of hexadecimal writes to the
registers `$0340-$035F` applied at startup. Examples (can be combined with
`--screenshot-at C:FILE`):

```bash
# Chunky: unlock + NG_MODE=$05 + palette index 0 = magenta
./oric1-emu -r roms/basic11b.rom -n --ula-ng-poke "340=4E,340=47,341=05,348=00,349=0F,34A=0F" \
    --screenshot-at 4000000:/tmp/chunky.ppm -c 4500000

# 80 columns: unlock + NG_MODE=$09
./oric1-emu -r roms/basic11b.rom -n --ula-ng-poke "340=4E,340=47,341=09" \
    --screenshot-at 4000000:/tmp/t80.ppm -c 4500000
```

## Rebuilding the .tap files

BASIC sources (`*.bas`) → `.tap` with `bas2tap`:

```bash
make tools
./bas2tap demos/ula-ng/ng_text80.bas -o demos/ula-ng/ng_text80.tap --auto-run
./bas2tap demos/ula-ng/ng_copper.bas -o demos/ula-ng/ng_copper.tap --auto-run
./bas2tap demos/ula-ng/ng_sprite.bas -o demos/ula-ng/ng_sprite.tap --auto-run
```

Assembly sources (`*.s`, xa65 syntax) → `.bin` → `.tap` (loaded/executed at
`$0500`; **beware**: `bin2tap --start` uses `strtol` with base 0, so
prefix hexadecimal values with `0x`; the `xa` assembler does not tolerate
UTF-8 accented characters in the source, hence the lack of comments in the `.s` files):

```bash
xa demos/ula-ng/ng_chunky.s -o /tmp/ng_chunky.bin
./bin2tap /tmp/ng_chunky.bin --start 0x0500 --exec 0x0500 -o demos/ula-ng/ng_chunky.tap --name NGCHUNKY

xa demos/ula-ng/ng_attributes.s -o /tmp/ng_attributes.bin
./bin2tap /tmp/ng_attributes.bin --start 0x0500 --exec 0x0500 -o demos/ula-ng/ng_attributes.tap --name NGATTR

xa demos/ula-ng/ng_vdu.s -o /tmp/ng_vdu.bin
./bin2tap /tmp/ng_vdu.bin --start 0x0500 --exec 0x0500 -o demos/ula-ng/ng_vdu.tap --name NGVDU

xa demos/ula-ng/ng_vdu_gfx.s -o /tmp/ng_vdu_gfx.bin
./bin2tap /tmp/ng_vdu_gfx.bin --start 0x0500 --exec 0x0500 -o demos/ula-ng/ng_vdu_gfx.tap --name NGVDUGFX

xa demos/ula-ng/ng_vdu_spr.s -o /tmp/ng_vdu_spr.bin
./bin2tap /tmp/ng_vdu_spr.bin --start 0x0500 --exec 0x0500 -o demos/ula-ng/ng_vdu_spr.tap --name NGVDUSPR
```

## Reference

ULA-NG specification: `docs/ula-ng/ULA-NG-SPEC.md`. Register map
(`$0340-$035F`), unlock sequence, and details of the 8 features (§5.1
palette indirection → §5.8 chunky/80col).
