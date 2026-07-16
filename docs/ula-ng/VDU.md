# ULA-NG — VDU intégré (design v0.1)

> **Statut : DOCUMENT DE DESIGN** (à valider avant implémentation). Décrit un
> port de commandes de style VDU exposé par l'ULA-NG, dont l'interpréteur et la
> mémoire de travail vivent **dans l'ULA-NG** — pas dans les 64 Ko du 6502.

## 1. Objectif

Piloter l'ULA-NG par un **flux d'octets de commande** (comme on `PRINT CHR$(…)`)
au lieu d'écritures de registres brutes, et loger l'**interpréteur + les buffers**
dans la mémoire de l'ULA-NG. Le 6502 ne porte **aucun code pilote** : il *stream*
des octets, l'ULA-NG interprète.

## 2. État de l'art (référence de conception, ~2026)

Ce modèle « coprocesseur d'affichage piloté par flux VDU, à mémoire propre » est
le courant dominant des machines 8-bit modernes et des ré-implémentations FPGA :

- **Agon Light — VDP** : le CPU (eZ80) envoie un flux **VDU** (lignée BBC BASIC)
  à un ESP32 qui exécute un firmware vidéo avec **sa propre mémoire** ; modes,
  palette, sprites, bitmaps, audio. Notion clé reprise ici : les **« buffered
  commands »** (uploader des données dans la mémoire du VDP, réutilisables par
  ID). → *la référence directe de ce document*.
- **VERA (Commander X16)** : puce vidéo FPGA moderne, interface **registres +
  data-port à auto-incrément**, sprites, couches, palette. → valide l'idiome
  data-port que l'ULA-NG utilise déjà (attributs/sprites en streaming).
- **OCULA-GPU (ce projet)** : fenêtre de commandes `$03E8-$03EF` (INFO, FILL,
  COPY, SCROLL, WAIT_VBL) interprétées **dans l'ULA**. → précédent maison direct
  du pattern « port de commandes + interpréteur ».
- **Copper / display-list** (héritage Amiga) : déjà présent (ULA-NG §5.4).

**Choix d'architecture qui en découle** (cible FPGA Tang Primer 20K / GW2A-18) :
*split hard/soft*. Le **datapath temps réel** (fetch scanline, palette, sprites,
copper) reste en **RTL dur** ; l'**interpréteur VDU** tourne en **firmware sur
soft-core RISC-V** (le GW2A peut en héberger un) ou sur le MCU compagnon — comme
OCULA le fait avec son RP2350. On ne câble pas un interpréteur VDU en FSM pure.

> Connaissances arrêtées début 2026 — des sorties très récentes peuvent manquer.

## 3. Le port `NG_VDU`

| Adr | Nom | R/W | Rôle |
|---|---|---|---|
| `$0357` | `NG_VDU` | W | Écrire un octet dans le flux de commandes VDU. |

Nécessite l'ULA-NG **déverrouillée** (comme toute la fenêtre `$0340-$035F`).
Depuis le BASIC : `POKE#357,octet` ; en code machine : `STA $0357` ; le flux se
« streame » octet par octet, exactement comme un `OSWRCH`/VDU.

## 4. Jeu de commandes v0.1 (minimal, à valider)

Vocabulaire aligné sur le VDU BBC/Agon quand ça a un sens, traduit en actions
ULA-NG. Chaque commande = un **code** + N octets de **paramètres**.

| Code | Params | Action ULA-NG | Analogue BBC/Agon |
|---|---|---|---|
| `20` | — | **Reset** : `NG_MODE=0` → retour au rendu normal. | `VDU 20` (restore) |
| `22 n` | 1 | **MODE** : `n`=0 std, 1 chunky (320), 2 texte 80col → `NG_MODE`. | `VDU 22,n` (MODE) |
| `19 l r g b` | 4 | **Palette** : LUT[`l`] = RGB444 (`r,g,b` sur 4 bits). | `VDU 19` (palette) |
| `18 a` | 1 | **Couleur de fond par cellule** : active les attributs // et remplit le plan avec `a`=`(paper<<3)\|ink`. | `VDU 18` (GCOL) |
| `31 col row a` | 3 | **Colorer une cellule** (`col` 0-39, `row` 0-24) avec l'attribut `a` — sans color clash. | `VDU 31` (TAB) |

Codes inconnus : ignorés (0 paramètre) — comme un VDU qui absorbe l'inconnu.

**Prévu v0.2+** (hors périmètre v0.1, listé pour cadrer) :
- `16` CLG / `25 k x y` PLOT → tracé dans un **VRAM chunky** *porté par l'ULA-NG*
  (nouveau buffer, voir §6) ;
- `23 …` définir sprite / caractère via le **protocole d'upload** (§5) ;
- `17`/`18` couleur « courante » + `28/24` fenêtres → `NG_SCRSTART`/scroll ;
- hook du **vecteur de sortie caractère** de l'Oric (équivalent `OSWRCH`) pour
  que `PRINT CHR$(…)` alimente `NG_VDU` sans POKE.

## 5. Protocole d'upload (« buffered commands », design v0.2)

Pour les données volumineuses (motifs de sprites 256 o, bitmaps chunky, fontes),
on reprend le modèle Agon, calqué sur le **streaming auto-incrémenté** déjà en
place dans l'ULA-NG :

1. **SELECT** un buffer/cible (ex. « sprite #k », « VRAM offset »),
2. **STREAM** les octets (auto-incrément, comme `NG_ATTR_DATA`/`NG_SPR_DATA`),
3. **USE** : référencer le buffer par ID (afficher le sprite, blitter la VRAM).

L'ULA-NG possède déjà les briques : `attr[]`, `sprites[].pattern[]`. Le protocole
VDU ne fait que les exposer par un vocabulaire uniforme et un ID de buffer.

## 6. Mémoire & VRAM

- v0.1 pilote l'**état existant** de l'ULA-NG (mode, palette, plan d'attributs) —
  **aucune nouvelle mémoire** requise.
- v0.2 (PLOT/CLG) nécessitera un **buffer VRAM chunky porté par `ula_ng`**
  (aujourd'hui le chunky lit la RAM CPU via `NG_SCRSTART`) : c'est la vraie étape
  « le VDU possède ses pixels ». À décider à ce moment-là.

## 7. Interpréteur (FSM)

État minimal, sans allocation : `vdu_cmd`, `vdu_params[8]`, `vdu_need`,
`vdu_got`. Quand `vdu_need==0` l'octet reçu est un **code** (on lit son nombre de
paramètres) ; sinon c'est un **paramètre** ; à `got==need` on **exécute** puis on
repart en attente de code. L'exécution se contente d'appeler la logique de
registres **déjà existante** (`ula_ng_write` sur `NG_MODE`, `NG_PAL_*`,
`NG_ATTR_*`) → intégration minimale, zéro duplication.

## 8. Modélisation émulateur

L'interpréteur en **C dans `ula_ng.c`** représente fidèlement le **firmware
soft-core** de la cible FPGA. Le 6502 émulé n'exécute aucun pilote : il écrit des
octets dans `$0357`. C'est la même abstraction que celle utilisée pour modéliser
l'OCULA-GPU.

## 9. Périmètre & risques (honnêteté)

- Adopter le modèle VDU complet (façon Agon VDP) = **un vrai sous-système** :
  on **commence minimal** (v0.1 ci-dessus) et on n'étend que sur besoin.
- La mémoire additionnelle de l'ULA-NG **n'est pas adressable par le CPU**
  (streaming only) : cohérent avec un port de commandes, mais le PLOT/VRAM (v0.2)
  demandera un buffer dédié.
- Ce port fait **grandir le rôle** de l'ULA-NG (d'« ULA » vers « ULA + VDU ») —
  choix d'architecture assumé, dans la lignée de l'OCULA-GPU.

## 10. Plan d'implémentation proposé (v0.1)

1. Registre `NG_VDU` ($0357) + FSM dans `ula_ng.c` (appelle `ula_ng_write`).
2. Tests unitaires (MODE, palette, fill, cellule, reset, commande inconnue).
3. Démo `ng_vdu.s` : déverrouille puis **stream une « VDU program »** (palette +
   mosaïque de couleur par cellule) — aucune logique d'interprétation côté 6502.
4. Garde visible + doc `README-ULA-NG.md` §VDU + spec §5.9.

## 11. Références

- Agon Light — documentation VDP / VDU (Console8/Quark firmware).
- Commander X16 — VERA programmer's reference.
- `docs/ocula_extensions.md` — OCULA-GPU (précédent maison).
- BBC Micro — VDU drivers (MOS), RISC OS VDU.
- `docs/ula-ng/ULA-NG-SPEC.md` (§5), `README-ULA-NG.md`.
