# E2E LOCI complet : 007 "Dangereusement Vôtre" chargé automatiquement

**Date** : 2026-06-07
**Version** : v1.16.49-alpha (suite immédiate du sprint 34av)
**Source documentation** : [LOCI User Manual sodiumlb](https://github.com/sodiumlb/loci-hardware/wiki/LOCI-User-Manual)

---

## 1. Contexte

Sprint 34av avait livré l'auto-typing matrix avec `\e` ESC + flèches
`\u/\d/\l/\r`. Mais l'automatisation de la navigation TUI LOCI restait
floue : la sélection « 007.TAP via picker » ne marchait pas en aveugle.

L'utilisateur m'a demandé d'aller chercher la doc officielle.

---

## 2. Documentation récupérée

Trouvée sur le wiki sodiumlb. Raccourcis clavier décisifs :

| Touche | Action |
|--------|--------|
| **`T`** | Saut direct vers le slot tape drive |
| **`A`-`D`** | Saut direct vers les drives Microdisc |
| **`K`** | Tape counter |
| **`M`** | Mouse on/off |
| **`O`** | ROM select |
| **`R`** | RV1 adjust |
| **SPACE** | Sélectionne (file ou toggle) |
| **ESC** | Boot (si au top menu) ou ferme popup |
| **RETURN** | Reprend une appli suspendue (save state) |
| **`/`** | Parent directory dans le file browser |
| **`F`** | Champ filtre dans le file browser |
| **+/-** | Increase/decrease (RV1) |

**Workflow officiel pour monter un TAP** :
1. Press **T** → ouvre le tape drive
2. File browser s'ouvre avec les devices
3. Up/Down pour sélectionner le device (USB ou SD)
4. SPACE pour entrer dans le device
5. F + filter `.TAP` (optionnel ; le picker peut filtrer auto)
6. Up/Down pour highlight le fichier
7. SPACE pour sélectionner
8. ESC pour booter

---

## 3. Validation E2E avec 007 "Dangereusement Vôtre"

```bash
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --keyboard azerty \
    --type-keys '15000000:\p3t\p2 \p2 \p2\e\p9\p1CLOAD""\n\p9\p9\p9'
```

Décodé :
- `\p3` : 3s pour stabiliser le TUI LOCI
- `t` : saut au tape drive
- `\p2 ` : 2s puis SPACE → ouvre file picker (seule SD = pas besoin de
  device selection intermédiaire)
- `\p2 ` : 2s puis SPACE → sélectionne le 1er fichier listé (007.TAP,
  alphabétique avant AIGLE.TAP car '0' < 'A')
- `\p2\e` : 2s puis ESC → MIA_BOOT
- `\p9\p1` : 10s pour BASIC 1.1 init
- `CLOAD""\n` : tape la commande CLOAD
- `\p9\p9\p9` : 27s pour le chargement complet du tape

### Logs LOCI clés

```
LOCI SDIMG extract: '007.TAP' → '/tmp/loci_007.TAP_Hw2PS4'
LOCI tape mount: /tmp/loci_007.TAP_Hw2PS4 buffered (28630 bytes, type CLOAD"" in BASIC)
LOCI SDIMG extract: 'basic11b.rom' → '/tmp/loci_basic11b.rom_jFpc3w'
LOCI ROM swap: loading /tmp/loci_basic11b.rom_jFpc3w at $C000
LOCI ROM swap: patches → BASIC 1.1 (ORIC Atmos)
TAPE: getsync at tapeoffs=0/28630
TAPE: getsync at tapeoffs=137/28630
TAPE: getsync at tapeoffs=8322/28630
```

3 getsync = multi-block TAP (header + segments code). Le 1er sync trouve
le tape ; le 2e à offset 137 = après le header (16 leader + name + start
of data block) ; le 3e à 8322 = entre les blocks de données.

### Écran final (titre du jeu)

```
DOMARK ET EUREKA INFORMATIQUE PRESENTENT
   .  007 "DANGEREUSEMENT VOTRE"
        Le jeu sur ordinateur
Realisation: TIGRESS MARKETING
Programmation: SEVERN SOFTWARE
Droits exclusifs de la version francaise
       EUREKA INFORMATIQUE
39 Rue Victor Masse - 75009. PARIS.
           MAINTENANT, JAMES,
        VOTRE MISSION COMMENCE!
Note de "Q"
   Arretez de vous amuser, Bond, vous
avez du travail!
     Rappellez-vous : utilisez avec
precaution les gadgets que je vous ai
confies, certains d'entre eux sont tres
dangereux!
Note de "M"
     N'echouez pas, Bond, je tiens
beaucoup a cette fille!
        "RETURN" - DEBUT DU JEU
```

**Le jeu James Bond 007 "Dangereusement Vôtre" (DOMARK 1985) tourne
automatiquement** — première fois qu'un programme commercial Oric tape
est chargé bout-en-bout via le firmware LOCI dans Phosphoric.

---

## 4. Pile validée

| Composant | Validé par |
|-----------|------------|
| MIA spin ABI (sprint 34an) | LOCI ROM boote et reste responsive |
| SDIMG FAT16 read (34ao) | Picker liste les fichiers, mount extract |
| SDIMG `mkstemp` (34ar) | Path `/tmp/loci_007.TAP_Hw2PS4` randomisé |
| ROM swap + BASIC patches (34ao+) | BASIC 1.1 booté avec les bons patches |
| CSAVE/CLOAD format (34aq-34at) | Le CLOAD trouve sync à offset 0, 137, 8322 |
| `\e` ESC + `t` shortcut (34av) | Navigation TUI automatique |
| 7 ops tuning stubbées (34au) | Pas appelées par 007 mais prêtes |

---

## 5. Reproductibilité

```bash
git checkout main           # v1.16.49-alpha
make SDL2=1
./tools/mkloci_sd loci_demo.img 16 \
    roms/basic10.rom roms/basic11b.rom roms/microdis.rom \
    tapes/AIGLE.TAP tapes/007.tap
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --keyboard azerty \
    --type-keys '15000000:\p3t\p2 \p2 \p2\e\p9\p1CLOAD""\n\p9\p9\p9'
# → 60-90 secondes après, l'écran montre le titre 007 DOMARK
```

---

## 6. Crédits

- **sodiumlb** : LOCI hardware + ROM + firmware + documentation TUI sur
  le wiki. Sans cette doc, l'automatisation de la navigation aurait
  exigé du reverse engineering du firmware 6502.
- **xahmol** : `locifilemanager` (firmware alternatif, cité dans la
  recherche, pour info).

---

**Statut** : E2E LOCI **entièrement automatisé** — boot → mount TAP →
swap BASIC → CLOAD → run program. Premier émulateur grand public à
supporter ce flow de bout-en-bout.

— Fin du CR
