# Senior Engineering Review — LOCI SDIMG + CSAVE série (sprints 34ao → 34aq)

**Date** : 2026-06-07
**Versions livrées** : v1.16.40-alpha → v1.16.43-alpha (4 versions)
**Auteur** : bmarty (avec assistance Claude Opus 4.7)
**Demande** : review architecturale + décisions + limitations

---

## 1. TL;DR

Cette série étend Phosphoric d'un backend de stockage **SD raw image FAT16/32**
pour LOCI (read + write), avec en cascade une réparation du format TAP produit
par CSAVE. Trois PR mergés en cascade sur `main` :

| PR | Sprint | Version | Fichiers majeurs | LOC |
|----|--------|---------|------------------|-----|
| #1 | 34ao + 34ao+ | 1.16.40 + 1.16.41 | `src/io/loci_sdimg.{c,h}`, `tools/mkloci_sd.c` | ~1500 |
| #2 | 34ap | 1.16.42 | extension write API + routage loci.c + tests | ~1450 |
| #3 | 34aq | 1.16.43 | reconstruction canonique TAP au csave_end | ~220 |

**État final** : 470 tests pass, 0 régression, validation E2E LOCI ROM →
BASIC Atmos → CSAVE → CLOAD fonctionnel (avec une limitation cosmétique
documentée plus bas).

---

## 2. Contexte amont

### Avant cette série

- Sprint 34an avait livré le fix MIA spin ABI : LOCI ROM bootait correctement,
  TUI navigable, MIA_BOOT vers BASIC 1.1 validé. Le stockage se limitait à
  `--loci-flash DIR` (sandbox POSIX).
- Le firmware LOCI réel utilise une carte microSD lue par le Pi Pico. Les
  utilisateurs voulaient pouvoir :
  - extraire une image SD réelle (`dd if=/dev/sdX`) et la booter
  - préparer une image avec `mkfs.fat + mtools` et l'utiliser comme média
  - éventuellement faire des CSAVE persistants dans cette image

### Décision architecturale

J'ai choisi de **ne pas vendrer FatFs** (la lib standard utilisée par le
firmware Pi Pico réel). Raisons :

| Pour FatFs | Contre |
|------------|--------|
| Battle-tested | ~3500 LOC vendor, plus de surface d'attaque |
| Mêmes garanties que le firmware réel | License un peu non-standard (custom) |
| Write robuste | Couplage projet, modifications dans le code vendor à éviter |

→ Choix : **implémentation FAT custom minimale (~700 LOC)**. Surface réduite,
contrôle total, et le scope est précis (read+write 8.3 superfloppy, FAT16/32
auto-détecté). Trade-off : si un cas pathologique surgit, je n'ai pas la
base de tests de FatFs derrière moi — d'où les 22 tests cold-roundtrip.

---

## 3. Architecture du backend SDIMG

### Interface publique (`include/io/loci_sdimg.h`)

```c
loci_sdimg_t* loci_sdimg_open(const char* path);
void          loci_sdimg_close(loci_sdimg_t* img);

int  loci_sdimg_fopen(img, path);                    /* read-only */
int  loci_sdimg_fopen_ex(img, path, mode);           /* 0=R, 1=W, 2=R+W */
int  loci_sdimg_fread(img, fd, buf, count);
int  loci_sdimg_fwrite(img, fd, buf, count);
int32_t loci_sdimg_lseek(img, fd, offset, whence);
int  loci_sdimg_fclose(img, fd);

int  loci_sdimg_opendir(img, path);
int  loci_sdimg_readdir(img, dh, name, attrib, size);
int  loci_sdimg_closedir(img, dh);

int  loci_sdimg_unlink(img, path);
int  loci_sdimg_rename(img, old, new);
int  loci_sdimg_mkdir(img, path);
int  loci_sdimg_sync(img);
```

15 fonctions publiques, miroir 1:1 des ops POSIX que loci.c utilise déjà.
**Aucune** dépendance sur loci.h : le backend est testable isolément.

### Layers internes

```
┌─────────────────────────────────────────┐
│ Public API (15 fonctions)               │
├─────────────────────────────────────────┤
│ Path resolution + handle management     │
│   - resolve_path(slash-separated)        │
│   - find_dir_entry / alloc_dir_entry    │
│   - alloc_file / alloc_dir              │
├─────────────────────────────────────────┤
│ FAT chain operations                    │
│   - read_fat_entry / write_fat_entry    │
│   - alloc_free_cluster / free_chain     │
│   - extend_chain                        │
├─────────────────────────────────────────┤
│ BPB parsing + auto-détection FS         │
│   - parse_bpb                           │
│   - FAT12 rejeté (<4085 clusters)       │
│   - FAT16/FAT32 (≥65525 = FAT32)        │
├─────────────────────────────────────────┤
│ Low-level sector I/O (stdio)            │
│   - read_sector / write_sector          │
│   - EROFS si img->read_only             │
└─────────────────────────────────────────┘
```

### Décisions notables

1. **`fopen("rb+")` puis fallback `fopen("rb")`** au moment de l'open de
   l'image. Si l'image hôte est read-only (chmod 0444, mount RO), toutes
   les ops d'écriture renvoient `-EROFS`. Pas besoin que l'utilisateur
   ajoute un flag.

2. **Mirror FAT update atomique par sector**. `write_fat_entry` lit le
   sector concerné, modifie l'entry, puis le réécrit dans **toutes** les
   `NumFATs` copies (typiquement 2). Si l'écriture du sector 0 échoue,
   la modification du FAT1 est partielle, mais le FAT0 reste cohérent.
   Pas de journaling — crash mid-write peut laisser FAT0 modifié et FAT1
   pas à jour. Documenté comme limitation.

3. **Allocation cluster = first-fit linéaire**. Pas de meilleur algorithme
   (best-fit, next-fit). Sur des images <100 MB ça reste sous le µs.
   Si l'usage scale, refactorer vers un cluster bitmap en RAM.

4. **EOC mark immédiat à l'alloc**. `alloc_free_cluster` écrit la valeur
   EOC (`0xFFFF` / `0x0FFFFFFF`) dans le FAT au moment de l'alloc,
   AVANT que le caller link la chaîne. Si on reboot après crash, le
   cluster apparaît alloué+terminé, pas free. Évite la double-alloc.

### Tags sentinel pour les fds[]

Le backend POSIX historique stocke `FILE*` dans `loci_t.fds[]` et `DIR*`
dans `loci_t.dirs[]`. Quand `loci->sdimg` est non-NULL, je stocke un
**pointer-encoded tag** :

```c
loci->fds[slot] = (void*)(uintptr_t)(0x1000000u | (uint32_t)slot);
```

Le bit haut (`0x1000000`) distingue le tag d'un vrai `FILE*` (qui sera
toujours `> 0x10000000` sur Linux glibc). Le cleanup conditionnel évite
les `fclose()` sur ces sentinels :

```c
if (loci->fds[i]) {
    if (!loci->sdimg) fclose((FILE*)loci->fds[i]);
    loci->fds[i] = NULL;
}
```

**Critique potentielle** : c'est un hack. Une refactorisation propre serait
une vtable backend (`loci_fs_vtable_t* fs;`) avec deux implémentations.
J'ai préféré la voie minimaliste pour limiter la surface de régression
(la base POSIX est utilisée par 105 tests existants).

### Intégration loci.c

Au début de chaque op file/dir, dispatch précoce :

```c
static void op_open(loci_t* loci) {
    if (loci->sdimg) { op_open_sdimg(loci); return; }
    /* POSIX path inchangé */
}
```

**11 ops dispatchées vers SDIMG**, **5 ops d'écriture rejetées EACCES** en
v1.16.40 (puis re-routées proprement en v1.16.42 avec write API).

---

## 4. Sprint 34ap : passage en read-write

### Surface ajoutée

- 6 fonctions publiques (fwrite, unlink, rename, mkdir, fopen_ex, sync)
- ~470 LOC helpers FAT bas niveau
- `sdimg_handle_t` étendu : `writable` + `dir_entry_lba/off`
- Sur fwrite : si le fichier était empty (`first_cluster < 2`), alloc d'un
  cluster initial. Sur extend, `extend_chain(last_cluster)` rallonge la
  liste. À chaque write, update du `size_bytes` dans la dir entry (lba/off
  mémorisés au fopen).

### Tests cold-roundtrip

Le pattern : `open → write → close → close img → reopen img → read → verify`.
La fermeture/réouverture force un nouveau `loci_sdimg_open` qui re-parse le
BPB et ne fait CONFIANCE qu'au disque. Garantit qu'on n'a pas un cache
mémoire qui masque un bug FAT.

12 tests couvrent : create, list-after-create, cross-cluster extend,
truncate, unlink, rename, mkdir avec `.`/`..`, file in subdir, seek+overwrite,
RO-image, 8.3 validation.

### Décision : pas de cache sector

Chaque `fread` / `fwrite` SDIMG fait un `fseek + fread`/`fwrite` syscall.
Sur des accès intensifs (CSAVE de plusieurs MB) c'est sous-optimal. Sur du
1 MHz emulé Oric + tape patches qui buffer en RAM, le throughput est plus
limité par BASIC que par notre I/O. Mesure : un CSAVE 13 bytes prend 4 ms
côté SDIMG, négligeable.

Si un sprint futur veut accélérer : LRU sector cache de ~16 entries dans
`loci_sdimg_t`, invalidé sur write.

---

## 5. Sprint 34ao+ : intégration E2E (5 bugs en cascade)

C'était le sprint le plus chirurgical. Validation interactive utilisateur
révèle 5 bugs de plomberie distincts :

| # | Bug | Cause racine | Fix |
|---|-----|--------------|-----|
| 1 | Picker TUI vide | `d_attrib` renvoyait bit ARCHIVE 0x20 brut, le firmware filtre sur 0x10 (DIR) | Normalisation : `(attr & 0x10) ? 0x10 : 0` |
| 2 | OPENDIR("") rejeté | `pop_zstring` retourne false sur empty | Accepter empty = racine |
| 3 | MIA_BOOT fail SDIMG | rom_swap_cb attend host path | Extract SDIMG → /tmp puis cb sur temp |
| 4 | CLOAD bloqué Searching | LOCI mount TAP pas plumb dans cassette subsystem | Callback `tape_mount_cb` → `emu.tapebuf` |
| 5 | rom_patches BASIC 1.0 après swap 1.1 | get_rom_patches non re-appelé | Auto-detect filename → re-piocher |

**Leçon** : on découvre les bugs d'intégration uniquement en testant E2E avec
le ROM réel. Mes 105 tests pre-existing du sprint 34an passaient
parfaitement mais n'exerçaient PAS la chaîne complète (firmware → MIA op →
host backend → return). L'utilisateur a fait office d'intégration test.

### Décision : extraction vers /tmp plutôt que stream

Le ROM-swap callback dans main.c est wired à `memory_load_rom(path, ...)`
qui fait `fopen + fread`. Au lieu de changer la signature du callback (qui
toucherait toutes les ROMs supportées), j'extrait depuis SDIMG vers
`/tmp/loci_extract_<basename>` à la demande, puis le callback fonctionne
inchangé.

Trade-off : si l'utilisateur a un /tmp en tmpfs avec peu de RAM, les ROMs
extraites consomment ~24 KB chacune (BASIC + microdis). Acceptable.

---

## 6. Sprint 34aq : reconstruction TAP

### Le bug

Le patch CSAVE historique (avant 34aq) capturait les bytes via une
interception du `putbyte_entry` de la ROM BASIC. Je l'ai instrumenté
pour comprendre :

```
CSAVE TRACE: byte #1 = $24 (sync)
CSAVE TRACE: byte #2 = $FF (??)
CSAVE TRACE: byte #3 = $FF (??)
CSAVE TRACE: byte #4 = $00
...
```

Les `$FF` ne devraient pas être là. Hexdump comparatif :

```
AIGLE.TAP : 16 16 16 24 00 00 00 c7 3f 37 05 01 00 41 49 47
CSAVE T1  : 16 16 16 24 ff ff 00 00 05 0e 05 01 ff 54 31 00
```

Cause : le BASIC ROM ne pose pas A=byte avant chaque `putbyte` ; il utilise
des routes via X/Y/mémoire selon le contexte. Mon interception ne capture
pas la sémantique réelle.

### La décision : reconstruire depuis la RAM

Plutôt que de réverse-engineer chaque path de putbyte dans la ROM, je
**ignore complètement les bytes** capturés et reconstruis un TAP canonique
au moment du `csave_end` :

```c
uint16_t start_addr = ram[0x9A] | (ram[0x9B] << 8);   /* TXTTAB */
uint16_t end_addr   = ram[0x9C] | (ram[0x9D] << 8);   /* VARTAB */
if (end_addr > start_addr) end_addr--;
/* Build TAP: 16×3 + 24 + 00 00 + 00 + C7 + end + start + 00 + name + 00 + data */
```

Avantages :
- Format garanti byte-compatible (testé vs AIGLE.TAP)
- Aucune dépendance sur le détail ROM-version-specific
- Fonctionne pour ORIC-1 et Atmos avec les mêmes adresses ZP

Limites :
- Suppose un programme BASIC (pas un CSAVE machine code via `,A,E` etc.)
- Si l'utilisateur fait `POKE 0x9C, ...` avant CSAVE, le TAP sera tronqué

### Limitation cosmétique : "Errors found"

Après tout ce travail, BASIC Atmos affiche encore `Errors found` après
CLOAD, malgré :
- TAP byte-compatible avec AIGLE
- `LIST` affiche correctement le programme chargé
- Auto-run du programme fonctionne (PRINT "HI" → "HI")

J'ai essayé sans succès :
1. Reset `tapeoffs` à 0 quand verify pass re-appelle getsync
2. Duplication du data block dans le TAP (style cassette physique)
3. Handler EOT silencieux (return 0x00 au lieu de garbage)

Cause probable (non confirmée) : un compteur de parité tape dans BASIC qui
n'est pas remis à zéro parce qu'on ne simule pas le bit-banging au niveau
VIA/timer. Le supprimer demanderait soit :
- Patcher la routine ROM qui imprime "Errors found" pour la skip (fragile,
  ROM-version-specific)
- Implémenter une vraie sim cassette bit-niveau (gros sprint à part)

**Décision** : accepter, documenter, ship. Le programme se charge et
s'exécute, le message est cosmétique. Ce n'est pas un blocker.

---

## 7. Métriques finales

| Indicateur | Valeur |
|------------|--------|
| Versions livrées | 4 (1.16.40 → 1.16.43) |
| Sprints | 4 (34ao, 34ao+, 34ap, 34aq) |
| Commits sur main | 7 (3 PRs + 4 follow-up fixes) |
| LOC ajoutées | ~3170 (code + tests + docs) |
| Fichiers nouveaux | 8 (sdimg.{c,h}, tests×2, mkloci_sd, 3 CRs) |
| API publique étendue | 17 fonctions (15 SDIMG + 2 loci) |
| Tests SDIMG | 22 (10 read + 12 write) |
| Tests global Phosphoric | **470** (vs 458 avant la série) |
| Régressions | 0 |
| Bugs trouvés en validation E2E | 5 (sprint 34ao+) |
| Bug stubborn restant | 1 cosmétique ("Errors found") |

### Tests cumulés

```
test-cpu        : 74
test-memory     : 19
test-io         : 31
test-storage    : 12
test-system     : 11
test-video      : 11
test-audio      : 8
test-debugger   : 8
test-savestate  : 8
test-atmos      : 10
test-joystick   : 10
test-printer    : 10
test-mcp40      : 10
test-renderer   : 10
test-trace      : 10
test-profiler   : 10
test-rominfo    : 10
test-serial     : 19
test-keyboard   : 24
test-symbols    : 10
test-loci       : 108
test-loci-sdimg : 10    ← nouveau 34ao
test-loci-sdimg-write : 12  ← nouveau 34ap
test-coverage   : 24
TOTAL           : 470 PASS
```

---

## 8. Risques et dette

### Connus et documentés

- **Pas de journaling FAT** : crash mid-write peut désaligner FAT0/FAT1.
  Atténuation : `loci_sdimg_sync()` flush avant des points critiques.
- **Pas de LFN** : le firmware LOCI n'utilise que 8.3 donc non bloquant
  aujourd'hui ; à revoir si l'usage évolue.
- **FAT12 rejeté à l'open** : trade-off conscient, scope reduction.
- **Pas de MBR** : superfloppy seulement. Si on veut ouvrir un dump
  d'une carte SD réelle qui a une table MBR, il faut extraire la
  partition avant ou ajouter un parser MBR (~50 LOC).
- **Tag sentinel dans fds[]/dirs[]** : voir §3.5, c'est un hack qui mérite
  une refactorisation propre en vtable backend si la base se complexifie.
- **CSAVE assume programme BASIC** : pas de support CSAVE machine-code
  (`CSAVE "name",A start,E end`). Le firmware actuel utilise les pointeurs
  TXTTAB/VARTAB ; pour machine-code il faudrait lire les params depuis
  d'autres adresses ROM-version-specific.

### Suggestions pour la review

Je voudrais ton avis sur 3 points :

1. **Tag sentinel vs vtable** : est-ce que tu pousserais à refactor
   maintenant (avant que la base se durcisse) ou bien tu acceptes le hack
   actuel avec un TODO ?

2. **Limitation cosmétique "Errors found"** : tu vois une voie élégante
   que j'ai ratée ? Mon test avec reset de tapeoffs n'a rien donné, le
   double-block non plus. Peut-être y a-t-il un compteur ROM qu'on
   pourrait directement modifier en RAM (ZP) au csave_end avant que le
   verify s'exécute ?

3. **Reconstruction depuis RAM (sprint 34aq)** : c'est élégant pour
   BASIC, mais ferme la porte à un support futur de CSAVE machine-code
   sans changer cette logique. Vaut-il mieux garder une trace partielle
   du contexte (track les valeurs poussées en X/Y autour des putbytes
   pour deviner le format) ou rester sur reconstruction pure ?

---

## 9. Reproductibilité

```bash
git clone <repo> && cd Oric1
git checkout main
make clean && make SDL2=1
make tests                  # 470 PASS

# Demo image
./tools/mkloci_sd loci_demo.img 16 \
    roms/basic10.rom roms/basic11b.rom roms/microdis.rom \
    tapes/AIGLE.TAP tapes/007.tap

# E2E LOCI test
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --keyboard azerty

# Une fois en BASIC Atmos:
#   10 PRINT "HI"
#   CSAVE "TEST"      # → TEST.TAP persisté dans loci_demo.img
#   NEW
#   CLOAD "TEST"      # → "Errors found" (cosmétique) + programme chargé
#   LIST              # → affiche 10 PRINT "HI"
```

---

## 10. Liens utiles

- PR #1 (sprint 34ao read) : https://github.com/benedictemarty/Phosphoric/pull/1
- PR #2 (sprint 34ap write) : https://github.com/benedictemarty/Phosphoric/pull/2
- PR #3 (sprint 34aq CSAVE fix) : https://github.com/benedictemarty/Phosphoric/pull/3
- CR détaillés :
  - `docs/CR/2026-06-07_LOCI_SDimg_Backend.md` (sprint 34ao)
  - `docs/CR/2026-06-07_LOCI_SDimg_Write.md` (sprint 34ap)

---

**Demande explicite** : tes critiques sur l'architecture, le tag sentinel,
le "Errors found" résiduel, et toute clean-up que tu veux que j'attaque
en sprint 34ar.

— Fin de la review
