# ULA-NG — Cahier des charges d'implémentation

**Projet :** ajouter une ULA « next-gen » (ULA-NG) à l'émulateur **Phosphoric** (émulateur Oric-1 / Atmos de benedictemarty).
**Cible finale :** cette implémentation logicielle est la *référence* d'une future implémentation Verilog sur FPGA (**Sipeed Tang Primer 20K**, Gowin **GW2A-18**). Le comportement défini ici devra être reproductible bit à bit en HDL. Chaque choix doit donc rester **synthétisable en pensée** : pas de virgule flottante dans le chemin vidéo, pas de structure impossible à câbler.

> **Cible confirmée (photo produit + wiki Sipeed)** : bundle **20K Core Board +
> 20K Dock ext-board**, FPGA **GW2A-LV18PG256C8/I7** (marquage puce vérifié) —
> 20 736 LUT4, 15 552 FF, **BSRAM 828 Kbit** (46 blocs), **DDR3 128 Mbit** + NOR
> flash 32 Mbit sur la carte cœur, format SODIMM 204 broches.
> **Répartition mémoire visée** — à respecter dès le modèle logiciel :
> - **DDR3 128 Mbit sur la carte cœur** (le GW2A n'a pas de RAM in-package) : plan d'attributs parallèles (§5.6), banques VRAM, tables de sprites — *toute* mémoire volumineuse, hors des 64 Ko du 6502.
> - **Block RAM (BSRAM ~828 Kbit, budget serré)** : line buffers de composition, LUT palette (16×12 b), charset, petits caches sprites — *jamais* un framebuffer complet.
> - **Sortie vidéo : HDMI disponible via le dock ext-board** (également RGB565 FPC, Ethernet, USB-OTG/JTAG, GPIO). Le timing pixel visera le mode HDMI retenu ; sans impact sur le comportement de référence défini ici.

---

## Révisions post-audit (décisions figées)

Ces décisions découlent de l'audit `AUDIT.md` et **priment** sur le texte d'origine ci-dessous en cas de divergence :

1. **Détection robuste** (§3) : pas de test « ≠ 0 » (la fenêtre `$0340` recouvre le miroir du VIA → faux positif). Handshake exact `NG_ID == version` **+** registre complément `NG_IDCHK` (`$034F`) = `~NG_ID`.
2. **Lignes raster** (§5.2) : `ula_ng_scanline` piloté sur la **ligne trame complète 0-311** (découplé du rendu visible 0-223). `NG_RASTERLINE` 8 bits couvre 0-255 (visible + haut vblank) ; 9ᵉ bit réservé dans `NG_MODE` si besoin futur.
3. **LUT palette** (§5.1) : **16 entrées × 12 bits** (RGB444), expansion RGB444→888 par réplication de quartet (`c8 = c4*0x11`). Couvre standard (8) et chunky (16).
4. **Sémantique registres** (§9) : tout registre à effet visuel s'applique à la **ligne suivante** (latch en hblank) ; **exception** `NG_STATUS` (acquit IRQ) = immédiat. Tableau détaillé en §9.
5. **Plan d'attributs / banques / sprites** (§5.6, §5.7) : **mémoire additionnelle portée par `ula_ng`** (miroir de la SDRAM externe), hors des 64 Ko du 6502.

---

## 0. Avant toute chose — audit du code (première tâche obligatoire)

Cette spec contient des marqueurs `[À CONFIRMER]`. **Ne pas écrire de code fonctionnel avant d'avoir rempli ces trous.** Produire d'abord un court rapport `AUDIT.md` répondant à :

1. **Langage et build.** Quel langage (C / C++ / Rust / autre) ? Comment se compile et se lance l'émulateur ? Y a-t-il déjà des tests ?
2. **Granularité du rendu vidéo.** Le rendu se fait-il :
   - (a) **frame-based** : la VRAM est lue d'un bloc en fin de trame ;
   - (b) **scanline-based** : une ligne est composée à chaque ligne balayée ;
   - (c) **cycle-exact** : la ULA lit la mémoire au fil des cycles du 6502.
   Localiser la fonction concernée (nom + fichier + lignes).
3. **Interception des accès mémoire.** Comment sont routées les lectures/écritures CPU ? Existe-t-il un dispatch sur l'adresse (switch, table de handlers) ou un tableau mémoire plat ? Où précisément la page 3 (`#0300`-`#03FF`) est-elle traitée (le VIA, notamment) ?
4. **Structure de la couleur.** Où se fait la conversion des 8 couleurs Oric vers les pixels de sortie (RGB) ? Palette codée en dur ?
5. **Horloge / IRQ.** Comment le 6502 émulé reçoit-il une IRQ ? Quelle est la source de temps (compteur de cycles, de scanlines) accessible depuis le code vidéo ?

Le reste de la spec suppose qu'on peut passer à un rendu **au moins scanline-based** (b). Si Phosphoric est frame-based (a), **la première étape d'implémentation est de convertir le pipeline vidéo en scanline** — le signaler dans `AUDIT.md` comme préalable, car les fonctions raster (IRQ ligne, palette par ligne) en dépendent.

---

## 1. Principes directeurs

1. **Compatibilité d'abord.** Au reset, l'ULA-NG est indiscernable d'une HCS10017 : attributs série, 8 couleurs, 50 Hz, quirks inclus. Aucun programme existant ne doit voir de différence.
2. **Activation explicite.** Les extensions ne s'activent qu'après une **séquence de déverrouillage** (voir §3). Objectif : qu'aucun logiciel balayant la page 3 ne déclenche une extension par accident.
3. **Modularité miroir FPGA.** Tout le nouveau code vit dans un module isolé `ula_ng` (fichier(s) dédié(s)), avec des frontières qui correspondent à ce que seront les frontières du module Verilog. Trois interfaces seulement :
   - `ula_ng_write(addr, value)` / `ula_ng_read(addr)` — accès registres (page 3) ;
   - `ula_ng_scanline(line_number)` — appelé par la boucle vidéo à chaque ligne ;
   - une sortie **ligne d'IRQ** vers le cœur 6502.
4. **Testabilité.** Tout comportement observable doit pouvoir être capturé en trace (voir §6) pour servir de golden reference au banc de test Verilog.

---

## 2. Carte des registres

Fenêtre proposée : **`#0340`-`#035F`** dans la page 3 (libre dans la cartographie communautaire : VIA `#0300`-`#030F`, Microdisc `#0310`-`#031F`, ACIA `#031C`, Jasmin `#03F4`+).
**`[À CONFIRMER]`** : vérifier qu'aucune extension émulée par Phosphoric n'occupe déjà `#0340`-`#035F`. Si conflit, décaler la fenêtre et mettre à jour ce tableau.

| Adresse | Nom | Accès | Rôle |
|---|---|---|---|
| `#0340` | `NG_LOCK` / `NG_ID` | W / R | Écriture : séquence de déverrouillage. Lecture : **verrouillé → passthrough VIA** (indiscernable) ; déverrouillé → octet de version (**`0x1E` = v1.0**). |
| `#034F` | `NG_IDCHK` | R | Complément de `NG_ID` : `~NG_ID` (`0xE1` déverrouillé, `0x00`→`0xFF` verrouillé). Handshake anti-faux-positif (voir §3). |
| `#0341` | `NG_MODE` | R/W | Bits de mode : b0 = extensions actives, b1 = mode attributs parallèles, b2-3 = mode vidéo (00 = std, 01 = chunky 4bpp, 10 = texte 80col), b4-5 = banque VRAM, b6 = 50/60 Hz, b7 = réservé. |
| `#0342`-`#0343` | `NG_SCRSTART` | R/W | Adresse de début d'écran (16 bits, LSB puis MSB). |
| `#0344` | `NG_SCROLLX` | R/W | Décalage fin X (0-5 pixels). |
| `#0345` | `NG_SCROLLY` | R/W | Décalage fin Y (0-7 pixels). |
| `#0346` | `NG_RASTERLINE` | R/W | Numéro de ligne déclenchant l'IRQ raster. |
| `#0347` | `NG_STATUS` | R/W | Lecture : b7 = IRQ raster en attente. Écriture : **acquittement** (clear b7) **+ b0 = enable IRQ raster** (persistant). |
| `#0348` | `NG_PAL_IDX` | R/W | Index de palette à programmer (**0-15**, LUT 16 entrées), auto-incrément optionnel. |
| `#0349`-`#034A` | `NG_PAL_DATA` | R/W | Couleur 12 bits (4096 teintes) : `#0349` = `0000RRRR`, `#034A` = `GGGGBBBB`. |
| `#034B` | `NG_COP_CTRL` | W | Copper (§5.4) : écriture = reset du pointeur de flux (vide la liste). |
| `#034C` | `NG_COP_DATA` | W | Copper : flux 3 octets/entrée — `ligne`, `(index<<4)|R`, `(G<<4)|B` (64 entrées max). |
| `#034D` | `NG_ATTR_FILL` | W | Attributs // (§5.6) : remplit tout le plan 8 Ko avec l'octet écrit `(paper<<3)|ink` + reset du pointeur de flux. |
| `#034E` | `NG_ATTR_DATA` | W | Attributs // : écrit une cellule au pointeur `(paper<<3)|ink` puis auto-incrémente (modulo 8192). |
| `#0350` | `NG_SPR_CTRL` | W | Sprites (§5.7) : b0 = enable global. |
| `#0351` | `NG_SPR_SEL` | W | Sprite sélectionné pour la programmation (0-15) + reset du pointeur de motif. |
| `#0352` | `NG_SPR_X` | W | Position X (0-255) du sprite sélectionné. |
| `#0353` | `NG_SPR_Y` | W | Position Y (0-255). |
| `#0354` | `NG_SPR_ATTR` | W | b0 = sprite visible. |
| `#0355` | `NG_SPR_DATA` | W | Flux motif : 1 octet/pixel (`0` = transparent, `1`-`7` = index palette), auto-incrément (mod 256). |
| `#0356` | `NG_SPR_STATUS` | R | b7 = collision sprite-sprite depuis la dernière lecture (clear on read). |

Toute adresse de la fenêtre non listée : lecture `0xFF`, écriture ignorée (mais réserver pour extension).

---

## 3. Séquence de déverrouillage (`NG_LOCK`)

Objectif : signature improbable en fonctionnement normal.

- Écrire successivement `0x4E` ('N') puis `0x47` ('G') dans `#0340`, **sans autre écriture dans la fenêtre `#0340`-`#035F` entre les deux**.
- À la bonne séquence : `NG_ID` (`$0340`) renvoie la version (`0x1E`), `NG_IDCHK` (`$034F`) renvoie son complément (`0xE1`), et `NG_MODE.b0` devient inscriptible. Tant que verrouillé, écrire dans `#0341`-`#035F` est **sans effet**.
- Un reset re-verrouille tout et remet tous les registres à 0 (état HCS10017).

> **Passthrough verrouillé (décision d'implémentation, étape 1).** Pour une
> non-régression **bit-à-bit**, en état verrouillé l'ULA-NG **ne pilote pas** la
> fenêtre `$0340-$035F` : les lectures **retombent sur le VIA** et les écritures
> y tombent aussi (le module surveille seulement `$0340` pour la séquence). Elle
> ne « possède » la fenêtre qu'après déverrouillage. Côté FPGA = tristate
> (`drive_bus = unlocked && addr_in_window`). Conséquence : la détection ci-dessous
> marche à l'identique (avant déverrouillage, `LDA $0340` lit le VIA → `≠ 0x1E` →
> `no_ng` ; après, `0x1E`).

> **Détection robuste (révision post-audit).** `$0340` recouvre le **miroir du VIA**
> (le VIA répond en fallback sur tout `$0300-$03FF`). Sur une machine **sans**
> ULA-NG, `LDA $0340` lit l'ORB du VIA (latch colonne clavier) → valeur non nulle
> → un test « ≠ 0 » donnerait un **faux positif**. Il faut donc (a) comparer à la
> **valeur exacte** `0x1E`, **et** (b) vérifier la cohérence `NG_ID XOR NG_IDCHK
> == 0xFF`, que le VIA ne peut pas produire sur deux adresses. Les écritures
> `'N'/'G'` frappent l'ORB du VIA sur machine nue (inoffensif : latch colonne).

Détection côté logiciel Oric (à documenter pour les codeurs) :
```asm
    LDA #$4E : STA $0340        ; 'N'
    LDA #$47 : STA $0340        ; 'G'  (aucune autre écriture $0340-$035F entre les deux)
    LDA $0340 : CMP #$1E : BNE no_ng     ; version exacte ?
    LDA $034F : EOR $0340 : CMP #$FF : BNE no_ng   ; NG_ID XOR NG_IDCHK = $FF ?
    ; ULA-NG présente et déverrouillée
no_ng:
```

---

## 4. Ordre d'implémentation

Implémenter et **valider une feature avant de passer à la suivante** (chaque étape a un test visible) :

1. `NG_LOCK` / `NG_ID` + plomberie d'interception page 3.
2. **Palette indirection** (§5.1) — les 8 couleurs passent par une LUT.
3. **IRQ raster** (§5.2) — la feature la plus demandée.
4. **Start address** (§5.3) — double buffer / scroll grossier.
5. Palette par scanline (§5.4).
6. Scroll fin X/Y (§5.5).
7. Attributs parallèles (§5.6).
8. Sprites (§5.7).
9. Modes chunky / 80 colonnes (§5.8).

---

## 5. Spécification des fonctionnalités

### 5.1 Palette indirection
Les couleurs logiques deviennent des index dans une **LUT de 16 entrées × 12 bits** (RGB444) — 8 suffisent au mode standard, 16 servent au chunky 4bpp (§5.8). Au reset, les 8 premières entrées contiennent les 8 couleurs Oric d'origine (mapping identité → compatibilité). Le chemin de rendu remplace tout accès direct « couleur → RGB » par « couleur → LUT → RGB ». Expansion **RGB444 → RGB888 par réplication de quartet** (`c8 = c4 * 0x11`), aucune interpolation (synthétisable).

**Point de conversion (résolu par l'audit) :** `get_rgb()` dans `src/video/video.c` (≈ l.109-113) lit déjà `vid->pal_rgb[c][3]` — **c'est le seul endroit à faire pointer sur la LUT NG**. `pal_rgb` EST la LUT ; il suffit de l'alimenter depuis `NG_PAL_*` (16×12 bits) au lieu de la table fixe quand les extensions sont actives.

### 5.2 IRQ raster
`NG_RASTERLINE` fixe une ligne ; quand `ula_ng_scanline(n)` reçoit `n == NG_RASTERLINE`, lever `NG_STATUS.b7` et asserter la ligne d'IRQ vers le 6502. L'IRQ reste active jusqu'à écriture d'acquittement dans `NG_STATUS`.

**Numérotation (révision post-audit) :** `ula_ng_scanline` est piloté sur la **ligne trame complète 0-311** (`frame_cycles / 64`), découplé du rendu visible 0-223, pour permettre aussi les IRQ en zone vblank. `NG_RASTERLINE` 8 bits couvre 0-255 (tout le visible + haut vblank) ; un 9ᵉ bit reste réservé dans `NG_MODE` pour 256-311 si besoin.

**Raccord IRQ (résolu par l'audit) :** IRQ 6502 = bitfield **level-triggered OU-câblé** (`cpu.irq`, `include/cpu/cpu6502.h`). Ajouter `IRQF_ULANG = 0x20` (prochain bit libre) et asserter/acquitter via `cpu_irq_set/clear(&emu->cpu, IRQF_ULANG)`. Chaque source garde son bit → **on combine, on n'écrase pas** (exigence respectée par construction).

**Enable (décision d'implémentation, étape 3) :** un bit d'activation évite les IRQ parasites (au reset `NG_RASTERLINE`=0). **`NG_STATUS.b0` (écriture) = enable** (persistant) ; toute écriture de `NG_STATUS` acquitte aussi (clear b7). L'IRQ n'est levée que si **déverrouillé && NG_MODE.b0 && enable**. En BASIC nu (vecteur IRQ en ROM), armer l'IRQ sans ISR gèle la machine (boucle d'IRQ non acquittée) — c'est le comportement attendu ; une barre raster propre requiert un ISR (redirection du vecteur sous Sedoric/overlay).

**Implémenté (étape 3, validé)** : `ula_ng_scanline(line)` piloté par la boucle vidéo sur la trame 0-311, `NG_STATUS.b7`/acquit, `IRQF_ULANG`. Unit tests + observable end-to-end (BASIC gelé quand armé sans ISR).

### 5.3 Start address
`NG_SCRSTART` remplace l'adresse de base fixe du fetch vidéo (`#A000` en HIRES, `#BB80` en TEXT). Par défaut = valeur d'origine. Permet double buffering (basculer entre deux buffers) et scroll vertical grossier (incrément par ligne).

**Implémenté (étape 4, validé)** : `NG_SCRSTART` (`$0342` LSB / `$0343` MSB, 16 bits) est appliqué au **fetch de la zone principale** (lignes 0-199 : `$A000`+y·40 en HIRES, `$BB80`+row·40 en TEXT) quand actif (`déverrouillé && NG_MODE.b0`). **`$0000` = base par défaut du mode** (compat, pas de blank au reset ; `$0000` n'est jamais un écran valide). Les 3 rangées de statut (lignes 200-223) restent fixes à `$BB80`. Câblage `video_t` par pointeur (`ng_scrstart`). Test visible : `NG_SCRSTART = $BB80+40` → l'écran remonte d'une rangée (comparaison pixel-exacte du framebuffer).

### 5.4 Palette par scanline
Une petite liste d'instructions palette (mini-copper) en RAM, appliquée pendant le hblank par `ula_ng_scanline`. Format à définir simplement : table de (ligne, index, couleur) triée. Garder trivialement synthétisable (une FIFO relue par ligne).

**Implémenté (étape 5, validé)** : liste copper de 64 entrées max, chaque entrée = `(ligne, index LUT, couleur RGB444)`. Programmation par flux : `NG_COP_CTRL` (`$034B`) en écriture réinitialise la liste ; `NG_COP_DATA` (`$034C`) reçoit **3 octets par entrée** : `[0]=ligne`, `[1]=(index<<4)|R`, `[2]=(G<<4)|B` (commit à chaque 3ᵉ octet). `ula_ng_scanline(line)` applique à la LUT (`u->pal[index]`) toute entrée dont `ligne == line` (actif si `déverrouillé && NG_MODE.b0`). Le hook vidéo relit la LUT → effet **ligne suivante** (cohérent §9). Prévoir une entrée « ligne 0 » pour poser la base à chaque trame. Test visible : couleur 7 rouge (ligne 0) puis bleue (ligne 30) → bandes de couleur verticales (framebuffer).

### 5.5 Scroll fin X/Y
`NG_SCROLLX` (0-5) et `NG_SCROLLY` (0-7) décalent le pipeline de fetch au niveau pixel. En pratique : offset appliqué au moment de composer la ligne.

**Implémenté (étape 6, validé)** : `NG_SCROLLX` (`$0344`, clampé 0-5 = largeur cellule 6 px), `NG_SCROLLY` (`$0345`, masqué 0-7 = hauteur 8 px). Appliqué à la composition de la zone principale (0-199) quand actif (`déverrouillé && NG_MODE.b0`) : **Y** décale la ligne source (`src_y = y + scrolly`, contenu vers le haut) ; **X** décale l'affichage (`px = col·6 - scrollx`, `set_pixel` clippe hors écran, une cellule de plus fetchée pour combler le bord droit). Combiné au scroll grossier (§5.3) → défilement lisse. Inactif / 0 → rendu bit-à-bit inchangé. Test visible : `NG_SCROLLY=4` / `NG_SCROLLX=3` → décalage pixel-exact vérifié au framebuffer.

### 5.6 Attributs parallèles
Un second plan mémoire de 8 Ko (banque sélectionnée par `NG_MODE.b4-5`) fournit encre+papier par cellule 8×1 **sans consommer d'octets pixel dans le flux** — supprime le color clash sériel. Actif seulement si `NG_MODE.b1`.

**Localisation mémoire (révision post-audit) :** ce plan (et les banques VRAM, et les tables de sprites §5.7) vit dans une **mémoire additionnelle portée par le module `ula_ng`** — miroir de la **DDR3 externe** (128 Mbit) de la carte cœur Tang Primer 20K — **hors des 64 Ko** adressables par le 6502. Accès par une fenêtre de registres NG (adresse + auto-incrément), jamais mappé dans l'espace CPU. Côté émulateur : un buffer dans `ula_ng_t` ; côté FPGA : la DDR3.

**Implémenté (étape 7, validé)** : plan `ula_ng.attr[8192]` (octet par cellule, `(paper<<3)|ink`, 3 bits chacun), indexé `y·40 + col` (0-7999 sur les 8192). Programmation par flux : `NG_ATTR_FILL` (`$034D`) remplit tout le plan d'un octet uniforme + remet le pointeur à 0 (une écriture = un fond complet, pratique pour les démos) ; `NG_ATTR_DATA` (`$034E`) écrit la cellule courante et auto-incrémente (modulo 8192). Actif seulement si `déverrouillé && NG_MODE.b1` (`ula_ng.attr_active`). Quand actif, la composition de la zone principale (0-199) tire encre+papier **du plan** au lieu des attributs sériels (les octets `#00-#1F` ne sont plus interprétés comme attributs → **plus de color clash**). Inactif → rendu bit-à-bit inchangé. Test visible : `NG_MODE.b1` + `NG_ATTR_FILL=$21` (papier bleu 4, encre rouge 1) → zone principale entièrement bleu+rouge (aucune autre couleur), vérifié au framebuffer.

### 5.7 Sprites matériels
Jusqu'à 16 sprites 16×16, 3 bpp (index palette), avec priorité et détection de collision. Table de sprites en RAM pointée par un registre de `#0350`-`#035F`. Composition dans le pipeline de sortie (après le fond, avant conversion RGB) — invisibles pour la VRAM. Bit de collision lisible dans la zone `#0350`-`#035F`.

**Implémenté (étape 8, validé)** : 16 sprites 16×16 dans la mémoire additionnelle `ula_ng.sprites[16]` (motif 1 octet/px, `0` = transparent, `1`-`7` = index LUT palette ; hors des 64 Ko du 6502, miroir DDR3). Programmation par flux via la fenêtre : `NG_SPR_CTRL` ($0350, b0 = enable global), `NG_SPR_SEL` ($0351, sprite courant + reset pointeur motif), `NG_SPR_X`/`NG_SPR_Y` ($0352/$0353, position écran), `NG_SPR_ATTR` ($0354, b0 = visible), `NG_SPR_DATA` ($0355, flux motif auto-incrémenté). Composition dans `ula_ng_composite_scanline()` appelée par le hook vidéo après le rendu du fond de chaque scanline (couleur = LUT palette NG). **Priorité par index** : les sprites sont composés 15→0, donc le sprite 0 finit au-dessus. **Détection de collision** sprite-sprite (recouvrement de pixels opaques sur une scanline) → `NG_SPR_STATUS` ($0356, b7, clear on read). Gate `unlocked && NG_SPR_CTRL.b0` (`spr_active`) ; inactif → rendu inchangé. Test visible : sprite 0 (16×16 rempli d'index 1 = rouge) à (100,100) → bloc rouge de 256 px pixel-exact au framebuffer. *Non encore implémenté (raffinement futur)* : priorité vis-à-vis du fond (le bit « derrière le fond » nécessite un tampon d'index de fond ; les sprites sont pour l'instant toujours au premier plan), 3 bpp compact côté FPGA (émulateur = 1 octet/px).

### 5.8 Modes chunky / 80 colonnes
- **Chunky 4bpp** : 160×200, 16 couleurs parmi 4096 (via LUT étendue si besoin).
- **Texte 80 colonnes** : police redéfinissable en RAM. Utile pour Sedoric.
Sélection par `NG_MODE.b2-3`.

**Implémenté (étape 9, validé)** : sélection par `NG_MODE.b2-3` (`01` = chunky, `10` = 80 col ; caches `ula_ng.chunky_active` / `text80_active` = `active && vidmode`, donc gate `unlocked && NG_MODE.b0`). Les modes sont **latchés au début de trame** (comme les modes OCULA) : la largeur du framebuffer reste stable une trame entière. Données lues depuis `NG_SCRSTART` (§5.3, défaut `$A000`).
- **Chunky 4bpp** : 160 px/rangée, chacun 4 bits = index dans la **LUT palette NG 16 entrées** (RGB888, §5.1). 80 octets/rangée (quartet haut = pixel gauche). Chaque pixel chunky occupe 2 px framebuffer → **320 px** de large (`OCULA_EXTHIRES_W`). **Plein écran** : le mode couvre toute la hauteur visible (rangées 0-223 lues depuis le buffer), **sans pied de texte 40 colonnes** — contrairement aux modes dérivés du HIRES, un mode bitmap moderne n'a pas de bande de statut (ce qui évitait un trou noir 240-319 en bas). Test visible : `NG_MODE=$05` + palette index 0 = magenta → framebuffer **320×224**, magenta dominant (zones à zéro) + autres couleurs des données = 16 couleurs.
- **Texte 80 colonnes** : 80 caractères × 6 px = **480 px** (`OCULA_MAX_W`). Charset RAM redéfinissable (`$B400`/`$B800` via `get_charset_byte`, mécanisme natif Oric — aucune police inventée) ; attributs série et couleurs (LUT NG 0-7) comme en texte standard. Test visible : `NG_MODE=$09` → framebuffer **480×224**, caractères rendus depuis `$A000`.

Composition parallèle avec les sprites (§5.7) : `ula_ng_composite_scanline` est appelée après le rendu du fond dans les deux modes. Inactif → rendu bit-à-bit inchangé (largeur 240). *Non implémenté (raffinement futur)* : la variante 160×200 « stretchée » à 240 px (choix : 2× vers 320 px, mapping entier exact) ; le 3 bpp/4096 teintes suppose une LUT étendue côté FPGA (émulateur = 4 bits/px + LUT 16 entrées).

---

## 6. Traces (« golden reference »)

Ajouter un mode debug (flag CLI, ex. `--ula-trace=fichier`) qui, **par frame**, sérialise :
- l'état des registres `#0340`-`#035F` ;
- la sortie RGB **ligne par ligne** (ou un hash par ligne pour alléger) ;
- les cycles/lignes où l'IRQ raster est assertée.

Format texte simple, déterministe, diffable. Ces traces serviront de référence : le futur banc de test Verilog rejouera les mêmes programmes et comparera. Toute divergence localise le bug à la ligne près.

---

## 7. Compatibilité — non-régression

Avant de considérer une étape terminée, vérifier que **le mode verrouillé (extensions off) est bit-à-bit identique** au comportement d'avant modification. Idéalement :
- capturer des traces de référence de quelques programmes (démos utilisant les attributs, double hauteur, lores) **avant** tout changement ;
- rejouer après chaque étape ; le mode classique doit produire des traces identiques.
Programmes de test suggérés : la demonstration tape de l'Atmos (beaucoup d'attributs, double taille, lores 1), et un jeu utilisant les caractères double hauteur.

---

## 8. Livrables attendus

1. `AUDIT.md` — réponses au §0.
2. Module `ula_ng` (fichier(s) séparé(s)) avec les 3 interfaces du §1.3.
3. Modifications minimales du pipeline vidéo et du dispatch page 3, clairement isolées et commentées.
4. Mode trace du §6.
5. `README-ULA-NG.md` côté utilisateur : séquence de déverrouillage, carte des registres, 3 exemples assembleur commentés (activer le mode, charger une palette, poser une IRQ raster).
6. Jeux/snippets de test pour chaque feature.

---

## 9. Contraintes « pensées FPGA » (à respecter dès le logiciel)

- Pas de flottant dans le chemin vidéo ; couleurs et offsets en entiers.
- Toute structure de données du module `ula_ng` doit avoir un équivalent matériel évident (registre, petite RAM, LUT, FIFO). Éviter allocations dynamiques dans le chemin temps réel.
- Les signaux de contrôle type `#0340`-`#035F` sont échantillonnés de façon déterministe (une fois par accès / par ligne), jamais « quand ça arrive ».
- Documenter, pour chaque registre, s'il est lu de façon combinatoire (dans la ligne courante) ou synchrone (effet à la ligne/trame suivante) — ce détail conditionne la fidélité FPGA.

### Sémantique par registre (décision post-audit)

Règle : **tout registre à effet visuel s'applique à la ligne SUIVANTE** (latch en hblank), cohérent avec le rendu scanline qui fige la VRAM à l'instant CPU. **Exceptions** immédiates (combinatoires) : acquit d'IRQ et déverrouillage.

| Registre | Effet | Échantillonnage |
|---|---|---|
| `NG_LOCK`/`NG_ID`/`NG_IDCHK` | verrou / identité | **combinatoire** (immédiat) |
| `NG_MODE` | mode vidéo, banques | **synchrone** — ligne suivante (b0 activation : immédiate) |
| `NG_SCRSTART` | base de fetch | **synchrone** — trame suivante (double buffer) |
| `NG_SCROLLX/Y` | décalage pixel | **synchrone** — ligne suivante |
| `NG_RASTERLINE` | ligne d'IRQ | **synchrone** — comparé par `ula_ng_scanline` |
| `NG_STATUS` (écriture) | acquit IRQ | **combinatoire** (immédiat) |
| `NG_PAL_IDX`/`NG_PAL_DATA` | LUT palette | **synchrone** — ligne suivante (permet le split raster) |
| `NG_SPRITE_*` | contrôle sprites | **synchrone** — ligne suivante ; `collision` en lecture = **combinatoire** |
