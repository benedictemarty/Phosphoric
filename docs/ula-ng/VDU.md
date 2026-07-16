# ULA-NG — VDU intégré (v0.1)

> **Statut : v0.1 + v0.2 (graphiques) + v0.3 (upload) IMPLÉMENTÉES ET VALIDÉES.**
> Port de commandes de style VDU exposé par l'ULA-NG (`NG_VDU` $0357), dont
> l'interpréteur **et la VRAM** vivent **dans l'ULA-NG** — pas dans les 64 Ko du
> 6502. Interpréteur dans `src/io/ula_ng.c` (stand-in du firmware soft-core FPGA) ;
> tests `test-ula-ng` ; démos `ng_vdu.s` (mosaïque), `ng_vdu_gfx.s` (tracé de
> lignes), `ng_vdu_spr.s` (sprite défini par flux) — pilotées **uniquement par le
> flux**, sans pilote 6502. Le hook OSWRCH transparent a été investigué et écarté
> (§4 : l'Oric n'a pas de vecteur de sortie par caractère ; piste ROM « 1.2 » en
> branche `feature/ula-ng-rom12-vdu`). Reste : fontes/bitmaps upload, PLOT étendu.

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

### Graphiques v0.2 (VRAM chunky portée par l'ULA-NG)

| Code | Params | Action ULA-NG | Analogue BBC/Agon |
|---|---|---|---|
| `16` | — | **CLG** : active le mode chunky + la **VRAM ULA-NG**, l'efface. | `VDU 16` (CLG) |
| `17 c` | 1 | **Couleur de tracé** courante (`c` 0-15, index LUT). | `VDU 18`/GCOL |
| `25 x y` | 2 | **PLOT** un point (`x` 0-159, `y` 0-223) — *simplifié* : point seul, coords 8 bits (le `VDU 25` BBC complet gère modes/lignes/coords 16 bits). | `VDU 25` (PLOT) |
| `26 x0 y0 x1 y1` | 4 | **DRAW** une ligne (Bresenham) dans la VRAM. | `VDU 25` DRAW |

Codes inconnus : ignorés (0 paramètre) — comme un VDU qui absorbe l'inconnu.

### Upload v0.3 (sprites via flux) — voir §5

`23 id` (begin upload motif sprite) + `24 id x y f` (position + enable).

**Prévu ensuite** (hors périmètre actuel) :
- fontes / bitmaps chunky via le même protocole d'upload (§5) ;
- fenêtres (`28`) → `NG_SCRSTART`/scroll ; PLOT étendu (triangles, coords 16 bits).

### Ergonomie d'entrée : conclusion sur le hook OSWRCH (investigué)

Objectif visé : `PRINT CHR$(…)` alimente `NG_VDU` **sans POKE**, façon BBC.
Investigation menée (désassemblage ROM Atmos `basic11b.rom` + tests) :

- **Pas de vecteur OSWRCH par caractère propre sur l'Oric.** La sortie caractère
  BASIC (`$CCD9`) contient bien un hook `BIT $02F1 / JSR ($023E)`, mais `$02F1`
  bit7 (redirection) est **remis à 0 en continu par l'IRQ 50 Hz** ; ni `PRINT`
  (flag forcé) ni `LPRINT` n'ont appelé un wedge installé en `$023E` (compteur
  resté à 0 — l'imprimante de l'émulateur capte au niveau matériel Centronics,
  pas par ce vecteur). Voie **abandonnée** (aucun code cassé livré).
- **Problème de vitesse** : rediriger par le port imprimante (Centronics, poignée
  de main) est ~1000× plus lent qu'un `STA $0357` direct → inutilisable pour les
  données en masse (sprites 256 o, images 16 000 o). Bon uniquement pour de
  petites commandes.

**Conclusion** : le chemin **rapide et fiable reste le pilotage direct**
(`STA $0357` en ML, `POKE #357` en BASIC), déjà en place. Le confort « BBC »
(vecteur de sortie RAM revectorable, pleine vitesse) suppose de **patcher une
ROM « BASIC 1.2 »** qui *ajoute* ce vecteur (l'Oric ne l'a pas). C'est une piste
réelle mais volumineuse (couture 6502 dans la ROM, œuvre dérivée sous copyright)
→ **explorée hors `main`, dans la branche `feature/ula-ng-rom12-vdu`**
(design : `docs/ula-ng/ROM12-VDU.md`).

## 5. Protocole d'upload (« buffered commands », v0.3 — sprites)

Pour les données volumineuses (motifs de sprites 256 o, plus tard bitmaps/fontes),
on reprend le modèle Agon, calqué sur le **streaming auto-incrémenté** de l'ULA-NG.
**Implémenté pour les sprites** :

| Code | Params | Action |
|---|---|---|
| `23 id` | 1 | **SELECT + BEGIN UPLOAD** : sprite `id` (0-15) ; les **256 octets suivants** sont streamés dans son motif (1 o/px, 0-7). |
| `24 id x y f` | 4 | **USE** : position (`x`,`y`) + `f` b0 = visible ; active aussi les sprites globalement. |

Séquence : `VDU 23,id` → 256 octets de motif → `VDU 24,id,x,y,1`. L'interpréteur
tient un compteur `vdu_upload` : tant qu'il est non nul, chaque octet reçu va dans
le motif (pas interprété comme commande). Extensible plus tard aux fontes/bitmaps
(même mécanisme, autre cible).

## 6. Mémoire & VRAM

- v0.1 pilote l'**état existant** de l'ULA-NG (mode, palette, plan d'attributs) —
  aucune nouvelle mémoire.
- **v0.2 (fait)** : `ula_ng.vram` (160×224 4bpp = 17920 o, 2 px/octet) — la vraie
  étape « le VDU possède ses pixels ». Quand `vram_active` (posé par `CLG`), le
  mode chunky lit **cette VRAM** au lieu de la RAM CPU (`NG_SCRSTART`). `PLOT`/
  `DRAW` écrivent dedans (Bresenham). `reset`/`20` la désactive. Côté FPGA :
  ce buffer vit dans la DDR3/BSRAM de l'ULA-NG.

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
