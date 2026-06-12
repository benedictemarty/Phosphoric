# Spécification des extensions OCULA

**Statut** : brouillon v0.2 (Sprint 40) — à faire converger avec le projet
matériel OCULA ([forum.defence-force.org t=2709](https://forum.defence-force.org/viewtopic.php?t=2709)).

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
`vid_mode = val & 0x07`, avec bit 2 = HIRES, bit 1 = 60 Hz. **Le bit 0
est un don't-care sur le HCS 10017** — OCULA lui donne un sens :

| Attribut | HCS 10017 (stock)  | OCULA                       |
|----------|--------------------|-----------------------------|
| 24       | TEXT 50 Hz         | TEXT 40 colonnes            |
| **25**   | TEXT 50 Hz (bit 0 ignoré) | **TEXT 80 colonnes** |
| 26       | TEXT 50 Hz         | TEXT 40 colonnes            |
| 28       | HIRES 50 Hz        | HIRES (bit 0 sans effet : bit 2 prioritaire) |
| 29       | HIRES 50 Hz        | réservé étape 3 (HIRES étendu) |

Activation depuis le BASIC : `POKE #BB80,25` (l'attribut est lu au
balayage suivant). Retour 40 colonnes : placer l'attribut 24 ou 26 dans
l'écran 80 colonnes (`POKE #A000,26`).

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

## Réservations (étapes suivantes)

- **Étape 3** : attribut 29 = HIRES étendu (480 px monochrome) ;
  palette redéfinissable (séquence d'attributs à spécifier)
- **Étape 4** : banking mémoire, registre d'identification OCULA
  (équivalent du marqueur `'L'` en $0319 de LOCI)

## Implémentation de référence

- Émulateur : Phosphoric `--ula ocula` (src/video/video.c,
  `render_80col_scanline`, latch dans `video_render_scanline`)
- Tests : `make test-ocula` (tests/unit/test_ocula.c)
- Matériel : à venir (firmware RP2350B du projet OCULA)
