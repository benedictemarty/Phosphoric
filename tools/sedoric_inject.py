#!/usr/bin/env python3
"""sedoric_inject.py -- injecte un binaire comme fichier SEDORIC dans une image
disque RAW (secteurs concaténés, ordre side-major : toutes les pistes de la
face 0 puis celles de la face 1). Convertir ensuite en MFM avec `dsk_raw2mfm.py`.

Opère en RAW (offsets directs, pas de cadrage MFM/CRC) -- alternative robuste à
l'injection directe en MFM. À la différence de la version d'origine (SCUMM-Oric),
CETTE version est MULTI-FICHIERS SÛRE : elle lit le catalogue et les descripteurs
déjà présents pour exclure les secteurs occupés avant d'allouer (deux injections
successives ne se marchent plus dessus). Voir docs/SEDORIC.md.

Format SEDORIC (vérifié byte-exact contre le manuel « SEDORIC 3.0 »,
sedna3_0.pdf ; cf docs/SEDORIC.md) :
  - Secteur Système = piste 20 sec 1 : +9..29 nom disque (21 o),
    +0x1E..0x59 INIST (60 o : commandes ASCII exécutées au boot, séparées par
    ':', terminées par #00) -- l'AUTOEXEC de démarrage.
  - BITMAP/VTOC = piste 20 sec 2 : +2,+3 free (LE), +4,+5 nb fichiers (LE).
  - Directory = piste 20 sec 4 : +0,+1 lien suivant (0=fin), +2 high-water mark,
    entrées 16 o à partir de +16 : name[9] ext[3] trk sec nsec status(0x40).
  - Descripteur fichier : +0,+1 lien | +2 = 0xFF | +3 type (b0=AUTO, b6=bloc
    data, b7=BASIC → 0x40 = ML, 0x41 = AUTO ML) | +4,+5 load LE | +6,+7 end LE |
    +8,+9 exec LE si AUTO | +0xA,+0xB nb secteurs data LE | +0xC.. carte
    (trk,sec)×n terminée par 00 00.

Usage : sedoric_inject.py <raw_in.dsk> <bin> <load_hex> <NAME.EXT> <raw_out.dsk>
        [tracks=42] [sectors=17] [init=CMD] [exec_hex=load]
  init     : si fourni, écrit l'INIST (autoexec) avec cette commande.
  exec_hex : adresse d'exécution AUTO (défaut = load).
"""
import sys

SECSZ = 256
DIR_TRACK = 20
SYS_SECTOR = 1          # Secteur Système (nom disque, INIST)
VTOC_SECTOR = 2         # BITMAP (free / nb fichiers)
DIR_SECTOR = 4          # Directory
INIST_OFF = 0x1E        # offset INIST dans le Secteur Système
INIST_MAX = 60          # longueur max INIST


def main():
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    raw = bytearray(open(sys.argv[1], "rb").read())
    data = open(sys.argv[2], "rb").read()
    # Le loader SEDORIC charge des secteurs entiers : padder au multiple de 256
    # pour que tout le binaire soit chargé (pas de dernier secteur partiel perdu).
    if len(data) % SECSZ:
        data += b"\x00" * (SECSZ - len(data) % SECSZ)
    load = int(sys.argv[3], 16)
    namespec = sys.argv[4].upper()
    outf = sys.argv[5]
    tracks = int(sys.argv[6]) if len(sys.argv) > 6 else 42
    sectors = int(sys.argv[7]) if len(sys.argv) > 7 else 17
    init_cmd = sys.argv[8] if len(sys.argv) > 8 else ""
    exec_given = len(sys.argv) > 9              # exec fourni => fichier AUTO
    execaddr = int(sys.argv[9], 16) if exec_given else load

    nm, _, ex = namespec.partition(".")
    name = (nm[:9] + " " * 9)[:9]
    ext = (ex[:3] + " " * 3)[:3]

    def off(track, side, sec):              # side-major : bloc = side*tracks+track
        return ((side * tracks + track) * sectors + (sec - 1)) * SECSZ

    def rd(track, sec):
        o = off(track, 0, sec)
        return raw[o:o + SECSZ]

    def wr(track, sec, buf):
        o = off(track, 0, sec)
        raw[o:o + SECSZ] = (bytes(buf) + b"\x00" * SECSZ)[:SECSZ]

    # --- carte des secteurs déjà occupés (multi-fichiers sûr) : parcours du
    #     catalogue puis des cartes de secteurs des descripteurs existants ---
    used = set()
    dt, ds, guard = DIR_TRACK, DIR_SECTOR, 0
    while guard < 64:
        guard += 1
        dirs = rd(dt, ds)
        used.add((dt, ds))                   # le secteur catalogue lui-même
        for e in range(16, SECSZ, 16):
            if dirs[e] == 0 and dirs[e + 15] == 0:
                continue
            if dirs[e + 15] & 0x80:          # supprimé
                continue
            cdt, cds, dg = dirs[e + 12], dirs[e + 13], 0
            while dg < 64:
                dg += 1
                desc = rd(cdt, cds)
                used.add((cdt, cds))         # le secteur descripteur lui-même
                p = 12
                while p + 1 < SECSZ:
                    if desc[p] == 0 and desc[p + 1] == 0:
                        break
                    used.add((desc[p], desc[p + 1]))
                    p += 2
                if desc[0] == 0 and desc[1] == 0:
                    break
                cdt, cds = desc[0], desc[1]
        if dirs[0] == 0 and dirs[1] == 0:
            break
        dt, ds = dirs[0], dirs[1]

    end = load + len(data) - 1
    ndata = (len(data) + SECSZ - 1) // SECSZ
    total = ndata + 1                       # descripteur + secteurs data

    # --- allouer total secteurs depuis piste 21, face 0, hors secteurs occupés ---
    alloc = []
    t, s = 21, 1
    while len(alloc) < total and t < tracks:
        if t != DIR_TRACK and (t, s) not in used:
            alloc.append((t, s))
        s += 1
        if s > sectors:
            s = 1
            t += 1
    if len(alloc) < total:
        sys.exit("pas assez de secteurs libres")
    desc_t, desc_s = alloc[0]

    # --- secteur descripteur ---
    # AUTO ($41) dès qu'une adresse d'exécution est fournie (ou un INIST) :
    # requis pour que `LOAD"NOM"` charge ET exécute le fichier (cf docs §6).
    is_auto = bool(init_cmd) or exec_given
    d = bytearray(SECSZ)
    d[0] = 0; d[1] = 0                     # lien : pas de descripteur suivant
    d[2] = 0xFF                            # premier descripteur
    d[3] = 0x41 if is_auto else 0x40       # b6=bloc data, b0=AUTO
    d[4] = load & 0xFF; d[5] = (load >> 8) & 0xFF
    d[6] = end & 0xFF;  d[7] = (end >> 8) & 0xFF
    if is_auto:
        d[8] = execaddr & 0xFF; d[9] = (execaddr >> 8) & 0xFF
    d[10] = ndata & 0xFF; d[11] = (ndata >> 8) & 0xFF
    p = 12
    for (dt2, ds2) in alloc[1:]:
        d[p] = dt2; d[p + 1] = ds2; p += 2
    d[p] = 0; d[p + 1] = 0
    wr(desc_t, desc_s, d)

    # --- secteurs data ---
    for i, (dt2, ds2) in enumerate(alloc[1:]):
        wr(dt2, ds2, data[i * SECSZ:(i + 1) * SECSZ])

    # --- entrée directory (piste 20 sec 4) ---
    # Parcours de la chaîne de secteurs catalogue (t20 s4 -> lien +0,+1 -> ...)
    # pour trouver un slot libre ; si toute la chaîne est pleine, on alloue un
    # secteur libre et on le chaîne (les secteurs catalogue sont localisés par
    # lien piste/secteur, pas par zone fixe -- manuel SEDORIC 3.0, ANNEXE 7).
    cat_t, cat_s, slot, new_cat, guard = DIR_TRACK, DIR_SECTOR, None, False, 0
    while guard < 64:
        guard += 1
        dirs = bytearray(rd(cat_t, cat_s))
        nent = sum(1 for e in range(16, SECSZ, 16) if dirs[e] != 0 or dirs[e + 15] != 0)
        s = 16 + nent * 16
        if s + 16 <= SECSZ:
            slot = s
            break
        if dirs[0] == 0 and dirs[1] == 0:
            break                            # fin de chaîne, tout est plein
        cat_t, cat_s = dirs[0], dirs[1]
    if slot is None:
        used.add((desc_t, desc_s))
        for (dt2, ds2) in alloc[1:]:
            used.add((dt2, ds2))
        nt, ns, ncat = 21, 1, None
        while nt < tracks:
            if nt != DIR_TRACK and (nt, ns) not in used:
                ncat = (nt, ns); break
            ns += 1
            if ns > sectors:
                ns = 1; nt += 1
        if ncat is None:
            sys.exit("plus de secteur libre pour un nouveau secteur catalogue")
        dirs[0], dirs[1] = ncat            # lien précédent -> nouveau
        wr(cat_t, cat_s, dirs)
        dirs = bytearray(SECSZ)            # nouveau secteur catalogue vierge
        cat_t, cat_s = ncat
        slot, new_cat = 16, True
    dirs[slot:slot + 9] = name.encode("ascii")
    dirs[slot + 9:slot + 12] = ext.encode("ascii")
    dirs[slot + 12] = desc_t
    dirs[slot + 13] = desc_s
    dirs[slot + 14] = total
    dirs[slot + 15] = 0x40
    dirs[2] = (slot + 16) & 0xFF            # high-water mark
    wr(cat_t, cat_s, dirs)

    # --- VTOC (piste 20 sec 2) : free -= total (+1 si nouveau secteur cat.), files += 1 ---
    v = bytearray(rd(DIR_TRACK, VTOC_SECTOR))
    free = (v[2] | v[3] << 8) - total - (1 if new_cat else 0)
    files = (v[4] | v[5] << 8) + 1
    v[2] = free & 0xFF; v[3] = (free >> 8) & 0xFF
    v[4] = files & 0xFF; v[5] = (files >> 8) & 0xFF
    wr(DIR_TRACK, VTOC_SECTOR, v)

    # --- INIST (autoexec) : piste 20 sec 1, offset 0x1E, ASCII + #00 ---
    if init_cmd:
        if len(init_cmd) >= INIST_MAX:
            sys.exit("INIST trop long (%d >= %d)" % (len(init_cmd), INIST_MAX))
        sysd = bytearray(rd(DIR_TRACK, SYS_SECTOR))
        for i in range(INIST_OFF, INIST_OFF + INIST_MAX):
            sysd[i] = 0
        enc = init_cmd.encode("ascii")
        sysd[INIST_OFF:INIST_OFF + len(enc)] = enc
        sysd[INIST_OFF + len(enc)] = 0x00
        wr(DIR_TRACK, SYS_SECTOR, sysd)

    open(outf, "wb").write(bytes(raw))
    print("injecte %s.%s : load $%04X-$%04X exec $%04X, %d secteurs (desc t%d s%d), "
          "free=%d files=%d%s"
          % (name.strip(), ext.strip(), load, end, execaddr, total, desc_t, desc_s,
             free, files, (" INIST=%r" % init_cmd) if init_cmd else ""))


if __name__ == "__main__":
    main()
