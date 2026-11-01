#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
"""contact_sheet.py — planches contact des écrans capturés par tap_sweep.sh (2.31.10).

Usage : contact_sheet.py SHOTS_DIR OUT_PREFIX [COLS] [ROWS]

Assemble les captures (*.ppm/*.png) en grilles de vignettes légendées
(OUT_PREFIX_001.png, …), pour un examen à l'œil des écrans HIRES qu'aucune
vérification texte ne voit. Nécessite Pillow.

Author: bmarty <bmarty@mailo.com>
"""
import os
import sys

from PIL import Image, ImageDraw

src, prefix = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 8
rows = int(sys.argv[4]) if len(sys.argv) > 4 else 6
TW, TH, LAB = 240, 224, 12
files = sorted(f for f in os.listdir(src) if f.lower().endswith((".ppm", ".png")))
per = cols * rows
for page in range(0, len(files), per):
    sheet = Image.new("RGB", (cols * TW, rows * (TH + LAB)), (40, 0, 40))
    d = ImageDraw.Draw(sheet)
    for i, f in enumerate(files[page:page + per]):
        x, y = (i % cols) * TW, (i // cols) * (TH + LAB)
        try:
            im = Image.open(os.path.join(src, f)).convert("RGB").resize((TW, TH))
            sheet.paste(im, (x, y + LAB))
        except OSError:
            pass
        d.text((x + 2, y), f.rsplit(".", 2)[0][:38], fill=(255, 255, 0))
    out = f"{prefix}_{page // per + 1:03d}.png"
    sheet.save(out)
    print(out, len(files[page:page + per]))
