# Brouillons de posts — forum.defence-force.org t=2709

Suivi des échanges OCULA (palette/bordure/registres). Langue : anglais.

## Post #1 — POSTÉ le 2026-06-25 (p≈34924-aligné, réponse à sodiumlb)

Proposait de déplacer palette + bordure de l'in-band `$BFE0-$BFFF` vers les
registres write-only ROM de sodiumlb (zéro DRAM, clôt l'objection de Dbug).
→ Réponses obtenues le 25 juin :
- **sodiumlb (p=34926)** : « page 3 RAM solution with writeable locations for
  palette and border gives the same capability as the 32 bytes solution just
  with more space and mapping flexibility » ; « ROM space writes is the only
  solution so far (for this mode) that allows instant response ». → **Direction
  validée** : page-3 RAM = stockage/config, écritures ROM = chemin instantané
  (rasters).
- **Dbug (p=34925)** : « VSync / HSync is absolutely necessary to get proper
  rasters and color changes that don't flash crazily » + question 80 col
  (écran/charset en place + page flipping, ou descendus en mémoire ?).

## Post #2 — POSTÉ le 2026-06-25 (p=34927, réponse à sodiumlb + Dbug)

Great, thanks both — that pins it down.

@Sodiumlightbaby — settled, then: palette and border live in the page-3 RAM
writeable locations (the extra space and mapping flexibility is a nice bonus
over my 32-byte block), with the ROM-space write-only registers as the
instant-response path for DRAM-is-the-RAM. I'll target exactly that split —
page-3 RAM as the configurable home, ROM-space writes for the per-line updates —
and drop the $BFE0-$BFFF footprint entirely, which also clears the collision
worry Dbug and Symoon raised.

@Dbug — fully agreed, raster and colour changes are worthless without tight
sync; cycle-counting alone flashes all over the place. Since OCULA owns the
video timing, the clean fix is for it to *expose* that: a readable current-line
value plus a "wait for next HSync/VSync" so a copper-style routine lands its
register writes on the right scanline instead of guessing. Would a readable line
counter + an HSync/VSync wait cover what you'd want to drive, or do you have a
preferred shape for the sync hook?

On 80 columns: the screen memory **moves** — it goes to $A000 (80 bytes/row × 28
rows = $A000-$A8BF, the area HIRES would otherwise use), so it never overlaps the
charsets, which stay exactly where they are ($B400 standard, $B800 alternate).
No page flipping for the text itself; it's just a wider framebuffer read from a
lower base.

## Post #3 — À POSTER (layout concret du page-3 RAM, pour validation sodiumlb)

@Sodiumlightbaby — I went ahead and wired your page-3 RAM idea into Phosphoric
so there's something concrete to shoot at rather than hand-waving. Here's the
shape I picked — tell me where it diverges from what you have in mind.

- **Mapping register** — one write-only ROM page (I used **$EB**): `STA $EBxx`
  with the target page in the data byte maps a **256-byte window** there;
  writing 0 unmaps it. Gated by the unlock; re-locking unmaps. So a program
  does: unlock → set the map register → check the signature in the window →
  use it.
- **Window layout** (offsets in the mapped page):
  - `$00/$01` = `'O'/'C'` signature
  - `$02` = capability bits
  - `$03` = CPU bank (R/W — drives the $A000-$BFFF banking)
  - `$04-$0B` = the 8 RGB332 palette entries (R/W)
  - `$0C` = border (R/W)
  - rest reserved

The nice part is this **unifies the two halves** you described: the ROM-space
write-only registers stay the instant per-line path (rasters), and these same
palette/border bytes are simply **readable/writeable** through the window — so
the page-3 RAM gives back the read-back that the blind-write registers can't.
Both views touch the same state.

Open questions for you: is **$EB** (or any single ROM page) fine for the mapping
register, do you have a preferred **default/parking page** for the window, and
does the byte layout above match how you'd lay out the 256 bytes — or would you
rather the palette/border sit at different offsets (or even keep the window
purely for ID + control and leave palette/border to the ROM writes only)?

It's all behind the unlock and coexists with the older fixed $03E0 window in the
emulator, so nothing regresses while we settle the encoding.
