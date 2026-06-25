# Brouillon de post — forum.defence-force.org t=2709

Réponse à **Sodiumlightbaby** (message du 24 juin 23:47 : registres write-only
en espace ROM + page-3 RAM remappable + unlock magique). Langue : anglais.
Direction validée 2026-06-25 : **s'aligner sur son schéma de registres** et
proposer de déplacer palette + bordure hors de la DRAM (`$BFE0-$BFFF`) vers ses
registres write-only → zéro octet DRAM, clôt l'objection `$BFE0-$C000` de Dbug.
Phosphoric implémente aujourd'hui la version in-band (Sprints 64-65 + démo
`oculabord`) ; la migration vers les registres est un follow-up suspendu à son
accord. Spec : `docs/ocula_extensions.md` v0.10.

---

@Sodiumlightbaby — thanks, that register plan makes sense and I'd rather build
on it than around it. Quick reactions and one concrete proposal.

**Your scheme, as I read it:** unlock via magic writes in ROM space → write-only
registers selected by the high address byte (64 of them, one per ROM page) → a
mapping register exposes the hidden 256-byte page-3 RAM so software can read back
the OCULA signature and state. I'm on board with all of it — it keeps
DRAM-is-the-RAM viable and the unlock is the same blind-ROM-write idea I'd
sketched.

**Proposal: move the palette and the border into those write-only registers,
out of DRAM entirely.** I'd originally put the 8-entry RGB332 palette and the
border colour in-band at $BFE0-$BFFF (the bytes just above the text screen). But
that's exactly the region Dbug and Symoon flagged as in use (Encounter, FT-DOS,
Seoric). If instead each palette entry and the border are OCULA write-only
registers — one ROM page each, ~9 of your 64 — the feature consumes **zero DRAM**
and Dbug's collision objection disappears entirely.

- **RGB332 still wins:** one byte per register = one atomic write, so the ULA
  never latches a half-updated colour. RGB444 would need two writes per entry → a
  visible wrong colour if the line is sampled between them.
- **Per-scanline rasters still work:** the CPU just rewrites the relevant
  register mid-frame, timed to the line — the classic copper pattern — and the
  ULA reads its own internal register at each scanline start. One write = a single
  split (the 1985 Multicoloric card), one per line = raster bars, every HBLANK =
  a plasma. The border register is what gives Dbug the raster bars bleeding into
  the overscan.
- **Border = $00 (or pre-unlock) ⇒ black border**, byte-for-byte a stock Oric.
- I'd also fold identification / banking / GPU into your page-3 RAM mapping
  rather than the fixed $03E0 window I'd drafted — same idea, your version is more
  flexible.

I've got the in-band version implemented and tested in Phosphoric today (palette,
border, the visible overscan around the active image, and a little BASIC demo),
so I can validate timing either way — but I'd rather land on your register layout
than keep my own. Does moving palette + border into the write-only register space
work for you, and is there a page range you'd want them to sit at?
