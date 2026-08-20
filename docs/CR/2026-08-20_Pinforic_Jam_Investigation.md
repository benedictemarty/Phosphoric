# CR — Jam Pinforic (chargement disque de jeu) : Phosphoric hors de cause

**Date** : 2026-08-20
**Statut** : **Clos — aucun correctif Phosphoric requis** (bug côté Pinforic, confirmé par vérif croisée Oricutron)
**Composants** : Microdisc/WD1793 (`src/storage/disk.c`), protocole `--control` (`load-disk`, `keys`), traces `--fdc-trace` / `--trace-ring`

## Contexte

Démonstration de l'interpréteur Infocom **Pinforic** (design *deux disquettes* : disque
programme `pinforic.dsk`, puis disque de jeu). Scénario :

1. Boot `pinforic.dsk` (Sedoric V3.0) → lancement `INFOCOM` → splash Pinforic
   « Insert game disk & press a key ».
2. Échange à chaud drive A : `pinforic.dsk` → `zork1.dsk`.
3. Appui touche → Pinforic lit le jeu → **le CPU jamme** (opcode illégal `$02`),
   PC ≈ `$30F0` (variable selon les runs), l'émulation s'arrête.

Hypothèse initiale (équipe) : le hot-swap laisserait l'état FDC incohérent, ou Pinforic
lirait sans **RESTORE** en supposant la tête en piste 0.

## Démarche & outils Phosphoric utilisés

- **`--control`** : `load-disk A zork1.dsk` (hot-swap déterministe), `keys`, `read`, `regs`,
  `break`, `peek disk`.
- **`FDC_TRACE=1`** (`--fdc-trace`) : séquence exacte des commandes WD1793.
- **`--trace-ring 50`** : 50 dernières instructions avant le hang (idéal pour ce type de saut).

## Preuves (mesurées)

### 1. Le FDC lit correctement — hypothèses « no RESTORE » / « état FDC » réfutées

`peek disk` avant/après `load-disk` : état inchangé (`c_track=0A`, `cur_off=0100`) — **fidèle
au matériel** (un vrai swap laisse aussi le registre de piste tel quel ; le programme doit
faire un RESTORE). Et le trace FDC après l'appui touche montre que **Pinforic FAIT le RESTORE** :

```
[FDC] seek target=0 (c_track=10 track_reg=10 data=00)   ← RESTORE piste 0
[FDC] READ c_track=0 sector=1 side=0 ok
[FDC] READ c_track=1 sector=1..9 side=0/1 ok            ← lecture séquentielle
[FDC] READ c_track=2 sector=1..9 side=0/1 ok
```

**Toutes les lectures `ok`, 0 `NOT_FOUND`.** Le hot-swap et le positionnement FDC sont corrects.

### 2. Le jam est une IRQ vecteurisée dans des données chargées (`--trace-ring`)

```
203D  STA $58              ← dernier code Pinforic légitime (page $20)
30EB  RLA ($91),Y          ← SP F8→F5 : 3 octets empilés = IRQ matériel
30ED  NOP $A0,X            ← octets 33 91 54 A0 02 = DONNÉES du jeu, pas du code
30EF  JAM ($02)            ← halt
```

Entre `$203D` et `$30EB`, **SP décrémente de 3 (PC+P)** = signature d'un **IRQ matériel**.
Le CPU sert l'interruption et **vecteurise vers `$30EB`, qui est de la donnée chargée depuis
`zork1.dsk`** → exécution de garbage → JAM. Le vecteur/handler d'IRQ a donc été **écrasé par
les données du jeu** (buffer overrun). L'IRQ tombant à un cycle variable → **PC de jam non
déterministe** (`$30EB`/`$30EF`/`$30F0`).

## Vérification croisée — Oricutron 1.2.0

Même couple `pinforic.dsk` + `zork1.dsk`, piloté dans Oricutron (menu F1 → *Insert disk 0* →
`zork1.dsk`, puis touche) :

| Émulateur | Résultat au chargement de `zork1.dsk` |
|---|---|
| **Phosphoric** | JAM — IRQ dans données chargées, `PC≈$30F0`, opcode `$02` |
| **Oricutron**  | **Crash identique** — CPU échappé en `$0002`, exécute `$FF` (illégal) ; débogueur auto sur JAM |

Écran de garbage figé identique, même mode de défaillance. L'adresse exacte varie
(`$30F0` / `$0002` / `$FFFF`) — signature du **pointeur corrompu / IRQ dans du garbage**.

## Cause racine

**Bug Pinforic** : lors du chargement de `zork1.dsk`, Pinforic écrit les données du jeu au
mauvais endroit (incompatibilité de **format/layout** entre ce binaire `INFOCOM.COM` /
`PINFORIC.BIN` et l'image `zork1.dsk` fournie — versions/placement secteur divergents),
**écrasant le vecteur/handler d'interruption**. À la première IRQ (VIA/Microdisc), le CPU
saute dans du garbage → JAM.

## Conclusion

**Phosphoric n'est pas en cause.** Le FDC WD1793 lit correctement le disque hot-swappé
(RESTORE émis par le programme, secteurs `ok`, 0 `NOT_FOUND`), et **deux émulateurs
cycle-accurate indépendants (Phosphoric, Oricutron) crashent de façon identique** →
défaut 100 % côté Pinforic (placement des données de jeu). **Aucun correctif émulateur requis.**

Les outils `--fdc-trace` et surtout `--trace-ring` ont permis d'isoler la cause en un run.

## Repro

```bash
# Phosphoric (control) :
oric1-emu --control -m atmos -r roms/basic11b.rom --disk-rom roms/microdis.rom \
          --fdc-timing fast -d pinforic.dsk --type-keys "9000000:INFOCOM\n"
#   > continue ; (au splash) load-disk A zork1.dsk ; keys \n
#   Diagnostic : FDC_TRACE=1 …  +  --trace /tmp/ring.log --trace-ring 50

# Oricutron :
oricutron -m atmos -k microdisc -w -d pinforic.dsk
#   INFOCOM ⏎ ; F1 → Insert disk 0 → zork1.dsk ; (splash) une touche ; F2 = moniteur
```
