#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
"""Compare deux images PPM (P6) ligne par ligne.

Affiche « <lignes différentes> <lignes partiellement différentes> ».

Une ligne *partiellement* différente — identique sur une partie de sa largeur,
différente sur le reste — prouve que l'échantillonnage vidéo est **intra-ligne** :
la coupure ne tombe pas sur une frontière de scanline. C'est le critère du test
`test_raster_split.sh` (épic V2-E4 : l'ULA fetche une cellule par cycle).
"""

import sys


def load(path):
    """Renvoie (largeur, hauteur, octets de pixels RGB) d'un PPM binaire P6."""
    data = open(path, "rb").read()
    if data[:2] != b"P6":
        raise SystemExit(f"{path}: ce n'est pas un PPM binaire (P6)")
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":                      # commentaire
            while i < len(data) and data[i : i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1                                               # le blanc après maxval
    w, h, _maxval = fields
    return w, h, data[i:]


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: ppm_diff_lines.py a.ppm b.ppm")
    w1, h1, a = load(sys.argv[1])
    w2, h2, b = load(sys.argv[2])
    if (w1, h1) != (w2, h2):
        print("0 0")                                     # dimensions différentes
        return

    different = partial = 0
    for y in range(h1):
        row_a = a[y * w1 * 3 : (y + 1) * w1 * 3]
        row_b = b[y * w1 * 3 : (y + 1) * w1 * 3]
        if row_a == row_b:
            continue
        different += 1
        same = sum(1 for x in range(w1)
                   if row_a[x * 3 : x * 3 + 3] == row_b[x * 3 : x * 3 + 3])
        if 0 < same < w1:
            partial += 1
    print(f"{different} {partial}")


if __name__ == "__main__":
    main()
