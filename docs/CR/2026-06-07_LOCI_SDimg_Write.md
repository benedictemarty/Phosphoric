# Compte-rendu — Sprint 34ap : SDIMG read-write 2026-06-07

**Auteur** : bmarty (avec assistance Claude Opus 4.7)
**Branche** : `feat/loci-sdimg-write`
**Version livrée** : v1.16.42-alpha
**Statut** : Tests automatisés ✅ — Validation E2E LOCI à faire

---

## 1. Motivation

Le sprint 34ao livrait un backend SDIMG read-only. Cela permettait au
LOCI ROM de lire des fichiers depuis l'image SD mais bloquait toute
opération d'écriture (CSAVE, MKDIR, UNLINK, RENAME). Ce sprint complète
le backend avec un support FAT16/32 write.

---

## 2. API ajoutée

```c
/* Open existing (mode 0=R) or create/truncate (mode 1=W, 2=R+W). */
int  loci_sdimg_fopen_ex(loci_sdimg_t* img, const char* path, int mode);

/* Write at current cursor; extends file + allocates clusters as needed. */
int  loci_sdimg_fwrite(loci_sdimg_t* img, int fd,
                       const void* buf, uint16_t count);

/* Delete file: free chain, mark entry deleted (0xE5). */
int  loci_sdimg_unlink(loci_sdimg_t* img, const char* path);

/* Rename (same dir → in-place; cross-dir → alloc+delete). */
int  loci_sdimg_rename(loci_sdimg_t* img,
                       const char* old_path, const char* new_path);

/* Create directory with "." and ".." entries. */
int  loci_sdimg_mkdir(loci_sdimg_t* img, const char* path);

/* Flush host I/O buffer. */
int  loci_sdimg_sync(loci_sdimg_t* img);
```

---

## 3. Architecture

### Helpers bas niveau

| Helper | Rôle |
|--------|------|
| `write_sector` | atomic per-sector write, early EROFS si image RO |
| `write_fat_entry` | update sur les NumFATs copies du miroir FAT |
| `alloc_free_cluster` | first-fit search + EOC mark immédiat |
| `free_cluster_chain` | libère toute la chain en partant du first |
| `extend_chain` | alloc + link last → new |
| `find_dir_entry` | retourne (lba, off) pour update in-place |
| `alloc_dir_entry` | trouve slot libre, étend cluster du dir si plein |
| `to_fat_83` | normalisation host basename → 8.3 packed (uppercase, chars interdits) |
| `update_dir_entry` | rewrite size + first_cluster fields |
| `write_new_entry` | crée nouvelle entry (attrib, cluster, size) |
| `mark_entry_deleted` | first byte → 0xE5 |

### Extension de `sdimg_handle_t`

```c
typedef struct {
    /* existing fields ... */
    bool     writable;            /* opened in W or R+W mode */
    uint32_t dir_entry_lba;       /* location of dir entry, for size updates */
    uint32_t dir_entry_off;
} sdimg_handle_t;
```

Le `dir_entry_lba/off` est rempli au moment de l'open (via lookup parent
dir → find_dir_entry) et utilisé à chaque fwrite pour persister la
nouvelle taille du fichier.

### Open semantics

```c
FILE* fp = fopen(path, "rb+");
if (!fp) {
    fp = fopen(path, "rb");
    if (fp) img->read_only = true;   // fallback à read-only
}
```

Si l'image hôte est read-only (mode 0444 ou montage RO), toutes les ops
d'écriture renvoient `-EROFS` au lieu de planter ou corrompre l'image.

---

## 4. Routing dans loci.c

| Op | Avant 34ap | Après 34ap |
|----|------------|------------|
| `op_open` avec O_CREAT/O_TRUNC/RDWR | EACCES | `fopen_ex(mode≥1)` |
| `op_write_xstack` | EACCES | `fwrite` depuis xstack |
| `op_write_xram` | EACCES | `fwrite` depuis fenêtre xram |
| `op_unlink` | EACCES | `loci_sdimg_unlink` |
| `op_rename` | EACCES | `loci_sdimg_rename` |
| `op_mkdir` | EACCES | `loci_sdimg_mkdir` |

Le mapping des modes LOCI vers les modes SDIMG :
```c
int rw = flags & LOCI_O_RDWR;     /* bits 0-1 */
if ((flags & write_flags) || rw == 1 || rw == 3) {
    mode = (rw == 3) ? 2 : 1;     /* RDWR → 2, sinon W=1 */
}
```

---

## 5. Tests

`tests/unit/test_loci_sdimg_write.c` — 12 tests cold-roundtrip :

| # | Test | Vérifie |
|---|------|---------|
| 1 | open_blank_image_is_writable | Pas de RO accidentel sur image fraîche |
| 2 | create_small_file_round_trip | Create + write + close + reopen + read |
| 3 | file_listed_in_root_after_create | Dir entry visible après close |
| 4 | write_across_cluster_boundary | Extend chain sur 5000 bytes (>1 cluster) |
| 5 | truncate_existing_via_fopen_ex_w | fopen(W) free old chain |
| 6 | unlink_removes_file_and_frees_clusters | Re-create possible après unlink |
| 7 | rename_in_same_dir | Update name in place |
| 8 | mkdir_creates_subdir_with_dot_entries | Cluster init avec . et .. |
| 9 | create_file_in_subdir | Lookup parent + extend dir cluster |
| 10 | write_then_seek_overwrite | Read-modify-write au milieu du fichier |
| 11 | read_only_image_rejects_writes | chmod 0444 → EROFS sur toutes les ops |
| 12 | invalid_83_name_rejected | "TOOLONG.TXT" + char '*' rejetés |

### Pattern cold-roundtrip

Chaque test fait : `open → write → close+reopen` (nouvelle instance
`loci_sdimg_t*`) → `read` pour vérifier que les données ont bien
atterri sur disque (pas juste dans une cache mémoire).

---

## 6. Validation

```bash
$ make test-loci-sdimg-write
  [12] invalid_83_name_rejected                                PASS
  Results: 12 passed, 0 failed (total: 12)

$ make tests
# 470 tests global, 0 fail
```

Compte total :
- Avant 34ap : 458 tests
- Après 34ap : **470** (+12 SDIMG write)

---

## 7. Limitations restantes

| Limite | Notes |
|--------|-------|
| Pas de support LFN | Le firmware LOCI utilise les noms 8.3 — non bloquant |
| FAT12 rejeté | Image < 4 MB → utiliser FAT16 |
| Pas de MBR | Image doit être superfloppy (BPB à sector 0) |
| Pas de ftruncate | Trunc passe par `fopen(W)` |
| Pas de cache | 1 fread/fwrite par sector — OK à 1 MHz CPU |
| Pas de journaling | Crash en cours d'écriture peut laisser FAT incohérente |
| Cross-dir rename | Implémenté comme alloc+delete (pas atomique) |

---

## 8. Métriques

| Indicateur | Valeur |
|------------|--------|
| LOC ajoutées dans loci_sdimg.c | ~470 |
| LOC ajoutées dans loci.c | ~110 |
| LOC test write | ~370 |
| API publique étendue | 6 fonctions |
| Tests SDIMG total | 22 (10 read + 12 write) |
| Tests global Phosphoric | 470 |
| Régression | 0 |
| Sprint dans la série LOCI | 16e (34y → 34ap) |
| Version bumped | 1.16.41 → 1.16.42 |

---

## 9. Reproductibilité

```bash
git clone <repo> && cd Oric1
git checkout feat/loci-sdimg-write
make clean && make SDL2=1
make test-loci-sdimg-write  # 12 PASS
make tests                  # 470 PASS

# Démo écriture E2E (image existante du sprint 34ao)
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img
# (en BASIC après MIA_BOOT)
> 10 PRINT "HELLO"
> CSAVE "TEST"
> NEW
> CLOAD "TEST"
> LIST   # devrait afficher 10 PRINT "HELLO"
```

---

## 10. À faire pour le prochain sprint

- Validation E2E interactive : CSAVE depuis BASIC → vérifier que le
  fichier apparaît dans le picker LOCI à la prochaine session
- Optionnel : ajouter un cache sector LRU (~16 sectors) pour réduire
  les fread/fwrite en cas d'écriture séquentielle intensive
- Optionnel : support partition MBR (parse table à sector 0 + offset
  vers la première partition active)

---

**Statut** : Sprint 34ap livré côté tests automatisés. Validation E2E
interactive (CSAVE + RELOAD) à confirmer à la prochaine session.

— Fin du CR
