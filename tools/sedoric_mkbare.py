#!/usr/bin/env python3
"""sedoric_mkbare.py -- rend un disque système SEDORIC « nu » : neutralise (ou
remplace) l'INIST (autoexec de boot) pour qu'il démarre directement au prompt
`Ready` au lieu de lancer une application (menu, jeu...).

Utile pour tester un `.COM` maison sur un master Sedoric sans programme de boot
concurrent (cf docs/SEDORIC.md §6). Méthode déterministe (pas de pilotage
--type-keys, contrairement à make_bootable_sedoric.sh qui échoue si le master
boote sur un menu).

L'INIST est en piste 20 secteur 1, offset 0x1E..0x59 (60 octets ASCII terminés
par #00 -- manuel SEDORIC 3.0, sedna3_0.pdf). Fonctionne sur une image MFM_DISK
(CRC du secteur data refait) ou RAW side-major.

Usage : sedoric_mkbare.py <in.dsk> <out.dsk> [nouvel_INIST]
  sans nouvel_INIST : neutralise l'INIST (boot -> Ready).
  avec              : y écrit ces commandes (ex: "LOAD\"PROBE\"" pour autolancer).

Vérifié : SEDO40u.DSK (INIST "CLS:MENU.LNG:MENU") -> boot "SEDORIC V4.0 / Ready".
"""
import sys

SECSZ = 256
TRK_RAW = 6400
MFM_HDR = 256
DIR_TRACK = 20
SYS_SECTOR = 1
INIST_OFF = 0x1E
INIST_MAX = 60


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if (c & 0x8000) else (c << 1) & 0xFFFF
    return c


def patch_inist(buf, off, text):
    """écrit l'INIST (ou le neutralise) dans le champ de données à `off`."""
    for i in range(INIST_OFF, INIST_OFF + INIST_MAX):
        buf[off + i] = 0
    if text:
        enc = text.encode("ascii")
        if len(enc) >= INIST_MAX:
            sys.exit("INIST trop long (%d >= %d)" % (len(enc), INIST_MAX))
        buf[off + INIST_OFF:off + INIST_OFF + len(enc)] = enc


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    d = bytearray(open(sys.argv[1], "rb").read())
    outf = sys.argv[2]
    new_inist = sys.argv[3] if len(sys.argv) > 3 else ""

    if len(d) > MFM_HDR and d[:8] == b"MFM_DISK":
        tracks = d[12] | d[13] << 8
        # bloc side-major : face 0, piste 20
        base = MFM_HDR + (0 * tracks + DIR_TRACK) * TRK_RAW
        td = d[base:base + TRK_RAW]
        off = None
        for i in range(TRK_RAW - 4):
            if (td[i] == 0xA1 and td[i+1] == 0xA1 and td[i+2] == 0xA1
                    and td[i+3] == 0xFE and td[i+6] == SYS_SECTOR):
                for j in range(i + 10, i + 60):
                    if td[j] == 0xA1 and td[j+1] == 0xA1 and td[j+2] == 0xA1 and td[j+3] == 0xFB:
                        off = base + j + 4
                        break
                break
        if off is None:
            sys.exit("secteur système (t20 s1) introuvable dans le MFM")
        patch_inist(d, off, new_inist)
        crc = crc16(bytes([0xA1, 0xA1, 0xA1, 0xFB]) + bytes(d[off:off + SECSZ]))
        d[off + SECSZ] = (crc >> 8) & 0xFF
        d[off + SECSZ + 1] = crc & 0xFF
        fmt = "MFM_DISK"
    else:
        # RAW side-major : t20 s1 face 0 = (0*tracks + 20)*17 + 0
        # géométrie inférée : 17 secteurs/piste, 2 faces si taille le permet
        sectors = 17
        n = len(d) // (sectors * SECSZ)
        tracks = n // 2 if n % 2 == 0 and n > DIR_TRACK * 2 else n
        off = ((0 * tracks + DIR_TRACK) * sectors + (SYS_SECTOR - 1)) * SECSZ
        if off + SECSZ > len(d):
            sys.exit("image RAW trop petite pour la piste 20")
        patch_inist(d, off, new_inist)
        fmt = "RAW"

    open(outf, "wb").write(bytes(d))
    print("%s : INIST %s -> %s (%s)" % (
        outf, ("= %r" % new_inist) if new_inist else "neutralisé",
        "boot Ready" if not new_inist else "autolance", fmt))


if __name__ == "__main__":
    main()
