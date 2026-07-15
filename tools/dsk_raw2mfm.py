#!/usr/bin/env python3
"""dsk_raw2mfm.py -- convertit une image disque Oric BRUTE (secteurs concaténés,
produite p.ex. par `oric1-emu --disk-create`) en conteneur `MFM_DISK` (cadré
A1 A1 A1 FE/FB + CRC), compatible `tap2sedoric`, `sedoric-info` et les outils MFM.

Ordre des secteurs de l'image RAW :
  - 'sidemajor' (défaut, VÉRIFIÉ pour SEDORIC : toutes les pistes de la face 0
    puis toutes celles de la face 1) ;
  - 'interleaved' (track-major side-interleaved -- ne convient PAS à SEDORIC).

Framing répliqué d'un vrai disque MFM Oric :
  gap initial 60x$4E, puis par secteur :
    12x$00 | A1 A1 A1 FE [trk side sec size] CRC | 22x$4E | 12x$00 |
    A1 A1 A1 FB [256 data] CRC | 38x$4E
  puis padding $4E jusqu'a 6400 o/piste.

Ordre des blocs de piste du conteneur MFM : SIDE-MAJOR (toutes les pistes de la
face 0 puis celles de la face 1), conforme au lecteur MFM de Phosphoric
(src/storage/sedoric.c : track_idx = side*tracks + track).

Usage : dsk_raw2mfm.py <raw.dsk> <out_mfm.dsk> [order] [sides] [tracks] [sectors]

Port depuis le projet SCUMM-Oric (scumm2oric/tools). Format de piste vérifié
byte-exact contre un vrai disque MFM ; voir docs/SEDORIC.md.
"""
import sys
import struct

TRK_RAW = 6400
SECSZ = 256
SIZE_CODE = 0x01  # 256 octets/secteur


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if (c & 0x8000) else (c << 1) & 0xFFFF
    return c


def build_track(track, side, sectors_data):
    """sectors_data : liste de N blocs de 256 octets (secteurs 1..N)."""
    out = bytearray()
    out += b"\x4E" * 60                       # gap initial
    for si, data in enumerate(sectors_data):
        sec = si + 1
        out += b"\x00" * 12                   # sync
        idf = bytes([0xA1, 0xA1, 0xA1, 0xFE, track, side, sec, SIZE_CODE])
        out += idf
        c = crc16(idf)
        out += bytes([(c >> 8) & 0xFF, c & 0xFF])
        out += b"\x4E" * 22                   # gap2
        out += b"\x00" * 12                   # sync
        df = bytes([0xA1, 0xA1, 0xA1, 0xFB]) + data
        out += df
        c = crc16(df)
        out += bytes([(c >> 8) & 0xFF, c & 0xFF])
        out += b"\x4E" * 38                   # gap3
    if len(out) > TRK_RAW:
        raise SystemExit("piste trop longue: %d > %d" % (len(out), TRK_RAW))
    out += b"\x4E" * (TRK_RAW - len(out))     # padding fin de piste
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    raw = open(sys.argv[1], "rb").read()
    outf = sys.argv[2]
    order = sys.argv[3] if len(sys.argv) > 3 else "sidemajor"
    sides = int(sys.argv[4]) if len(sys.argv) > 4 else 2
    tracks = int(sys.argv[5]) if len(sys.argv) > 5 else 42
    sectors = int(sys.argv[6]) if len(sys.argv) > 6 else 17
    expect = sides * tracks * sectors * SECSZ
    if len(raw) != expect:
        sys.stderr.write("ATTENTION: taille %d != attendu %d\n" % (len(raw), expect))

    hdr = bytearray(b"MFM_DISK")
    hdr += struct.pack("<I", sides)
    hdr += struct.pack("<I", tracks)
    hdr += struct.pack("<I", 1)               # geometry
    hdr += b"\x00" * (256 - len(hdr))

    out = bytearray(hdr)
    # Blocs du conteneur MFM émis en SIDE-MAJOR (face 0 entière puis face 1),
    # position fichier = side*tracks+track — l'ordre lu par l'émulateur Phosphoric.
    # `order` sélectionne l'interprétation de l'image RAW en entrée.
    for side in range(sides):
        for track in range(tracks):
            if order == "sidemajor":
                block = side * tracks + track  # RAW : toutes pistes face0 puis face1
            else:
                block = track * sides + side   # RAW : track-major side-interleaved
            base = block * sectors * SECSZ
            secs = [raw[base + i * SECSZ: base + (i + 1) * SECSZ] for i in range(sectors)]
            out += build_track(track, side, secs)
    open(outf, "wb").write(out)
    print("ecrit %s : MFM_DISK sides=%d tracks=%d sectors=%d taille=%d" %
          (outf, sides, tracks, sectors, len(out)))


if __name__ == "__main__":
    main()
