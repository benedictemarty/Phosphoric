# ULA-NG — User guide

A "next-generation" ULA for Phosphoric: video extensions enabled by
unlocking, **indistinguishable from an HCS 10017 while locked**.
Software reference for a future FPGA port (Sipeed Tang Primer 20K /
GW2A-18).

> Status: **8 features implemented and validated** (§5.1 palette indirection →
> §5.8 chunky/80col). See `ULA-NG-SPEC.md` (full spec) and `AUDIT.md`
> (architecture). Runnable demos: `demos/ula-ng/`.

---

## 0. "Normal ULA" mode (the default)

At reset, **the ULA-NG is locked**: it behaves exactly like
the original ORIC-1/Atmos ULA (HCS 10017), **bit for bit**. The register
window `$0340-$035F` then falls through to the VIA. **No program sees any
difference** unless it explicitly unlocks the ULA-NG.

In other words: *do nothing = normal ULA*. There is **no flag to pass**
to the emulator — the demos unlock it themselves (`-t ng_chunky.tap -f`).

> The ULA-NG is an independent layer, dormant by default: as long as it is
> not unlocked, the chip remains indistinguishable from a standard HCS 10017 ULA.

---

## 1. There is no BASIC keyword

The Oric's BASIC is frozen in ROM (1983) and knows nothing of the ULA-NG: **no
keyword** such as `HIRES`/`TEXT` activates it. The ULA-NG is driven **by writing
its registers**, through three equivalent means:

1. **`POKE`** from BASIC (addresses in decimal or `#hex`).
2. **`STA $034x`** in machine code (see the `.s` demos).
3. **`--ula-ng-poke "…"`** on the emulator side (injection at startup).

This is faithful to real hardware: on an FPGA board, the registers would also
be driven with POKE, like any Oric hardware extension. A set of extended
keywords would require a **companion extension ROM** (not provided).

---

## 2. Unlocking

Register window: **`$0340`-`$035F`**. Sequence: write `'N'` (`$4E`) then
`'G'` (`$47`) to **`$0340`**, with no other write to the window in between.

```asm
        LDA #$4E : STA $0340        ; 'N'
        LDA #$47 : STA $0340        ; 'G'
        LDA $0340 : CMP #$1E : BNE no_ng             ; NG_ID = version?
        LDA $034F : EOR $0340 : CMP #$FF : BNE no_ng  ; handshake ~NG_ID
        ; ULA-NG present and unlocked
no_ng:
```

In BASIC: `POKE#340,78:POKE#340,71` (78=`$4E`, 71=`$47`).

A **reset re-locks everything** → immediate return to the normal ULA.

---

## 3. Enabling a mode (after unlocking)

Each feature is armed by a register. `NG_MODE` (`$0341`) carries most of them:

| Feature (spec) | Register / bits | `NG_MODE` value |
|---|---|---|
| Palette, copper, fine scroll, start address, raster IRQ | `NG_MODE.b0` (extensions active) | `$01` |
| Parallel attributes (§5.6) | `NG_MODE.b1` | `$02` |
| **Chunky 4bpp** (§5.8) | `NG_MODE` b0 + b2-3 = `01` | `$05` |
| **80-column text** (§5.8) | `NG_MODE` b0 + b2-3 = `10` | `$09` |
| **Sprites** (§5.7) | `NG_SPR_CTRL.b0` (`$0350`), **independent** of `NG_MODE` | — |

**Returning to normal** without a reset: write `NG_MODE=0` (and `NG_SPR_CTRL=0`) — the
visual extensions are disabled (standard rendering), while the window stays
owned until reset.

---

## 4. Register map (`$0340`-`$035F`)

| Addr | Name | R/W | Role |
|---|---|---|---|
| `$0340` | `NG_ID` (R) / `NG_LOCK` (W) | R/W | R: `$1E` if unlocked. W: 'N','G' sequence. |
| `$0341` | `NG_MODE` | R/W | b0 = extensions active; b1 = parallel attributes; b2-3 = video mode (00 std, 01 chunky, 10 80col). |
| `$0342`-`$0343` | `NG_SCRSTART` | W | Video fetch base (LSB/MSB). `$0000` = default. |
| `$0344` | `NG_SCROLLX` | W | Fine X offset (0-5 px). |
| `$0345` | `NG_SCROLLY` | W | Fine Y offset (0-7 px). |
| `$0346` | `NG_RASTERLINE` | W | Line (0-255) that triggers the raster IRQ. |
| `$0347` | `NG_STATUS` | R/W | R b7 = IRQ pending. W = acknowledge + b0 = IRQ enable. |
| `$0348` | `NG_PAL_IDX` | W | Palette LUT index (0-15), auto-increment. |
| `$0349` | `NG_PAL_DATA` lo | W | `0000RRRR`. |
| `$034A` | `NG_PAL_DATA` hi | W | `GGGGBBBB` (commit + index increment). |
| `$034B` | `NG_COP_CTRL` | W | Resets the copper list. |
| `$034C` | `NG_COP_DATA` | W | 3-byte-per-entry stream: `ligne` (line), `(idx<<4)|R`, `(G<<4)|B`. |
| `$034D` | `NG_ATTR_FILL` | W | Fills the whole 8 KB attribute plane + resets the pointer. Byte = `(paper<<3)|ink`. |
| `$034E` | `NG_ATTR_DATA` | W | 1-byte-per-cell stream (auto-increment, index `scanline*40+col`). |
| `$034F` | `NG_IDCHK` | R | `~NG_ID` (handshake). |
| `$0350` | `NG_SPR_CTRL` | W | b0 = global sprite enable. |
| `$0351` | `NG_SPR_SEL` | W | Selected sprite (0-15) + resets the pattern pointer. |
| `$0352` | `NG_SPR_X` | W | Sprite X position (0-255). |
| `$0353` | `NG_SPR_Y` | W | Y position (0-255). |
| `$0354` | `NG_SPR_ATTR` | W | b0 = sprite visible. |
| `$0355` | `NG_SPR_DATA` | W | 16×16 pattern stream: 1 byte/px (`0`=transparent, `1`-`7`=LUT index), auto-increment. |
| `$0356` | `NG_SPR_STATUS` | R | b7 = sprite-sprite collision (clear on read). |
| `$0357` | `NG_VDU` | W | Built-in VDU command stream (see [VDU.md](VDU.md)). |

---

## 5. `--ula-ng-poke` recipes (emulator)

The CLI flag programs the registers at startup. `SEQ` = `AAA=VV` pairs (hex)
separated by commas. Combine with `--screenshot-at CYCLES:FICHIER` (FICHIER = file).

```bash
# Palette: colour 7 (white) -> green
--ula-ng-poke "340=4E,340=47,341=01,348=07,349=00,34A=F0"

# Copper: colour 7 red (line 0) then blue (line 30) -> bands
--ula-ng-poke "340=4E,340=47,341=01,34B=00,34C=00,34C=7F,34C=00,34C=1E,34C=70,34C=0F"

# Start address: scroll by one text row ($BB80+40 = $BBA8)
--ula-ng-poke "340=4E,340=47,341=01,342=A8,343=BB"

# Parallel attributes: blue paper (4) + red ink (1) = $21
--ula-ng-poke "340=4E,340=47,341=02,34D=21"

# Chunky 4bpp: NG_MODE=$05 + palette index 0 = magenta (320px screen)
--ula-ng-poke "340=4E,340=47,341=05,348=00,349=0F,34A=0F"

# 80-column text: NG_MODE=$09 (480px screen)
--ula-ng-poke "340=4E,340=47,341=09"

# Raster IRQ at line 100 (enable): WARNING, without an ISR -> IRQ loop
--ula-ng-poke "340=4E,340=47,341=01,346=64,347=01"
```

BASIC equivalent: `POKE` the same addresses in decimal (`$0340`=832…). The BASIC
line length limit (~80 chars) means long sequences have to be split.

---

## 6. From machine code

```asm
        LDA #$4E : STA $0340       ; unlock 'N'
        LDA #$47 : STA $0340       ; unlock 'G'
        LDA #$01 : STA $0341       ; NG_MODE.b0 = extensions active
        LDA #$01 : STA $0348       ; palette index 1
        LDA #$0F : STA $0349       ; R=F
        LDA #$F0 : STA $034A       ; G=F,B=0 -> yellow, commit
```

For the raster IRQ, install an ISR that acknowledges (`STA $0347`) — this requires a
redirectable IRQ vector (Sedoric/overlay or a custom ROM), not bare BASIC.

Bulky data (sprite patterns, 16000-byte chunky images, 8000-cell attribute
plane) is programmed through streams and is best filled in
machine code rather than with BASIC `POKE` (slow).

---

## 7. Ready-to-use demos

`demos/ula-ng/` contains one demo per headline feature, with a launch
menu (`menu.sh`) and a detailed `README.md`:

| Demo | Feature | Source |
|---|---|---|
| `ng_chunky`     | Full-screen chunky 4bpp 320×224, 16 colours, animated palette | machine code |
| `ng_text80`     | 80-column text (480 px)                                        | BASIC |
| `ng_attributes` | Per-cell colour mosaic (no colour clash)                       | machine code |
| `ng_copper`     | Rainbow raster bars (palette per scanline)                     | BASIC |
| `ng_sprite`     | Bouncing 16×16 sprite                                          | BASIC |

```bash
make SDL2=1
demos/ula-ng/menu.sh
```

---

## 7a. Built-in VDU (`NG_VDU` $0357)

Instead of writing the registers one by one, you can **stream VDU-style
commands** into `$0357`; the interpreter lives **inside the ULA-NG** (the 6502 carries
no driver). v0.1 set: `20` reset, `22 n` MODE (0 std/1 chunky/2 80col),
`19 l r g b` palette, `18 a` per-cell background colour, `31 col row a` colour one
cell (without colour clash). BASIC example (blue paper/red ink = `$21`):

```basic
POKE#340,78:POKE#340,71 : REM unlock
POKE#357,18:POKE#357,#21 : REM VDU 18, $21
```

Details, upload protocol (v0.2) and state of the art: **[VDU.md](VDU.md)**.

## 8. Reference

- `docs/ula-ng/VDU.md` — built-in VDU (`NG_VDU` command port).
- `docs/ula-ng/ULA-NG-SPEC.md` — full specification (registers, timing,
  implementation decisions, FPGA target).
- `docs/ula-ng/AUDIT.md` — architecture and "FPGA mirror" boundaries.
- `demos/ula-ng/README.md` — how to use the demos + rebuilding the `.tap` files.
