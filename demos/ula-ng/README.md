# Démos ULA-NG

Petites démos mettant en évidence les capacités de l'**ULA-NG** — l'ULA
« next-generation » de Phosphoric, référence logicielle d'un futur portage
Verilog sur Sipeed Tang Primer 20K (GOWIN GW2A). Voir la spec
`docs/ula-ng/ULA-NG-SPEC.md`.

L'ULA-NG est **inerte au reset** (indiscernable d'une HCS10017 d'origine). Chaque
démo la **déverrouille** elle-même en écrivant la séquence `'N','G'` (`$4E`,`$47`)
sur `$0340`, puis programme les registres `$0340-$035F`. Aucune option
d'émulateur particulière n'est requise — un simple `-t <démo>.tap -f` suffit.

> **Note d'affichage** : sur certaines configs GPU/pilote, le renderer SDL
> accéléré donne une fenêtre noire. Les exemples ci-dessous utilisent
> `--render-software`.

## Menu interactif

```bash
make SDL2=1            # compile l'émulateur si besoin
demos/ula-ng/menu.sh
```

Options : `--scale N` (échelle SDL, défaut 2), `--rom CHEMIN` (défaut
`roms/basic11b.rom`), `--accel` (renderer accéléré au lieu du logiciel).
`Ctrl+C` dans la fenêtre d'une démo revient au menu ; `q` quitte.

## Lancer une démo manuellement

```bash
# Chunky 4bpp 320x200, 16 couleurs, palette animée (machine code)
./oric1-emu -r roms/basic11b.rom -t demos/ula-ng/ng_chunky.tap -f --render-software --scale 2

# Texte 80 colonnes (patiente ~30 s : le remplissage de l'écran $A000 est en BASIC)
./oric1-emu -r roms/basic11b.rom -t demos/ula-ng/ng_text80.tap -f --render-software --scale 2

# Attributs parallèles : encre recolorée sans color clash, cyclage
./oric1-emu -r roms/basic11b.rom -t demos/ula-ng/ng_attributes.tap -f --render-software --scale 2

# Copper / palette relatchée par scanline : barres raster arc-en-ciel
./oric1-emu -r roms/basic11b.rom -t demos/ula-ng/ng_copper.tap -f --render-software --scale 2

# Sprite matériel 16x16 : losange qui rebondit
./oric1-emu -r roms/basic11b.rom -t demos/ula-ng/ng_sprite.tap -f --render-software --scale 2
```

## Les démos

| Fichier | Feature (spec) | Ce qu'on voit | Source |
|---|---|---|---|
| `ng_chunky`     | Chunky 4bpp §5.8       | Image 320×200 16 couleurs (dégradé diagonal), palette animée en vagues | machine code |
| `ng_text80`     | Texte 80 colonnes §5.8 | 80 caractères par ligne (480 px), charset RAM `$B400`                   | BASIC |
| `ng_attributes` | Attributs parallèles §5.6 | Encre+papier par cellule cyclés, **sans color clash** sériel        | BASIC |
| `ng_copper`     | Copper / scanline §5.4 | Palette relatchée par ligne → barres raster arc-en-ciel                | BASIC |
| `ng_sprite`     | Sprite matériel §5.7   | Sprite 16×16 (losange) composé sur le fond, rebondissant               | BASIC |

## Piloter/injecter sans BASIC

Pour tester une feature sans écrire de programme, l'émulateur accepte
`--ula-ng-poke "AAA=VV,..."` : une séquence d'écritures hexadécimales dans les
registres `$0340-$035F` appliquée au démarrage. Exemples (combinables avec
`--screenshot-at C:FICHIER`) :

```bash
# Chunky : déverrouillage + NG_MODE=$05 + palette index 0 = magenta
./oric1-emu -r roms/basic11b.rom -n --ula-ng-poke "340=4E,340=47,341=05,348=00,349=0F,34A=0F" \
    --screenshot-at 4000000:/tmp/chunky.ppm -c 4500000

# 80 colonnes : déverrouillage + NG_MODE=$09
./oric1-emu -r roms/basic11b.rom -n --ula-ng-poke "340=4E,340=47,341=09" \
    --screenshot-at 4000000:/tmp/t80.ppm -c 4500000
```

## Reconstruire les .tap

Sources BASIC (`*.bas`) → `.tap` avec `bas2tap` :

```bash
make tools
./bas2tap demos/ula-ng/ng_text80.bas     -o demos/ula-ng/ng_text80.tap     --auto-run
./bas2tap demos/ula-ng/ng_attributes.bas -o demos/ula-ng/ng_attributes.tap --auto-run
./bas2tap demos/ula-ng/ng_copper.bas     -o demos/ula-ng/ng_copper.tap     --auto-run
./bas2tap demos/ula-ng/ng_sprite.bas     -o demos/ula-ng/ng_sprite.tap     --auto-run
```

Source assembleur (`*.s`, syntaxe xa65) → `.bin` → `.tap` (chargé/exécuté en
`$0500` ; **attention** : `bin2tap --start` utilise `strtol` base 0, donc
préfixer en hexadécimal avec `0x`) :

```bash
xa demos/ula-ng/ng_chunky.s -o /tmp/ng_chunky.bin
./bin2tap /tmp/ng_chunky.bin --start 0x0500 --exec 0x0500 -o demos/ula-ng/ng_chunky.tap --name NGCHUNKY
```

## Référence

Spécification ULA-NG : `docs/ula-ng/ULA-NG-SPEC.md`. Carte des registres
(`$0340-$035F`), séquence de déverrouillage, et détail des 8 features (§5.1
palette-indirection → §5.8 chunky/80col).
