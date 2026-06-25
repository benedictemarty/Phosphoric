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

## Post #2 — À POSTER (réponse à sodiumlb + Dbug)

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
