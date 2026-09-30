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
SYS_SECTOR = 1          # System Sector (disk name, INIST)
VTOC_SECTOR = 2         # BITMAP (free / file count)
DIR_SECTOR = 4          # Directory
INIST_OFF = 0x1E        # INIST offset in the System Sector
INIST_MAX = 60          # max INIST length


def main():
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    raw = bytearray(open(sys.argv[1], "rb").read())
    data = open(sys.argv[2], "rb").read()
    # The SEDORIC loader loads whole sectors: pad to a multiple of 256
    # so the whole binary gets loaded (no partial last sector lost).
    if len(data) % SECSZ:
        data += b"\x00" * (SECSZ - len(data) % SECSZ)
    load = int(sys.argv[3], 16)
    namespec = sys.argv[4].upper()
    outf = sys.argv[5]
    tracks = int(sys.argv[6]) if len(sys.argv) > 6 else 42
    sectors = int(sys.argv[7]) if len(sys.argv) > 7 else 17
    init_cmd = sys.argv[8] if len(sys.argv) > 8 else ""
    exec_given = len(sys.argv) > 9              # exec given => AUTO file
    execaddr = int(sys.argv[9], 16) if exec_given else load

    nm, _, ex = namespec.partition(".")
    name = (nm[:9] + " " * 9)[:9]
    ext = (ex[:3] + " " * 3)[:3]

    def off(track, side, sec):              # side-major: block = side*tracks+track
        return ((side * tracks + track) * sectors + (sec - 1)) * SECSZ

    def rd(track, sec):
        o = off(track, 0, sec)
        return raw[o:o + SECSZ]

    def wr(track, sec, buf):
        o = off(track, 0, sec)
        raw[o:o + SECSZ] = (bytes(buf) + b"\x00" * SECSZ)[:SECSZ]

    # --- map of already-used sectors (multi-file safe): walk the
    #     catalogue then the sector maps of the existing descriptors ---
    # ROBUSTNESS (bounds checking): the catalogue's (track,sector) pointers may
    # carry Sedoric's SIDE FLAG (bit 7 of the track byte: track 131 =
    # 0x83 = side 1, track 3) or be out-of-image leftovers. rd() only reads
    # side 0; following such a pointer returns a truncated sector and raises an
    # IndexError. So every access is BOUNDED, like build_game_disk._catalog_used
    # (side-1 files are not allocated here: allocation stays on
    # side 0 from track 21 onwards, so ignoring them is safe).
    def ok(t, s):
        return 0 <= t < tracks and 1 <= s <= sectors
    used = set()
    dt, ds, guard = DIR_TRACK, DIR_SECTOR, 0
    while guard < 64 and ok(dt, ds):
        guard += 1
        dirs = rd(dt, ds)
        used.add((dt, ds))                   # the catalogue sector itself
        for e in range(16, SECSZ, 16):
            if dirs[e] == 0 and dirs[e + 15] == 0:
                continue
            if dirs[e + 15] & 0x80:          # deleted
                continue
            cdt, cds, dg, first = dirs[e + 12], dirs[e + 13], 0, True
            while dg < 64 and ok(cdt, cds):
                dg += 1
                desc = rd(cdt, cds)
                used.add((cdt, cds))         # the descriptor sector itself
                p = 12 if first else 2       # map from +0x0C (1st) or +0x02 (continuation)
                while p + 1 < SECSZ:
                    if desc[p] == 0 and desc[p + 1] == 0:
                        break
                    if ok(desc[p], desc[p + 1]):
                        used.add((desc[p], desc[p + 1]))
                    p += 2
                if desc[0] == 0 and desc[1] == 0:
                    break
                cdt, cds = desc[0], desc[1]
                first = False
        if dirs[0] == 0 and dirs[1] == 0:
            break
        dt, ds = dirs[0], dirs[1]

    end = load + len(data) - 1
    ndata = (len(data) + SECSZ - 1) // SECSZ

    # Chained descriptors (validated in situ, see docs/SEDORIC.md): the 1st
    # descriptor carries the header (12 bytes) then the (track,sector) map from +0x0C
    # (122 pairs max); each following descriptor = link +0,+1 then map from
    # +0x02 (127 pairs max). The +0,+1 link points to the next descriptor.
    FIRST_CAP = (SECSZ - 12) // 2          # 122 pairs in the 1st descriptor
    CONT_CAP = (SECSZ - 2) // 2            # 127 pairs per following descriptor
    ndesc = 1 if ndata <= FIRST_CAP else 1 + -(-(ndata - FIRST_CAP) // CONT_CAP)
    total = ndesc + ndata                  # descriptors + data sectors
    if total > 255:
        sys.exit("fichier trop gros : %d secteurs > 255 (nsec directory sur 1 octet)" % total)

    # --- allocate total sectors from track 21, side 0, skipping used sectors ---
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
    desc_secs = alloc[:ndesc]
    data_secs = alloc[ndesc:]
    desc_t, desc_s = desc_secs[0]

    # AUTO ($41) as soon as an exec address is given (or an INIST):
    # required for `LOAD"NOM"` to load AND execute the file (see docs §6).
    is_auto = bool(init_cmd) or exec_given

    # --- descriptor sectors (chained) ---
    idx = 0
    for di in range(ndesc):
        d = bytearray(SECSZ)
        if di == 0:
            d[2] = 0xFF                    # first descriptor
            d[3] = 0x41 if is_auto else 0x40   # b6=data block, b0=AUTO
            d[4] = load & 0xFF; d[5] = (load >> 8) & 0xFF
            d[6] = end & 0xFF;  d[7] = (end >> 8) & 0xFF
            if is_auto:
                d[8] = execaddr & 0xFF; d[9] = (execaddr >> 8) & 0xFF
            d[10] = ndata & 0xFF; d[11] = (ndata >> 8) & 0xFF
            p, cap = 12, FIRST_CAP
        else:
            p, cap = 2, CONT_CAP           # following descriptor: link then map from +0x02
        n = min(cap, len(data_secs) - idx)
        for (dt2, ds2) in data_secs[idx:idx + n]:
            d[p] = dt2; d[p + 1] = ds2; p += 2
        idx += n
        if di < ndesc - 1:
            d[0], d[1] = desc_secs[di + 1]    # link -> next descriptor
        elif p + 1 < SECSZ:
            d[p] = 0; d[p + 1] = 0            # terminator on the last descriptor
        wr(desc_secs[di][0], desc_secs[di][1], d)

    # --- data sectors ---
    for i, (dt2, ds2) in enumerate(data_secs):
        wr(dt2, ds2, data[i * SECSZ:(i + 1) * SECSZ])

    # --- directory entry (track 20 sec 4) ---
    # Walk the chain of catalogue sectors (t20 s4 -> link +0,+1 -> ...)
    # to find a free slot; if the whole chain is full, allocate a free
    # sector and chain it (catalogue sectors are located by
    # track/sector link, not by a fixed area -- SEDORIC 3.0 manual, ANNEXE 7).
    # cat_t/cat_s ALWAYS stays on a VALID catalogue sector on side 0: a chain
    # link is only followed if it is in bounds (see note above: the
    # chain of a double-sided master may continue on side 1 = track >= 128).
    cat_t, cat_s, slot, new_cat, guard = DIR_TRACK, DIR_SECTOR, None, False, 0
    while guard < 64:
        guard += 1
        dirs = bytearray(rd(cat_t, cat_s))
        nent = sum(1 for e in range(16, SECSZ, 16) if dirs[e] != 0 or dirs[e + 15] != 0)
        s = 16 + nent * 16
        if s + 16 <= SECSZ:
            slot = s
            break
        nxt_t, nxt_s = dirs[0], dirs[1]
        if (nxt_t == 0 and nxt_s == 0) or not ok(nxt_t, nxt_s):
            break            # end of chain (or link out of image/side 1): a sector will be added
        cat_t, cat_s = nxt_t, nxt_s
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
        dirs[0], dirs[1] = ncat            # previous link -> new
        wr(cat_t, cat_s, dirs)
        dirs = bytearray(SECSZ)            # new blank catalogue sector
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

    # --- VTOC (track 20 sec 2): free -= total (+1 if new cat. sector), files += 1 ---
    v = bytearray(rd(DIR_TRACK, VTOC_SECTOR))
    free = (v[2] | v[3] << 8) - total - (1 if new_cat else 0)
    files = (v[4] | v[5] << 8) + 1
    v[2] = free & 0xFF; v[3] = (free >> 8) & 0xFF
    v[4] = files & 0xFF; v[5] = (files >> 8) & 0xFF
    wr(DIR_TRACK, VTOC_SECTOR, v)

    # --- INIST (autoexec): track 20 sec 1, offset 0x1E, ASCII + #00 ---
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
