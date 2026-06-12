# Spécification des extensions OCULA

**Statut** : brouillon v0.4 (Sprint 41) — vérifié sans conflit avec le
firmware officiel [sodiumlb/ocula-pivic-firmware](https://github.com/sodiumlb/ocula-pivic-firmware)
v0.1.4 (voir [ocula_firmware_alignment.md](ocula_firmware_alignment.md)) ;
à proposer upstream ([forum.defence-force.org t=2709](https://forum.defence-force.org/viewtopic.php?t=2709)).

Phosphoric sert de banc d'essai logiciel au remplacement de l'ULA HCS 10017
par un RP2350B. Ce document spécifie le contrat que les programmes Oric
peuvent utiliser sous le profil `--ula ocula`, conçu pour rester réalisable
sur le matériel réel.

## Principe : attributs série étendus

L'ULA ne voit pas les 8 bits bas du bus d'adresses (A0-A7) — les
extensions ne peuvent donc pas être configurées par un port I/O classique.
En revanche, l'ULA voit **tous les octets du flux vidéo** pendant le
balayage : c'est le mécanisme des attributs série de l'Oric (octets dont
les bits 6 et 5 sont à zéro).

Les extensions OCULA réutilisent les **bits « don't-care » des attributs
existants** : un programme OCULA reste exécutable sur un ULA d'origine,
où l'extension dégrade proprement vers le comportement standard.

## Attribut 25 : mode texte 80 colonnes

Le groupe d'attributs 24-31 (`val & 0x18 == 0x18`) définit le mode vidéo :
`vid_mode = val & 0x07`, avec bit 2 = HIRES, bit 1 = 50 Hz (levé = 50 Hz,
`ULA_50HZ 0x02` dans le firmware officiel). **Le bit 0 est un don't-care
sur le HCS 10017 comme dans le firmware OCULA v0.1.4** — cette spec lui
donne un sens :

| Attribut | vid_mode | HCS 10017 (stock)  | OCULA                       |
|----------|----------|--------------------|-----------------------------|
| 24       | `000`    | TEXT 60 Hz         | TEXT 40 colonnes 60 Hz      |
| **25**   | `001`    | TEXT 60 Hz (bit 0 ignoré) | **TEXT 80 colonnes** 60 Hz |
| 26       | `010`    | TEXT 50 Hz         | TEXT 40 colonnes 50 Hz      |
| **27**   | `011`    | TEXT 50 Hz (bit 0 ignoré) | **TEXT 80 colonnes** 50 Hz |
| 28       | `100`    | HIRES 60 Hz        | HIRES 60 Hz                 |
| **29**   | `101`    | HIRES 60 Hz (bit 0 ignoré) | **HIRES étendu 320×200** 60 Hz |
| 30       | `110`    | HIRES 50 Hz        | HIRES 50 Hz                 |
| **31**   | `111`    | HIRES 50 Hz (bit 0 ignoré) | **HIRES étendu 320×200** 50 Hz |

Le test 80 colonnes est `(vid_mode & 0b101) == 0b001` : les attributs
25 et 27 l'activent tous deux, le bit 1 (fréquence) restant indépendant.
Activation depuis le BASIC en PAL : `POKE #BB80,27` (27 préserve le
50 Hz ; 25 fonctionne aussi mais bascule en 60 Hz sur le matériel réel).
Retour 40 colonnes : placer l'attribut 26 dans l'écran 80 colonnes
(`POKE #A000,26`).

### Mémoire écran 80 colonnes

- **Base : $A000** (la zone qu'utilise HIRES, libre en mode texte)
- **80 octets par ligne × 28 lignes** = $A000-$A8BF (2240 octets)
- Les jeux de caractères TEXT restent en place : standard **$B400**,
  alternatif **$B800** (pas de chevauchement : l'écran s'arrête à $A8BF)
- Glyphes 6×8 pixels → résolution native **480×224**

### Sémantique

- Les attributs série (INK 0-7, attributs texte 8-15, PAPER 16-23,
  mode 24-31) fonctionnent **par colonne**, comme en 40 colonnes :
  encre/papier réarmés blanc/noir en début de chaque scanline.
- Double hauteur, clignotement, jeu alternatif et inversion (bit 7)
  fonctionnent comme en 40 colonnes.
- Le changement de mode est **latché en début de trame** : un attribut
  25/26 décodé en cours de trame prend effet à la trame suivante
  (stride du framebuffer stable sur toute une trame).
- Les 28 lignes (y compris les 3 dernières, lignes 200-223) sont lues
  depuis $A000 en mode 80 colonnes.
- Pas de mixage 80 colonnes / HIRES dans une même trame : si bit 2
  (HIRES) est levé, il est prioritaire sur le bit 0.

### Dégradation sur ULA d'origine

Sur un HCS 10017, l'attribut 25 vaut TEXT 50 Hz : l'écran reste en
40 colonnes lu depuis $BB80. Un programme peut détecter OCULA en
écrivant l'attribut 25 puis en testant le rendu… ou plus simplement via
une signature à définir (réservé : étape 4, registre d'identification).

## Attributs 29/31 : HIRES étendu 320×200 bicolore (étape 3)

Même logique que le 80 colonnes : bit 0 + bit 2 (HIRES) levés.
Le test est `(vid_mode & 0b101) == 0b101` (attributs 29 et 31, le bit 1
de fréquence restant indépendant).

- **Bitmap pur 320×200** : écran à **$A000**, 40 octets/ligne × 200
  (même empreinte mémoire que le HIRES standard, $A000-$BF3F). **Les
  8 bits de chaque octet sont des pixels** (MSB à gauche) — pas
  d'attributs série dans le bitmap, pas de bit d'inversion. Le 480 px
  initialement envisagé ne tient pas dans la fenêtre $A000-$BFFF
  (16 000 octets requis) ; le 320×200 évoqué sur le fil t=2709 tient
  exactement.
- **Couleurs** : encre = entrée 7 de la palette, papier = entrée 0 —
  redéfinissables via la palette OCULA (ci-dessous).
- **Lignes 200-223** : rangées texte standard ($BB80, lignes 25-27),
  attributs série actifs — c'est la **porte de sortie in-band** du mode
  bitmap (un attribut 24/26/28/30 y bascule le mode à la trame
  suivante).
- **Activation canonique** : `HIRES:POKE#A000,29` — l'écran HIRES doit
  être propre avant de poser l'attribut : si $A000-$BF3F contient des
  données résiduelles, leurs octets 24-31 re-décodés mi-trame reprennent
  la main sur le mode (comportement fidèle de l'ULA, identique sur
  matériel réel).
- **Dégradation** : sur ULA stock, attr 29/31 = HIRES standard 240 px.

## Palette redéfinissable (étape 3)

8 entrées **RGB332** à **$BFE0-$BFE7**, armées par les octets magiques
`'O','C'` ($4F,$43) à **$BFE8-$BFE9**. Relue à chaque début de trame ;
sans le magic (ou sous profil stock), palette Oric standard.

- S'applique à **tous les modes** sous OCULA (texte 40/80 col, HIRES
  standard et étendu) — c'est le « multi-coloric » du fil t=2709.
- $BFE0-$BFFF (32 octets au-dessus de l'écran texte) n'est **jamais
  balayé par l'ULA d'origine** : neutre sur matériel stock. Pour le
  vrai OCULA, la zone est lisible pendant le blanking (l'ULA contrôle
  le bus DRAM), y compris en mode DRAM-is-the-RAM.
- Exemple BASIC : `POKE#BFE8,79:POKE#BFE9,67` (arme 'O','C') puis
  `POKE#BFE7,224` (entrée 7 → rouge pur 0xE0). Désarmement :
  `POKE#BFE8,0`.

## Réservations (étape suivante)

- **Étape 4** : banking mémoire, registre d'identification OCULA
  (équivalent du marqueur `'L'` en $0319 de LOCI)

## Implémentation de référence

- Émulateur : Phosphoric `--ula ocula` (src/video/video.c,
  `render_80col_scanline`, latch dans `video_render_scanline`)
- Tests : `make test-ocula` (tests/unit/test_ocula.c)
- Matériel : à venir (firmware RP2350B du projet OCULA)
