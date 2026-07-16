# ULA-NG — Guide utilisateur

ULA « next-generation » pour Phosphoric : extensions vidéo activées par
déverrouillage, **indiscernables d'une HCS 10017 tant que verrouillées**.
Référence logicielle d'un futur portage FPGA (Sipeed Tang Primer 20K /
GW2A-18).

> État : **8 features implémentées et validées** (§5.1 palette-indirection →
> §5.8 chunky/80col). Voir `ULA-NG-SPEC.md` (spec complète) et `AUDIT.md`
> (architecture). Démos exécutables : `demos/ula-ng/`.

---

## 0. Le mode « ULA normal » (par défaut)

Au reset, **l'ULA-NG est verrouillée** : elle se comporte exactement comme
l'ULA d'origine de l'ORIC-1/Atmos (HCS 10017), **bit pour bit**. La fenêtre de
registres `$0340-$035F` retombe alors sur le VIA. **Aucun programme ne voit de
différence** tant qu'il ne déverrouille pas explicitement l'ULA-NG.

Autrement dit : *ne rien faire = ULA normale*. Il n'y a **aucun flag à passer**
à l'émulateur — les démos se déverrouillent elles-mêmes (`-t ng_chunky.tap -f`).

> **À ne pas confondre avec `--ula PROFILE`** : c'est un axe différent
> (`hcs10017`, défaut / `ocula`, le projet de remplacement RP2350). L'ULA-NG est
> une couche indépendante, dormante par défaut, qui fonctionne quel que soit le
> profil.

---

## 1. Il n'y a pas de mot-clé BASIC

Le BASIC de l'Oric est figé en ROM (1983) et ne connaît pas l'ULA-NG : **aucun
mot-clé** du type `HIRES`/`TEXT` ne l'active. On pilote l'ULA-NG **en écrivant
ses registres**, par trois moyens équivalents :

1. **`POKE`** depuis le BASIC (adresses en décimal ou en `#hexa`).
2. **`STA $034x`** en code machine (voir les démos `.s`).
3. **`--ula-ng-poke "…"`** côté émulateur (injection au démarrage).

C'est fidèle au vrai matériel : sur une carte FPGA, on piloterait aussi les
registres au POKE, comme toute extension hardware Oric. Un jeu de mots-clés
étendus nécessiterait une **ROM d'extension compagnon** (non fournie).

---

## 2. Déverrouillage

Fenêtre registres : **`$0340`-`$035F`**. Séquence : écrire `'N'` (`$4E`) puis
`'G'` (`$47`) dans **`$0340`**, sans autre écriture de la fenêtre entre les deux.

```asm
        LDA #$4E : STA $0340        ; 'N'
        LDA #$47 : STA $0340        ; 'G'
        LDA $0340 : CMP #$1E : BNE no_ng             ; NG_ID = version ?
        LDA $034F : EOR $0340 : CMP #$FF : BNE no_ng  ; handshake ~NG_ID
        ; ULA-NG présente et déverrouillée
no_ng:
```

En BASIC : `POKE#340,78:POKE#340,71` (78=`$4E`, 71=`$47`).

Un **reset re-verrouille tout** → retour immédiat à l'ULA normale.

---

## 3. Activer un mode (après déverrouillage)

Chaque feature s'arme par un registre. `NG_MODE` (`$0341`) porte l'essentiel :

| Feature (spec) | Registre / bits | Valeur `NG_MODE` |
|---|---|---|
| Palette, copper, scroll fin, start-address, IRQ raster | `NG_MODE.b0` (extensions actives) | `$01` |
| Attributs parallèles (§5.6) | `NG_MODE.b1` | `$02` |
| **Chunky 4bpp** (§5.8) | `NG_MODE` b0 + b2-3 = `01` | `$05` |
| **Texte 80 colonnes** (§5.8) | `NG_MODE` b0 + b2-3 = `10` | `$09` |
| **Sprites** (§5.7) | `NG_SPR_CTRL.b0` (`$0350`), **indépendant** de `NG_MODE` | — |

**Revenir au normal** sans reset : écrire `NG_MODE=0` (et `NG_SPR_CTRL=0`) — les
extensions visuelles se désactivent (rendu standard), la fenêtre restant
possédée jusqu'au reset.

---

## 4. Carte des registres (`$0340`-`$035F`)

| Adr | Nom | R/W | Rôle |
|---|---|---|---|
| `$0340` | `NG_ID` (R) / `NG_LOCK` (W) | R/W | R : `$1E` si déverrouillé. W : séquence 'N','G'. |
| `$0341` | `NG_MODE` | R/W | b0 = extensions actives ; b1 = attributs // ; b2-3 = mode vidéo (00 std, 01 chunky, 10 80col). |
| `$0342`-`$0343` | `NG_SCRSTART` | W | Base du fetch vidéo (LSB/MSB). `$0000` = défaut. |
| `$0344` | `NG_SCROLLX` | W | Décalage fin X (0-5 px). |
| `$0345` | `NG_SCROLLY` | W | Décalage fin Y (0-7 px). |
| `$0346` | `NG_RASTERLINE` | W | Ligne (0-255) déclenchant l'IRQ raster. |
| `$0347` | `NG_STATUS` | R/W | R b7 = IRQ en attente. W = acquit + b0 = enable IRQ. |
| `$0348` | `NG_PAL_IDX` | W | Index LUT palette (0-15), auto-incrément. |
| `$0349` | `NG_PAL_DATA` lo | W | `0000RRRR`. |
| `$034A` | `NG_PAL_DATA` hi | W | `GGGGBBBB` (commit + incrément d'index). |
| `$034B` | `NG_COP_CTRL` | W | Réinitialise la liste copper. |
| `$034C` | `NG_COP_DATA` | W | Flux 3 o/entrée : `ligne`, `(idx<<4)|R`, `(G<<4)|B`. |
| `$034D` | `NG_ATTR_FILL` | W | Remplit tout le plan d'attributs 8 Ko + reset pointeur. Octet = `(paper<<3)|ink`. |
| `$034E` | `NG_ATTR_DATA` | W | Flux 1 o/cellule (auto-incrément, index `scanline*40+col`). |
| `$034F` | `NG_IDCHK` | R | `~NG_ID` (handshake). |
| `$0350` | `NG_SPR_CTRL` | W | b0 = enable global sprites. |
| `$0351` | `NG_SPR_SEL` | W | Sprite sélectionné (0-15) + reset pointeur motif. |
| `$0352` | `NG_SPR_X` | W | Position X (0-255) du sprite. |
| `$0353` | `NG_SPR_Y` | W | Position Y (0-255). |
| `$0354` | `NG_SPR_ATTR` | W | b0 = sprite visible. |
| `$0355` | `NG_SPR_DATA` | W | Flux motif 16×16 : 1 o/px (`0`=transparent, `1`-`7`=index LUT), auto-incr. |
| `$0356` | `NG_SPR_STATUS` | R | b7 = collision sprite-sprite (clear on read). |
| `$0357` | `NG_VDU` | W | Flux de commandes VDU intégré (voir [VDU.md](VDU.md)). |

---

## 5. Recettes `--ula-ng-poke` (émulateur)

Le flag CLI programme les registres au démarrage. `SEQ` = paires `AAA=VV` (hex)
séparées par des virgules. Combinez avec `--screenshot-at CYCLES:FICHIER`.

```bash
# Palette : couleur 7 (blanc) -> vert
--ula-ng-poke "340=4E,340=47,341=01,348=07,349=00,34A=F0"

# Copper : couleur 7 rouge (ligne 0) puis bleu (ligne 30) -> bandes
--ula-ng-poke "340=4E,340=47,341=01,34B=00,34C=00,34C=7F,34C=00,34C=1E,34C=70,34C=0F"

# Start-address : scroll d'une rangée texte ($BB80+40 = $BBA8)
--ula-ng-poke "340=4E,340=47,341=01,342=A8,343=BB"

# Attributs parallèles : papier bleu (4) + encre rouge (1) = $21
--ula-ng-poke "340=4E,340=47,341=02,34D=21"

# Chunky 4bpp : NG_MODE=$05 + palette index 0 = magenta (écran 320px)
--ula-ng-poke "340=4E,340=47,341=05,348=00,349=0F,34A=0F"

# Texte 80 colonnes : NG_MODE=$09 (écran 480px)
--ula-ng-poke "340=4E,340=47,341=09"

# IRQ raster ligne 100 (enable) : ATTENTION, sans ISR -> boucle d'IRQ
--ula-ng-poke "340=4E,340=47,341=01,346=64,347=01"
```

Équivalent BASIC : `POKE` des mêmes adresses en décimal (`$0340`=832…). La limite
de ligne BASIC (~80 car.) impose de découper les longues séquences.

---

## 6. Depuis du code machine

```asm
        LDA #$4E : STA $0340       ; unlock 'N'
        LDA #$47 : STA $0340       ; unlock 'G'
        LDA #$01 : STA $0341       ; NG_MODE.b0 = extensions actives
        LDA #$01 : STA $0348       ; index palette 1
        LDA #$0F : STA $0349       ; R=F
        LDA #$F0 : STA $034A       ; G=F,B=0 -> jaune, commit
```

Pour l'IRQ raster, installer un ISR qui acquitte (`STA $0347`) — nécessite un
vecteur IRQ redirigeable (Sedoric/overlay ou ROM custom), pas le BASIC nu.

Les données volumineuses (motifs de sprites, images chunky 16000 o, plan
d'attributs 8000 cellules) se programment par flux et gagnent à être remplies en
code machine plutôt qu'en `POKE` BASIC (lent).

---

## 7. Démos prêtes à l'emploi

`demos/ula-ng/` contient une démo par feature marquante, avec un menu de
lancement (`menu.sh`) et un `README.md` détaillé :

| Démo | Feature | Source |
|---|---|---|
| `ng_chunky`     | Chunky 4bpp plein écran 320×224, 16 couleurs, palette animée | machine code |
| `ng_text80`     | Texte 80 colonnes (480 px)                                   | BASIC |
| `ng_attributes` | Mosaïque de couleur par cellule (no color clash)             | machine code |
| `ng_copper`     | Barres raster arc-en-ciel (palette par scanline)             | BASIC |
| `ng_sprite`     | Sprite 16×16 rebondissant                                    | BASIC |

```bash
make SDL2=1
demos/ula-ng/menu.sh
```

---

## 7 bis. VDU intégré (`NG_VDU` $0357)

Au lieu d'écrire les registres un par un, on peut **streamer des commandes de
style VDU** dans `$0357` ; l'interpréteur vit **dans l'ULA-NG** (le 6502 ne porte
aucun pilote). Jeu v0.1 : `20` reset, `22 n` MODE (0 std/1 chunky/2 80col),
`19 l r g b` palette, `18 a` fond couleur par cellule, `31 col row a` colorer une
cellule (sans color clash). Exemple BASIC (fond bleu/encre rouge = `$21`) :

```basic
POKE#340,78:POKE#340,71 : REM deverrouille
POKE#357,18:POKE#357,#21 : REM VDU 18, $21
```

Détails, protocole d'upload (v0.2) et état de l'art : **[VDU.md](VDU.md)**.

## 8. Référence

- `docs/ula-ng/VDU.md` — VDU intégré (port de commandes `NG_VDU`).
- `docs/ula-ng/ULA-NG-SPEC.md` — spécification complète (registres, timing,
  décisions d'implémentation, cible FPGA).
- `docs/ula-ng/AUDIT.md` — architecture et frontières « miroir FPGA ».
- `demos/ula-ng/README.md` — mode d'emploi des démos + reconstruction des `.tap`.
