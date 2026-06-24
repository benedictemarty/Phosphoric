# Démos OCULA

Petites démos mettant en évidence les capacités de l'**OCULA** (le remplacement
de l'ULA d'origine par un RP2350), émulé par Phosphoric depuis la v1.22.0-alpha.
Toutes s'exécutent sous le profil `--ula ocula`.

> **Note d'affichage** : sur certaines configs GPU/pilote, le renderer SDL
> accéléré donne une fenêtre noire. Ajoutez `--render-software` (utilisé dans
> tous les exemples ci-dessous).

## Menu interactif

Le plus simple : un menu qui liste les 4 démos et lance celle choisie.

```bash
make SDL2=1            # compile l'émulateur si besoin
demos/ocula/menu.sh
```

Options : `--scale N` (échelle SDL, défaut 2), `--rom CHEMIN` (défaut
`roms/basic11b.rom`), `--accel` (renderer accéléré au lieu du logiciel).
`Ctrl+C` dans la fenêtre d'une démo revient au menu ; `q` quitte.

## Lancer une démo manuellement

```bash
# Plasma / raster bars (palette relue par scanline)
./oric1-emu -r roms/basic11b.rom --ula ocula -t demos/ocula/oculaplasma.tap -f --render-software --scale 2

# HIRES étendu 320x200 + palette animée
./oric1-emu -r roms/basic11b.rom --ula ocula -t demos/ocula/oculahr.tap -f --render-software --scale 2

# Palette redéfinissable (cyclage par trame)
./oric1-emu -r roms/basic11b.rom --ula ocula -t demos/ocula/ocula_demo.tap -f --render-software --scale 2

# Texte 80 colonnes (le mode est forcé indépendamment du déverrouillage)
./oric1-emu -r roms/basic11b.rom --ocula-80col-basic -t demos/ocula/ocula80_demo.tap -f --render-software --scale 2
```

`-f` = fast-load (injection directe + auto-run, pas besoin de `CLOAD`).

## Les démos

| Fichier | Capacité OCULA | Type |
|---|---|---|
| `oculaplasma` | Plasma / raster bars — palette relue **à chaque scanline** | machine code |
| `oculahr` | HIRES étendu **320×200** + palette animée (paper + encre) | machine code |
| `ocula_demo` | Palette redéfinissable (cyclage d'une entrée par trame) | BASIC |
| `ocula80_demo` | Mode texte **80 colonnes** | BASIC |

### oculaplasma — plasma / raster bars
La fonctionnalité phare (fil Dbug, forum.defence-force.org t=2709) : la palette
`$BFE0-$BFE7` est relue **au début de chaque scanline**. Un simple `loop` qui
réécrit la palette en continu suffit ; comme chaque ligne échantillonne la
palette au cycle CPU où elle est peinte, on obtient un arc-en-ciel de bandes
horizontales défilantes (effet « copper »). 51 octets.

### oculahr — HIRES 320×200
Active le mode HIRES étendu (attribut 29 → `vid_mode 5`) : bitmap pur 8 px/octet,
320×200 (vs 240×200 sur ULA stock). Les couleurs sont les entrées palette 7
(encre) et 0 (paper), redéfinissables : la démo les cycle en phases opposées
→ paires complémentaires défilantes. Piège géré : les octets de valeur 24-31
seraient décodés comme attributs de mode et casseraient l'activation, d'où le
`ora #$20` sur chaque octet du bitmap.

### ocula_demo — palette redéfinissable (BASIC)
Déverrouille l'OCULA, arme la palette, et cycle une entrée depuis le BASIC
(une fois par trame) : le fond change de couleur en continu.

### ocula80_demo — texte 80 colonnes (BASIC)
Le buffer 80 colonnes vit à `$A000` (`row*80 + col`). La démo l'efface en
espaces puis y écrit des banners sur toute la largeur (480 px). À noter :
le BASIC est lent à remplir le buffer — laissez quelques secondes avant que
l'écran soit complet.

## Reconstruire les .tap

Sources BASIC (`*.bas`) → `.tap` avec l'outil `bas2tap` :

```bash
make tools
./bas2tap demos/ocula/ocula_demo.bas   -o demos/ocula/ocula_demo.tap   --auto-run
./bas2tap demos/ocula/ocula80_demo.bas -o demos/ocula/ocula80_demo.tap --auto-run
```

Sources assembleur (`*.s`, syntaxe xa65) → `.bin` → `.tap` (chargées/exécutées
en `$0500` ; **attention** : `bin2tap --start` utilise `strtol` base 0, donc
préfixer en hexadécimal avec `0x`, sinon `0500` est interprété en octal) :

```bash
xa demos/ocula/oculahr.s -o /tmp/oculahr.bin
./bin2tap /tmp/oculahr.bin --start 0x0500 --exec 0x0500 -o demos/ocula/oculahr.tap --name OCULAHR

xa demos/ocula/oculaplasma.s -o /tmp/oculaplasma.bin
./bin2tap /tmp/oculaplasma.bin --start 0x0500 --exec 0x0500 -o demos/ocula/oculaplasma.tap --name OCULAPLZ
```

## Référence

Spécification OCULA : `docs/ocula_extensions.md`. Déverrouillage opt-in des
extensions (knock `'O'`,`'C'` sur la page ROM `$FB00`), palette redéfinissable
armée par `'O'`,`'C'` à `$BFE8/$BFE9`.
