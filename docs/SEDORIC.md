# Format disque SEDORIC & outillage

> Référence primaire : manuel **SEDORIC 3.0** (`sedna3_0.pdf`), recoupé avec
> A. Chéramy « SEDORIC 3.0 à NU » et l'outillage du projet SCUMM-Oric / scoop-oric.
> **Rien n'est inventé** : les offsets ci-dessous sont ceux du manuel, vérifiés
> byte-exact contre le dump de descripteur donné en exemple par le manuel lui-même.

## 1. Conteneur MFM_DISK

En-tête 256 o : `"MFM_DISK"` + `sides` (u32 LE) + `tracks` (u32 LE) + geometry (u32 LE).
Puis un bloc de piste de **6400 o** par (face, piste), dans l'ordre **SIDE-MAJOR** :
toutes les pistes de la face 0, puis toutes celles de la face 1
(`bloc = side * tracks + track` ; cf `src/storage/sedoric.c`, `track_idx = s*tracks+t`).

Chaque bloc de piste : gap initial `60×$4E`, puis par secteur
`12×$00 | A1 A1 A1 FE [trk side sec size] CRC | 22×$4E | 12×$00 | A1 A1 A1 FB [256 data] CRC | 38×$4E`,
padding `$4E` jusqu'à 6400 o. CRC-16/CCITT (poly `$1021`, init `$FFFF`) sur les
4 octets de marque + les données.

Une image **RAW** (secteurs 256 o concaténés, même ordre side-major, sans cadrage)
est acceptée par `sedoric-info` et produite par `oric1-emu --disk-create` /
`tools/sedoric_inject.py`. `tools/dsk_raw2mfm.py` convertit RAW → MFM_DISK.

## 2. Secteurs système (piste 20)

| Secteur | Rôle | Champs clés |
|---|---|---|
| **s1** Système | nom disque + autoexec | `+9..+29` nom disque (21 o) · `+0x1E..+0x59` **INIST** (60 o : commandes ASCII de boot, séparées par `:`, terminées par `#00`) |
| **s2** VTOC/bitmap | compteurs | `+2,+3` secteurs libres (LE) · `+4,+5` nb de fichiers (LE) |
| **s4** Directory | catalogue | `+0,+1` lien vers secteur dir suivant (0=fin) · `+2` high-water mark · entrées de 16 o à partir de `+16` |

Entrée directory (16 o) : `name[9]` `ext[3]` `track` `sector` `nsec` `status`
(V4 fichier valide = `$40` ; bit 7 = supprimé).

> Note (constatée sur disques Master, cf `analyze_sedoric_vtoc.py`) : le compteur
> `free`/`files` de la VTOC peut être un **template 80 pistes** plaqué sur une image
> physique plus petite — c'est un modèle, pas un état recalculé. `sedoric-info`
> rapporte donc à la fois le compteur VTOC et le nombre de fichiers réellement
> parcourus dans le catalogue.

## 3. Secteur descripteur de fichier

Pointé par l'entrée directory (`track`,`sector`). **Dump réel du manuel** (descripteur
du fichier système BANQUE n°7, p.16) :

```
 0 1 2 3 4 5 6 7 8 9 A B C D E F
00 00 FF 40 00 C4 FF C7 00 00 04 00 05 0B 05 0C 05 0D 05 0E 00 00
```

| Offset | Champ | Exemple |
|---|---|---|
| `+0,+1` | lien vers descripteur suivant (00 00 = aucun) | `00 00` |
| `+2` | marqueur premier descripteur | `FF` |
| `+3` | **type** : b0=AUTO, b6=bloc data, b7=BASIC → `$40`=ML, `$41`=AUTO ML | `40` |
| `+4,+5` | adresse de chargement (LE) | `00 C4` = $C400 |
| `+6,+7` | adresse de fin (LE) | `FF C7` = $C7FF |
| `+8,+9` | **adresse d'exécution si AUTO** (LE) | `00 00` |
| `+0xA,+0xB` | **nombre de secteurs data** (LE) | `04 00` = 4 |
| `+0xC..` | carte des secteurs data `(track,sector)×n`, terminée `00 00` | `05 0B 05 0C 05 0D 05 0E 00 00` |

Ce dump **corrige** l'ancienne implémentation de `tap2sedoric` qui laissait `+3=0`
(type absent) et écrivait le nombre de secteurs en `+9,+10` **big-endian**
(chevauchant l'adresse d'exécution). Corrigé depuis v1.64.0.

## 4. Outils

| Outil | Rôle |
|---|---|
| `tap2sedoric <in.tap> -o out.dsk -b base.dsk [-n NAME.EXT] [-a] [-e EXEC] [-i "INIST"]` | injecte un `.tap` CSAVE dans un disque MFM Sedoric (fichier + descripteur conforme + entrée dir + VTOC ; `-a`/`-e` AUTO, `-i` autoexec de boot) |
| `sedoric-info <disk.dsk> [--check FREE:FILES]` | inspecte VTOC, nom disque, INIST, catalogue et descripteurs décodés ; `--check` = garde de régression sur les compteurs |
| `tools/sedoric_inject.py <raw_in> <bin> <load> NAME.EXT <raw_out> [tracks] [sectors] [init] [exec]` | injection en RAW (offsets directs) |
| `tools/dsk_raw2mfm.py <raw> <out.dsk> [order] [sides] [tracks] [sectors]` | RAW → MFM_DISK (blocs side-major) |

Chaîne RAW type : `oric1-emu --disk-create base.raw` (puis INIT au boot pour une
vraie VTOC) → `sedoric_inject.py` → `dsk_raw2mfm.py` → `oric1-emu --disk-rom microdis.rom -d`.

## 5. Injection multi-fichiers (limite levée)

Historiquement, `tap2sedoric` **et** `sedoric_inject.py` allouaient toujours à
partir de la piste 21 secteur 1 sans consulter les secteurs déjà occupés : une
**deuxième injection sur le même disque plaçait son descripteur au même secteur
que la première et l'écrasait** (le fichier précédent disparaissait de `LOADM`).

Depuis v1.64.0, les deux outils **parcourent le catalogue (chaîne complète) et
les cartes de secteurs des descripteurs existants** — y compris les **secteurs
catalogue chaînés eux-mêmes** — pour marquer les secteurs occupés avant
d'allouer. Deux injections successives obtiennent des descripteurs distincts.
Vérifié par `make test-sedoric-tools` (`tests/integration/test_sedoric_inject.sh`).

### Chaînage de secteurs directory (implémenté)

Quand le secteur de catalogue courant est plein (15 entrées), les outils
**parcourent la chaîne** via le lien `+0,+1` ; si toute la chaîne est pleine, ils
**allouent un secteur libre, l'initialisent en catalogue vierge et l'y chaînent**.
Les secteurs catalogue étant localisés **par lien piste/secteur** (pas par zone
fixe, manuel ANNEXE 7), un catalogue chaîné peut résider sur n'importe quel
secteur libre. **Validé in-situ** : un fichier posé dans le 2ᵉ secteur catalogue
(y compris injecté *après* la création de ce secteur) est listé par `DIR` et
chargé+exécuté par `LOAD"NAME"` dans l'émulateur.

## 6. Exécuter un fichier ML sous Sedoric (recette **validée in-situ**)

Chargement/exécution vérifiés dans l'émulateur (boot bare + `LOAD` → `$5000`
chargé et code exécuté). Pièges rencontrés et tranchés :

| Commande / cas | Résultat |
|---|---|
| taper `PROBE` (nom nu) au Ready | `?SYNTAX ERROR` — le nom nu ne lance qu'un **BASIC** AUTO (cf `MENU`) |
| `LOADM"PROBE"` | `?TYPE MISMATCH` — `LOADM` est la commande **ROM cassette**, pas Sedoric |
| `CLOAD"PROBE"` | pas d'erreur mais **ne charge rien** (`CLOAD`/`CLOAD,J` = fichiers BASIC) |
| `LOAD"PROBE"` sur `.BIN` | `?FILE NOT FOUND` — **`.COM` est l'extension par défaut** de `LOAD` |
| `LOAD"PROBE",J` sur AUTO | `BREAK ON BYTE #5000` — `,J` entre en conflit avec le flag AUTO |
| **`LOAD"PROBE"`** sur **`.COM` AUTO (type $41)** | **charge ET exécute** (le flag AUTO saute à l'adresse d'exécution) ✅ |

Recette : injecter en **`.COM` AUTO** (`tap2sedoric ... -n NAME.COM -a -e EXEC`), puis
au Ready : **`LOAD"NAME"`**. La commande Sedoric est **`LOAD`** (auto-détecte
BASIC vs binaire via l'octet de type `+3`), avec options `,A` (adresse) et `,J`
(jump), cf manuel §VSALO1 (`C04E` : b6=`,A`, b7=`,J`).

### Master Sedoric « nu » bootable (déterministe)

Pour valider un `.COM` maison, il faut un disque qui boote au `Ready` **sans
application concurrente**. `tools/make_bootable_sedoric.sh` (INIT piloté par
`--type-keys`) est **fragile** (échoue si le disque système maître boote sur un
menu, ex. `SEDO40u`). Méthode robuste, sans timing, via l'outil dédié :

```
tools/sedoric_mkbare.py disks/SEDO40u.DSK bare.dsk        # neutralise l'INIST
tools/sedoric_mkbare.py disks/SEDO40u.DSK auto.dsk 'LOAD"PROBE"'   # ou autolance
```

Il **neutralise (ou remplace) l'INIST** (piste 20 secteur 1, `+0x1E..+0x59`, CRC
du secteur MFM refait ; RAW aussi géré) → le disque tombe en `SEDORIC V4.0 /
Ready` nu tout en restant bootable. Vérifié sur `SEDO40u` (INIST
`CLS:MENU.LNG:MENU` → Ready nu).

> Note VTOC : `DIR` d'un tel Master affiche `D/80/17` (template 80 pistes) sur
> une image physique de 42 pistes — cohabitation sûre tant que les fichiers
> injectés restent dans les pistes physiques (cf `analyze_sedoric_vtoc`).

### Charger un fichier depuis du code machine (recette **validée in-situ**)

Depuis un programme ML en cours d'exécution (lancé par `LOAD`), l'**overlay RAM
Sedoric n'est pas mappé** : appeler directement les routines DOS (`$DB2D`
recherche, `$E0EA` lecture) **plante** (elles tombent dans la ROM BASIC). La voie
robuste passe par le **vecteur « ! » `$0467`** (en RAM basse `$04xx`, toujours
mappé), qui bascule sur l'overlay, exécute l'**interpréteur SEDORIC** (`$D3AE`)
sur la ligne de commande pointée par **TXTPTR (`$00E9/$00EA`)**, puis rebascule
sur la ROM et fait `RTS` (manuel : vecteur `!` en `$0467`, stub de bascule
`$0477` → `STA $0314`).

```asm
        LDA #<CMD : STA $E9        ; TXTPTR = adresse de la ligne de commande
        LDA #>CMD : STA $EA
        JSR $0467                  ; vecteur "!" : execute la commande (overlay gere)
        ; ... fichier charge ; poursuite normale ...
        RTS
CMD:    .byte "LOAD\"SCDATA\"", 0  ; ligne SEDORIC terminee par $00
```

C'est l'équivalent ML de taper `!LOAD"SCDATA"`. **Validé** : `LOAD"SCDATA"`
appelé ainsi charge le fichier (`$6000..` = données exactes), le programme
appelant reprend normalement. La séquence bas-niveau `$DB2D` (recherche →
POSNMP `$C025`/POSNMS `$C026`/POSNMX `$C027`) puis `$E0EA` (lecture selon
POSNMX/VSALO1 `$C04E`/DESALO `$C052`) est réelle mais exige de gérer soi-même la
bascule overlay via `$0477` — préférer `$0467`.

Rappel adresses (manuel) : BUFNOM `$C028` drive + `$C029` nom(9) + `$C032`
ext(3) ; XDEFLO `$DFE6` ; recherche `$DB2D` ; lecture `$E0EA` ; TXTPTR
`$00E9/$00EA` ; TIB (tampon clavier) `$0035`-`$0084`.
