# ULA-NG — Guide utilisateur

ULA « next-generation » pour Phosphoric : extensions vidéo activées par
déverrouillage, indiscernables d'une HCS10017 tant que verrouillées. Référence
logicielle d'un futur portage FPGA (Sipeed Tang Primer 20K / GW2A-18).

> État : étapes 1-5 implémentées (verrou, palette, IRQ raster, start-address,
> copper). Voir `ULA-NG-SPEC.md` (spec complète) et `AUDIT.md` (architecture).

## 1. Déverrouillage

Fenêtre registres : **`$0340`-`$035F`**. Verrouillée au reset (passthrough VIA,
non-régression bit-à-bit). Séquence de déverrouillage : écrire `'N'` (`$4E`) puis
`'G'` (`$47`) dans **`$0340`**, sans autre écriture de la fenêtre entre les deux.

```asm
        LDA #$4E : STA $0340        ; 'N'
        LDA #$47 : STA $0340        ; 'G'
        LDA $0340 : CMP #$1E : BNE no_ng      ; NG_ID = version ?
        LDA $034F : EOR $0340 : CMP #$FF : BNE no_ng  ; handshake ~NG_ID
        ; ULA-NG présente et déverrouillée
no_ng:
```

## 2. Carte des registres (implémentés)

| Adr | Nom | R/W | Rôle |
|---|---|---|---|
| `$0340` | `NG_ID` (R) / `NG_LOCK` (W) | R/W | R : `$1E` si déverrouillé. W : séquence 'N','G'. |
| `$0341` | `NG_MODE` | R/W | b0 = extensions actives (gate palette/copper/scroll). |
| `$0342`-`$0343` | `NG_SCRSTART` | W | Base du fetch vidéo (LSB/MSB). `$0000` = défaut. |
| `$0346` | `NG_RASTERLINE` | W | Ligne (0-255) déclenchant l'IRQ raster. |
| `$0347` | `NG_STATUS` | R/W | R b7 = IRQ en attente. W = acquit + b0 = enable IRQ. |
| `$0348` | `NG_PAL_IDX` | W | Index LUT palette (0-15), auto-incrément. |
| `$0349` | `NG_PAL_DATA` lo | W | `0000RRRR`. |
| `$034A` | `NG_PAL_DATA` hi | W | `GGGGBBBB` (commit + incrément d'index). |
| `$034B` | `NG_COP_CTRL` | W | Réinitialise la liste copper. |
| `$034C` | `NG_COP_DATA` | W | Flux 3 o/entrée : `ligne`, `(idx<<4)|R`, `(G<<4)|B`. |
| `$034F` | `NG_IDCHK` | R | `~NG_ID` (handshake). |

## 3. Programmation directe (émulateur) : `--ula-ng-poke`

Pour tester/démontrer sans BASIC, le flag CLI programme les registres au
démarrage. `SEQ` = paires `AAA=VV` (hex) séparées par des virgules.

```bash
# Palette : couleur 7 (blanc) -> vert
./oric1-emu -r roms/basic11b.rom \
  --ula-ng-poke "340=4E,340=47,341=01,348=07,349=00,34A=F0"

# Start-address : scroll d'une rangée texte ($BB80+40 = $BBA8)
./oric1-emu -r roms/basic11b.rom \
  --ula-ng-poke "340=4E,340=47,341=01,342=A8,343=BB"

# Copper : couleur 7 rouge (ligne 0) puis bleu (ligne $1E=30) -> bandes verticales
./oric1-emu -r roms/basic11b.rom \
  --ula-ng-poke "340=4E,340=47,341=01,34B=00,34C=00,34C=7F,34C=00,34C=1E,34C=70,34C=0F"

# IRQ raster à la ligne 100 (enable) : ATTENTION, sans ISR -> boucle d'IRQ
./oric1-emu -r roms/basic11b.rom \
  --ula-ng-poke "340=4E,340=47,341=01,346=64,347=01"
```

Combinez avec `--screenshot-at CYCLES:FILE` pour capturer le rendu.

Équivalent en BASIC : `POKE` des mêmes adresses en décimal (`$0340`=832…).
Note : la limite de ligne BASIC (~80 caractères) impose de découper les longues
séquences en plusieurs lignes.

## 4. Depuis du code machine (ROM/RAM)

Écrire les registres via `STA $034x`. Exemple palette (couleur 1 -> jaune) :

```asm
        LDA #$4E : STA $0340       ; unlock
        LDA #$47 : STA $0340
        LDA #$01 : STA $0341       ; NG_MODE.b0 = extensions actives
        LDA #$01 : STA $0348       ; index 1
        LDA #$0F : STA $0349       ; R=F
        LDA #$F0 : STA $034A       ; G=F,B=0 -> jaune, commit
```

Pour l'IRQ raster, installer un ISR qui acquitte (`STA $0347`) — nécessite un
vecteur IRQ redirigeable (Sedoric/overlay ou ROM custom), pas le BASIC nu.
