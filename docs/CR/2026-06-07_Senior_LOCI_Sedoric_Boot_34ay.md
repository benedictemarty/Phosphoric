# Senior Engineering Review — LOCI Sedoric boot stage 1 (sprints 34aw → 34ay)

**Date** : 2026-06-07
**Versions livrées** : v1.16.50 → v1.16.53-alpha (4 versions)
**Auteur** : bmarty (avec assistance Claude Opus 4.7)
**Demande** : review architecturale du bus DSK LOCI + diagnostic du blocage Sedoric stage 2

---

## 1. TL;DR

Cette série rebranche complètement le bus DSK (Microdisc) sur LOCI :
parsing MFM_DISK, WD1793 driven par le `fdc_t` cycle-accurate existant,
overlay ROM Microdisc activé dynamiquement, et IRQ asynchrone propagé au
CPU quand le firmware sodiumlb le poll.

**Résultat E2E** : depuis `roms/loci/locirom`, le pipeline complet
fonctionne :

```
LOCI MIA_BOOT(FDC) → swap ROM $C000 (basic11b) + overlay $E000 (microdis)
                  → re-reset CPU → boot Microdisc → Restore + Read Sector $01/Track 20
                  → boot loader Sedoric chargé en RAM $0400-$04FF
                  → JMP $0400, exécution loader, écran affiche « Booting. »
                  → STUCK à PC=$04F7 (stage 2 du loader)
```

Le stage 1 (transfert ROM Microdisc → RAM Sedoric loader) est **réussi**.
Le stage 2 (charge du noyau Sedoric depuis le loader RAM) bloque, hypothèse
principale : Read Multiple ou DRQ pacing manquant.

| PR | Sprint | Version | Files | LOC |
|----|--------|---------|-------|-----|
| 1 | 34aw | 1.16.50 | rebranchement `fdc_t` dans loci, MFM_DISK parser | ~400 |
| 2 | 34ax | 1.16.52 | CTRL register sémantique (INTENA/ROMDIS/EPROM) | ~200 |
| 3 | 34ay | 1.16.53 | INTRQ asynchrone → cpu_irq | ~30 |

484 tests pass (118 LOCI + 366 core), 0 régression.

---

## 2. Contexte amont

### Avant cette série

- Sprint 34av avait livré le `--type-keys loci-hid:` prefix : navigation TUI
  LOCI automatisable via le HID stack USB synthétique. La sélection d'un
  disque dans la TUI passait `Espace` (pas `Return`, surprise utilisateur
  documentée).
- LOCI ROM acceptait `MIA_BOOT(FDC)` mais le bus DSK n'avait qu'un stub :
  CTRL/STATUS/SECTOR/DATA en RAM, pas de Microdisc réel derrière. Donc
  Sedoric ne pouvait pas démarrer.

### Décision architecturale clé

La question initiale : *réimplémenter un WD1793 dans `loci.c` ou réutiliser
le `fdc_t` existant de `src/storage/disk.c` ?*

J'ai choisi **réutiliser**. Justification :

| Argument | Choix retenu |
|----------|--------------|
| Le `fdc_t` est déjà cycle-accurate, level-triggered, 412 LOC battle-tested via `test-storage` | ✓ Évite la duplication ; cohérence avec Microdisc natif |
| Le format disque côté LOCI est MFM_DISK (header `MFM_DISK` 256 bytes + tracks MFM raw) — différent de l'image flat attendue par `fdc_t` | Réutiliser le `sedoric_load()` existant pour parser MFM_DISK → array flat, puis injecter dans `fdc_t` |
| LOCI a son propre layout I/O ($0310-$031F partagé Microdisc/LOCI) | Bridger via callbacks `dsk_cpu_irq_set/clr` + `dsk_sync_overlay` |

**Conséquence** : le bus DSK LOCI n'est plus un stub mais un vrai WD1793.
Trade-off : couplage fort avec `storage/disk.c`. Acceptable vu que le format
disque est fixé par la ROM Microdisc (un seul standard à supporter).

---

## 3. Architecture livrée

### 3.1 Sprint 34aw — Rebranchement `fdc_t`

```c
/* include/io/loci.h */
typedef struct loci_s {
    /* ... */
    fdc_t    dsk_fdc;                  /* WD1793 cycle-accurate */
    uint8_t* dsk_image[4];             /* 4 drives, flat sector arrays */
    uint32_t dsk_image_size[4];
    uint8_t  dsk_tracks[4];
    uint8_t  dsk_sectors[4];
    uint8_t  dsk_intrq;                /* Active-low (0x00 asserted, 0x80 idle) */
    bool     dsk_intena;               /* CTRL bit 0 */
    /* ... */
} loci_t;
```

`dsk_open(drive, path)` lit le fichier, détecte `MFM_DISK` (memcmp 8 bytes),
appelle `sedoric_load()` qui retourne un array flat
`sides * tracks * sectors * 256`. SEDO40U.DSK = 696 320 bytes, 80 tracks,
17 sectors, 2 sides.

Reads $0310-$0317 sont routés vers `fdc_read(&l->dsk_fdc, reg)` ; writes
vers `fdc_write()`. Le DRQ/INTRQ du `fdc_t` est ponté vers `loci.dsk_drq`
et `loci.dsk_intrq` via callbacks.

**Convention active-low** : tous les flags exposés au CPU sont OR'd avec
`0x7F` (DRQ/INTRQ utilisent uniquement le bit 7, les 7 bits bas sont
floating sur le bus réel). C'est ce que la ROM Microdisc attend.

### 3.2 Sprint 34ax — Sémantique CTRL $0314

```
Bit 0 : INTENA   — enable IRQ vers CPU
Bit 1 : ROMDIS   — disable BASIC ROM ($C000-$DFFF)
Bit 3 : DENSITY  — single/double (informational, MFM only)
Bit 4 : SIDE     — head select
Bit 5-6 : DRIVE  — 0-3
Bit 7 : EPROM    — Microdisc overlay $E000 (toggle pendant boot)
```

Write CTRL synchronise immédiatement via 3 callbacks :

```c
void loci_set_dsk_bus_callbacks(loci_t* l,
    dsk_irq_cb irq_set,    /* cpu_irq_set(IRQF_DISK) */
    dsk_irq_cb irq_clr,    /* cpu_irq_clear(IRQF_DISK) */
    dsk_sync_cb sync_overlay,  /* memory.basic_rom_disabled + overlay_active */
    void* ctx);
```

**Piège évité** : un essai initial où `sync_overlay` mettait
`basic_rom_disabled = false` selon ROMDIS a régressé en remplaçant le ROM
Microdisc par le BASIC mid-exécution → écran « insert system disc » de
retour. Fix : `basic_rom_disabled` reste persistant `true` après MIA_BOOT,
seul `overlay_active` (EPROM bit) suit dynamiquement.

### 3.3 Sprint 34ay — IRQ asynchrone

Diagnostic : après Restore puis Read Sector, le `fdc_t` met
`delayed_int = 20` (cycles). À expiration, `fdc_ticktock()` appelle
`set_intrq()`. Mon premier code n'asseyait `cpu_irq_set` que dans le
handler write CTRL — donc l'IRQ asynchrone fired par le FDC n'atteignait
jamais le CPU. La ROM Microdisc poll $0314 en attendant le bit 7 = 0 et
tournait infiniment.

```c
static void loci_fdc_set_intrq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (!l) return;
    l->dsk_intrq = 0x00;
    if (l->dsk_intena && l->dsk_cpu_irq_set) {
        l->dsk_cpu_irq_set(l->dsk_bus_ctx);
    }
}
static void loci_fdc_clr_intrq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (!l) return;
    l->dsk_intrq = 0x80;
    if (l->dsk_cpu_irq_clr) {
        l->dsk_cpu_irq_clr(l->dsk_bus_ctx);
    }
}
```

`IRQF_DISK` est level-triggered → set/clr symétriques.

**Effet observé** : « Booting. » s'affiche, CPU sort du ROM Microdisc et
exécute en RAM (`PC=$04F7` au timeout). Stage 1 réussi.

---

## 4. État actuel — Stage 2 stuck

E2E reproductible :

```bash
./oric1-emu -r roms/loci/locirom --loci \
    --loci-sdimg loci_demo.img --keyboard azerty \
    --type-keys '15000000:\p3a\p2 \p2 \p2\e\p9\p9\p9' \
    --dump-ram-at 56000000:/tmp/screen.bin
```

Sur l'écran texte $BB80 :
```
14: Booting.
```
Et c'est tout. CPU finit à `PC=$04F7 A:06 X:BA Y:04 SP:AB`.

### Hypothèses pour le blocage

1. **Read Multiple non implémenté** : le loader Sedoric pourrait émettre
   $9C (Read Multiple) au lieu de $80 (Read Sector) pour charger le noyau.
   Le `fdc_t` ne traite explicitement que Read Single. À vérifier dans
   `src/storage/disk.c:fdc_write()` opcode dispatch.
2. **DRQ pacing trop rapide** : le `fdc_t` actuel propose le byte suivant
   immédiatement après lecture de DATA. Sedoric V4 pourrait avoir une
   séquence stricte avec attente d'IRQ entre bytes (peu probable mais à
   éliminer).
3. **Mauvais sector layout** : MFM_DISK déclare 17 sectors/track mais
   Sedoric V4.0 attend peut-être un layout interleaved que `sedoric_load`
   met à plat de façon naïve. À vérifier en dumpant le sector 1/track 20
   et en le comparant à un boot loader Sedoric connu.

### Diagnostics suggérés

- Trace `--trace dsk.log` filtré sur les opcodes FDC envoyés après PC=$0400
- Lire `mem[$0400-$04FF]` au moment du stop : si c'est bien le loader
  Sedoric, désassembler et identifier la boucle à $04F7
- Comparer à Oricutron : booter le même `SEDO40U.DSK` en flat (sans LOCI)
  via `--disk-rom roms/microdis.rom -d SEDO40U.DSK`. Si ça boote là mais
  pas via LOCI, c'est le pont LOCI→FDC qui régresse.

---

## 5. Tests

| Suite | Avant 34aw | Après 34ay |
|-------|------------|------------|
| test-loci | 115 | 118 (+3 WD1793 dsk tests) |
| test-storage | 8 | 8 (régression test_dsk_drq mis à jour pour `0x7F` mask) |
| Total | 481 | 484 |

Aucune régression hors test_loci_dsk_drq_register (attente mise à jour
pour le nouveau masque actif-bas).

---

## 6. Questions ouvertes pour le senior

1. **Stratégie stage 2** : aller plus loin avec instrumentation (trace
   ciblé loader Sedoric) ou rebrancher LOCI sur le path Microdisc natif
   (`microdisc_t`) plutôt que `fdc_t` directement ? Microdisc native a
   plus de plumbing (overlay register $0314 propre, IRQ retire-après-RTI).
2. **Vendor d'un Sedoric loader binaire de référence** : pour validation
   E2E déterministe, dumper un loader connu correct (depuis Oricutron par
   exemple) et comparer aux 256 bytes du secteur 1/track 20 lus par LOCI.
   Est-ce dans le périmètre légal du projet ?
3. **`fdc_t` Read Multiple** : si confirmé manquant, l'ajouter au
   `fdc_t` core (et profite à Microdisc native aussi) ou shimmer dans
   `loci.c` ?

---

## 7. Acceptance critères proposés pour sprint 34az

- Sedoric V4.0 affiche `SEDORIC V4.0 — (C) 1987` puis prompt READY
- `DIR` liste le contenu de SEDO40U.DSK
- 484 tests + tests Sedoric V4 boot E2E (un nouveau test-loci-sedoric)
- Pas de régression Microdisc native (`./oric1-emu --disk-rom -d` existant)

---

*Branch :* `sprint-34ay`
*Commits clés :* f967224 (34ay), précédents pour 34aw/34ax
*Reproductible :* commande E2E section 4
