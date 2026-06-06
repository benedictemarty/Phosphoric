# Compte-rendu — Sprint 34ao : Backend image SD raw LOCI 2026-06-07

**Auteur** : bmarty (avec assistance Claude Opus 4.7)
**Branche** : `feat/loci-sdimg`
**Version livrée** : v1.16.40-alpha
**Statut** : Implémentation read-only complète, 458 tests pass

---

## 1. Motivation

Le sprint précédent (34an) a livré LOCI end-to-end, mais le stockage
côté hôte se limitait au sandbox POSIX (`--loci-flash DIR`). Sur le
vrai matériel LOCI, le stockage est une **microSD lue par le Pi Pico**
avec un système de fichiers FAT. Cette différence empêche de tester
des images SD réelles (extraites par `dd`) ou des images générées
par des outils standards (`mkfs.fat`, `mtools`).

L'objectif de ce sprint : ajouter un backend `--loci-sdimg PATH.img`
qui parse une image disque raw au format FAT16/32 et expose ses
fichiers aux ops LOCI File I/O, sans toucher au code du firmware
LOCI ROM côté 6502.

---

## 2. Architecture livrée

### Module isolé

```
include/io/loci_sdimg.h    (~70 LOC, API publique)
src/io/loci_sdimg.c        (~470 LOC, parseur FAT16/32 read-only)
tests/unit/test_loci_sdimg.c (~250 LOC, 10 tests + générateur FAT16)
```

Aucune dépendance externe : pas de FatFs, pas de libfat. Implémentation
custom adaptée à notre usage spécifique (read-only, 8.3, superfloppy).

### Surface d'attaque sur loci.c

Minimaliste pour limiter le risque de régression :
- 1 champ ajouté : `void* sdimg` dans `loci_t`
- 11 ops dispatchent vers SDIMG quand `loci->sdimg` est non-NULL
- 5 ops d'écriture rejettent EACCES proprement
- 2 fonctions publiques : `loci_attach_sdimg` / `loci_detach_sdimg`

### Mapping handles

Le backend SDIMG utilise des slots internes (0..15 pour fichiers,
0..7 pour dirs). Pour éviter les collisions avec le backend POSIX,
on stocke un **tag sentinel non-pointeur** dans `fds[i]` / `dirs[i]` :
- `(void*)(0x1000000u | slot)` pour les fichiers
- `(void*)(0x2000000u | slot)` pour les dirs

Le cleanup (`loci_cleanup`) skip `fclose`/`closedir` quand
`loci->sdimg` est actif — le détachement libère les vrais handles
internes du backend.

---

## 3. Implémentation FAT

### Auto-détection FS

Règle Microsoft FAT spec (taille fixe, pas de magic) :
```c
if (count_of_clusters < 4085)        rejeté (FAT12 unsupported)
else if (count_of_clusters < 65525)  → FAT16
else                                  → FAT32
```

### BPB parsing (sector 0)

| Offset | Champ | Usage |
|--------|-------|-------|
| 11 | bytes_per_sector | en général 512 |
| 13 | sectors_per_cluster | |
| 14 | reserved_sectors | offset FAT1 |
| 16 | num_fats | en général 2 |
| 17 | root_entries | FAT16 seulement |
| 19 | total_sectors_16 | fallback 16 bits |
| 22 | fat_size_16 | FAT16 |
| 32 | total_sectors_32 | si total16 = 0 |
| 36 | fat_size_32 | FAT32 |
| 44 | root_cluster | FAT32 seulement |

### FAT chain walk

`read_fat_entry(cluster)` :
- FAT16 : `entry = u16[FAT_start + cluster*2]`, EOC ≥ 0xFFF8
- FAT32 : `entry = u32[FAT_start + cluster*4] & 0x0FFFFFFF`, EOC ≥ 0x0FFFFFF8

### Directory iteration

Chaque entrée fait 32 octets. Le scan skip :
- octet 0x00 → fin de répertoire
- octet 0xE5 → entrée supprimée
- attribut 0x0F (LFN) → long filename slot
- attribut 0x08 (VOLUME_ID) → label de volume

Reconstitution du nom 8.3 → `"NAME.EXT"` (extension omise si vide).

### Lookup case-insensitive

`ci_strcmp` compare en majuscules. Convention adoptée :
- Path d'entrée : libre (`hello.txt` ok)
- Représentation FAT : 8.3 uppercase fixed-width
- Sortie de `readdir` : "NAME.EXT" format normalisé

---

## 4. Mapping errno

POSIX → LOCI :

| POSIX | LOCI | Cas |
|-------|------|-----|
| ENOENT | LOCI_ENOENT (1) | fichier introuvable |
| EACCES | LOCI_EACCES (3) | tentative d'écriture |
| EISDIR | LOCI_EACCES (3) | open() sur un dir |
| ENOTDIR | LOCI_EINVAL (7) | opendir() sur un file |
| EBADF | LOCI_EBADF (16) | fd invalide |
| EMFILE | LOCI_EMFILE (5) | table de fds pleine |
| EIO | LOCI_EIO (11) | I/O bas niveau échouée |
| autre | LOCI_EIO | défaut conservateur |

---

## 5. Tests

10 tests dans `tests/unit/test_loci_sdimg.c` :

| # | Test | Vérifie |
|---|------|---------|
| 1 | open_image_detects_fat16 | Auto-détection FS + total_size |
| 2 | open_nonexistent_fails | NULL si fichier absent |
| 3 | opendir_root_lists_entries | Énumération root, attributs corrects |
| 4 | fopen_read_hello | Read complet d'un petit fichier |
| 5 | fopen_case_insensitive | "hello.txt" ↔ "HELLO.TXT" |
| 6 | fopen_nested_path | "SUB/INSIDE.BIN" cross-directory |
| 7 | fopen_missing_returns_enoent | Errno correct |
| 8 | lseek_set_cur_end | 3 modes de seek |
| 9 | opendir_subdir_lists_inside | Listing subdir + end-of-dir |
| 10 | fopen_bad_handle_close | EBADF sur fd invalide |

### Générateur d'image FAT16 inline

Pour éviter de commiter un binaire en git, le test génère une image
FAT16 minimale à la volée dans `/tmp/loci_sdimg_test_<PID>.img` :
- 4 secteurs BPB + 2 FATs de 32 secteurs
- Root dir avec "HELLO.TXT" (13 bytes), "SUB" (dir), label volume
- "SUB" → "INSIDE.BIN" (4 bytes : `DE AD BE EF`)
- Pad à 8000 secteurs (~4 MB) pour atteindre seuil FAT16

Cleanup en fin de main(), même en cas de fail.

---

## 6. Validation

```bash
$ make tests
# 458 tests, 0 fail
$ make test-loci-sdimg
Test image: /tmp/loci_sdimg_test_364040.img
  [1] open_image_detects_fat16                           PASS
  [2] open_nonexistent_fails                             PASS
  [3] opendir_root_lists_entries                         PASS
  [4] fopen_read_hello                                   PASS
  [5] fopen_case_insensitive                             PASS
  [6] fopen_nested_path                                  PASS
  [7] fopen_missing_returns_enoent                       PASS
  [8] lseek_set_cur_end                                  PASS
  [9] opendir_subdir_lists_inside                        PASS
  [10] fopen_bad_handle_close                            PASS
  Results: 10 passed, 0 failed (total: 10)
$ make test-loci
  Results: 108 passed, 0 failed (total: 108)  # aucune régression
```

---

## 7. Usage end-to-end

### Créer une image SD utilisable

```bash
# 16 MB de FAT16 vide
dd if=/dev/zero of=sdcard.img bs=1M count=16
mkfs.fat -F 16 -n LOCI sdcard.img

# Y pousser des fichiers (mtools sans /etc/mtools.conf)
MTOOLSRC=/dev/null mcopy -i sdcard.img roms/basic11b.rom ::/BASIC11.ROM
MTOOLSRC=/dev/null mcopy -i sdcard.img tapes/asteroids.tap ::/AST.TAP

# Lancer LOCI dessus
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg sdcard.img
```

### Compatibilité avec une vraie SD Pi Pico

```bash
# Copier depuis une vraie microSD LOCI
sudo dd if=/dev/sdX of=loci_real.img bs=1M status=progress
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_real.img
```
→ utile pour débugger un comportement observé sur le vrai hardware
sans risquer la carte physique.

---

## 8. Limitations connues

| Limite | Workaround | Roadmap |
|--------|------------|---------|
| Read-only | Préparer l'image côté hôte avec `mtools` | Sprint 34ap envisagé : write + FAT alloc |
| Pas de LFN | Renommer en 8.3 (FILE1.TXT) | Future si demande |
| Pas de MBR | Image doit être superfloppy | Future si demande |
| FAT12 rejeté | Utiliser FAT16 (image ≥ ~4 MB) | Pas prévu |
| Pas de cache | OK pour Oric (1 MHz) | Bench avant d'optimiser |

---

## 9. Métriques

| Indicateur | Valeur |
|------------|--------|
| LOC ajoutées | ~790 (sdimg.c + .h + test + intégration) |
| Tests ajoutés | 10 SDIMG |
| Tests total Phosphoric | 458 (vs 448 précédent) |
| Régression | 0 |
| Ops file/dir SDIMG | 11 (read) + 5 rejetées (write) |
| Sprint dans la série LOCI | 15e (34y → 34ao) |
| Bumped version | 1.16.39-alpha → 1.16.40-alpha |

---

## 10. Reproductibilité

```bash
git clone <repo> && cd Oric1
git checkout feat/loci-sdimg
make clean && make SDL2=1
make test-loci-sdimg   # 10 PASS
make tests             # 458 PASS

# Demo image rapide
dd if=/dev/zero of=demo.img bs=1M count=16
mkfs.fat -F 16 demo.img
MTOOLSRC=/dev/null mcopy -i demo.img roms/basic11b.rom ::/BASIC11.ROM
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg demo.img
```

---

**Statut** : Sprint 34ao livré, validation tests automatisés ✅.
Validation interactive sur LOCI ROM réel : à faire lors du prochain
boot E2E (le ROM LOCI demandera des fichiers via FOPEN/OPENDIR, qui
seront servis depuis l'image FAT au lieu du sandbox POSIX).

— Fin du CR
