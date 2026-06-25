# Brouillon de post — forum.defence-force.org t=2709

Réponse à Dbug (bordure + RGB332) sur le fil OCULA, à poster après le retour
du 24 juin 2026. Langue : anglais (langue du fil). Spec : `docs/ocula_extensions.md`
v0.10. Implémentation : Phosphoric Sprints 64-65 (registre $BFEA + rendu
overscan visible, 11 tests) + démo BASIC `demos/ocula/oculabord`.

---

@Dbug — thanks, both of your points pushed the spec in the right direction.
I've folded them in and prototyped the result in Phosphoric so it's not just
paper. Two answers:

**1. Border colour — yes, and for free as raster bars.**

The RGB332 palette only needs 8 bytes ($BFE0-$BFE7), so the 32-byte block
$BFE0-$BFFF has room to spare. I'm using the first free byte as a border
register:

- **$BFEA = BORDER**, an RGB332 colour, **re-read at the start of every
  scanline** — exactly like the palette. So rewriting it between scanlines
  gives you **border raster bars** that bleed past the active area, which I
  think is what you were after.
- It sits under the **same opt-in gate** as the palette (unlock knock + the
  'O','C' magic at $BFE8-$BFE9). Locked, or magic absent, or on a stock
  HCS 10017, $BFEA is plain RAM and the border stays black.
- **$BFEA = $00 ⇒ black border**, i.e. byte-for-byte a normal Oric even when
  the block is armed. A program that only wants the border (not a redefined
  palette) just arms the magic and writes the standard Oric palette into
  $BFE0-$BFE7.

The remaining bytes $BFEB-$BFFF stay reserved — that's the "position
registers" idea you raised: I've earmarked BORDER_CTL (border as a palette
index), in-band SCROLLX/Y, and a SPLIT line, but left them v2 so we don't
freeze an encoding before the hardware has an opinion.

**2. RGB332 vs RGB444 — agreed, RGB332.**

We're aligned, and your atomicity argument is the clincher: at 1 byte per
entry a colour write is **atomic**, so the per-scanline re-read never catches
a half-updated value. RGB444 (2 bytes/entry) could be sampled mid-update
between the low and high byte and flash a wrong colour on a line — and it
would eat 16 bytes, killing the register space above. So: 8× RGB332, no
double-buffer needed on the RP2350 side.

**On per-scanline vs "continuous" changes:** these are the same mechanism, the
only variable is how often you write. The block is latched once per scanline
(during the preceding HBLANK); a write that lands in that window applies to
the next line. One write = a single split (the 1985 Multicoloric card), one
write per line = copper-style rasters, a write every HBLANK = a plasma. The
floor is one scanline — there's no intra-line change, faithful to the
line-by-line fetch.

All of this degrades cleanly on a stock ULA and needs no prior detection
(tier 1, in-band). It's all live in Phosphoric now — the $BFEA register, the
visible overscan rendering around the active image, and a tiny BASIC demo that
cycles the border colour — so it's testable end to end rather than just on
paper.

Does $BFEA work for you as the border register, or would you rather it be a
palette index than a direct RGB332 value?
