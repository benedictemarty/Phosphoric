# Spécification des extensions OCULA

**Statut** : brouillon v0.7 (Sprint 43, étapes 1-5 implémentées) — vérifié
sans conflit avec le firmware officiel [sodiumlb/ocula-pivic-firmware](https://github.com/sodiumlb/ocula-pivic-firmware)
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

## Fenêtre I/O $03E0-$03E7 : identification + banking (étape 4)

Contrairement aux extensions vidéo (in-band), cette fenêtre est de
l'I/O classique dans la page $03xx — réaliste pour le matériel : l'ULA
décode déjà la page $03 (elle génère nIO), et en mode OCULA-is-the-RAM
elle voit le bus d'adresses complet et pilote le bus de données.

| Adresse | Accès | Contenu |
|---------|-------|---------|
| $03E0 (992) | R | `'O'` ($4F) — identification (pendant du `'L'` de LOCI en $0319) |
| $03E1 (993) | R | `'C'` ($43) |
| $03E2 (994) | R | capacités : bit 0 = 80 col, bit 1 = HIRES étendu, bit 2 = palette, bit 3 = banking (= $0F) |
| $03E3 (995) | R/W | **banque CPU $A000-$BFFF** (0-7, masqué sur 3 bits) |
| $03E4-$03E7 | R | réservé (lit $00 ; $03E4 réservé pour une future banque vidéo / page flipping) |

### Détection d'OCULA

`IF PEEK(992)=79 AND PEEK(993)=67 THEN` → OCULA présent. Sur un Oric
stock, $03E0-$03E7 retombe sur le miroir VIA → valeurs différentes.
**Piège BASIC** : écrire `PEEK(#3E0)` échoue, le tokenizer avale `3E0`
comme notation scientifique (3×10⁰) — utiliser les adresses décimales
992-995.

### Banking mémoire

- La fenêtre **$A000-$BFFF (8 Ko)** bascule entre la **banque 0**
  (RAM principale) et **7 banques annexes** dans la SRAM interne de
  l'OCULA → 56 Ko additionnels.
- **L'ULA balaye toujours la banque 0** : le banking est vu du CPU
  seulement. Composer une image dans une banque annexe puis la copier,
  ou attendre le registre banque vidéo ($03E4, réservé) pour du vrai
  page flipping.
- La fenêtre couvre l'écran texte ($BB80), les charsets ($B400/$B800)
  et la zone HIRES : banque ≠ 0 les masque au CPU (la ROM continue
  d'écrire « à l'aveugle » dans la banque active) — revenir en banque 0
  avant tout affichage.
- Banques annexes initialisées à zéro ; contenu persistant dans les
  savestates (section `OCB` du format .ost, présente seulement si le
  banking a servi).
- Exemple : `POKE 995,1` (banque 1) … `POKE 995,0` (retour).

## Étape 5 : OCULA-GPU

**Statut : implémentée (Sprint 43, v1.21.0-alpha)** — à discuter
upstream après retour sur les étapes 1-4 (RFC #53).

### Changement de contrat : tier 2

Les étapes 1-3 dégradent gracieusement sur ULA stock ; l'étape 4 est
détectable mais inerte. Le GPU rompt ce contrat : un programme qui
délègue son rendu ne fait rien d'utile sur un HCS 10017 — et les
écritures dans $03E8+ y retombent sur le miroir VIA (effets de bord
possibles). La spec distingue donc :

- **Tier 1** (étapes 1-3) : in-band, dégradation gracieuse, un même
  binaire tourne partout.
- **Tier 2** (étapes 4-5) : **détection préalable obligatoire**
  (`PEEK(992)=79 AND PEEK(993)=67`, puis bit 4 de $03E2 pour le GPU).

### Pourquoi c'est réaliste sur le matériel

- **RP2350B = 2 cœurs Cortex-M33** : cœur 1 au scanout DVI, cœur 0
  libre pour exécuter les commandes (file inter-cœurs).
- **OCULA-is-the-RAM** : la RAM est la SRAM interne du chip — le GPU
  lit/écrit la mémoire **sans toucher au bus Oric ni voler de cycles
  au 6502**. Exécution concurrente vraie. (DRAM-is-the-RAM : GPU
  limité au blanking, hors scope v1.)
- **L'OCULA génère PHI0** (cf. `SET PHI2` du firmware) : il peut
  **étirer l'horloge CPU** pendant une commande bloquante — la
  synchronisation sans broche IRQ (le socket ULA n'en a pas).

### Fenêtre de commande $03E8-$03EF

Prolonge la fenêtre d'identification/banking de l'étape 4.

| Adresse | Accès | Contenu |
|---------|-------|---------|
| $03E8 (1000) | W | **GPU_CMD** : opcode ; l'écriture déclenche l'exécution. Bit 7 levé = variante bloquante (étirement PHI0 jusqu'à la fin) |
| $03E9 (1001) | R | **GPU_STATUS** : $00 prêt/terminé, $01 occupé, ≥$80 code d'erreur |
| $03EA/$03EB (1002/1003) | R/W | **GPU_PTR** lo/hi : adresse du bloc d'arguments (16 octets) en RAM |
| $03EC-$03EF | — | réservé v2 (collisions sprites, compteurs) |

Capacités : **bit 4 de $03E2** = GPU présent ($03E2 passe de $0F à $1F).

### Sémantique d'exécution

- **Posted (défaut)** : écrire le bloc d'arguments, GPU_PTR, puis
  l'opcode dans GPU_CMD. STATUS passe à $01 ; le GPU copie le bloc
  d'arguments au déclenchement (le bloc est réutilisable aussitôt) ;
  STATUS retombe à $00. Le 6502 continue de tourner pendant ce temps.
- **Bloquant** : opcode | $80 → PHI0 étiré jusqu'à STATUS = $00.
  Toute la machine est suspendue (VIA et timers compris : le temps
  *relatif* est préservé, pas le temps réel). Budget maximal proposé :
  une trame par commande.
- **Banking** : les adresses $A000-$BFFF du bloc d'arguments désignent
  la **banque CPU active** ($03E3) — on peut donc composer dans une
  banque annexe puis copier vers la banque 0 visible (synergie
  étape 4).
- **Cohérence d'affichage** : les écritures GPU dans la zone balayée
  sont visibles dès la scanline suivante (tearing possible) — utiliser
  WAIT_VBL ou composer hors écran.

### Jeu de commandes v1

| Op | Nom | Bloc d'arguments | Effet |
|----|-----|------------------|-------|
| $01 | INFO | — | bloc ← [0] version GPU (=1), [1] sprites max (=0 en v1), [2] bitmask des ops supportées (=$1F), [3-15] zéro |
| $02 | FILL | dst(2) stride(1) w(1) h(1) val(1) | rectangle d'octets rempli |
| $03 | COPY | src(2) sstr(1) dst(2) dstr(1) w(1) h(1) | copie rectangulaire ; ordre choisi pour gérer le recouvrement |
| $04 | SCROLL | dx(1) dy(1) | registres persistants appliqués au fetch des modes bitmap étendus (29/31), wrap modulo 320/200 ; 0,0 = off |
| $05 | WAIT_VBL | — | toujours bloquant ($85) : CPU suspendu jusqu'au prochain blanking vertical — sync raster |
| $06-$7F | — | — | réservé v2 : LINE, sprites composités au scanout (table 8 entrées), décompression RLE, page flipping (couplé à $03E4) |

Exemple BASIC (remplir un rectangle 10×10 en HIRES étendu) :

```basic
10 REM bloc d'arguments en #0400 : dst=#A000, stride=40, w=10, h=10, val=255
20 POKE#400,0:POKE#401,160:POKE#402,40:POKE#403,10:POKE#404,10:POKE#405,255
30 POKE 1002,0:POKE 1003,4        ' GPU_PTR = #0400
40 POKE 1000,2                    ' FILL (posted)
50 IF PEEK(1001)<>0 THEN 50       ' attendre STATUS (ou POKE 1000,130 bloquant)
```

### Protections et erreurs

- **Garde mémoire basse** : le bloc d'arguments et les cibles
  FILL/COPY sous **$0400** sont rejetés (erreur $81) — protège la page
  zéro, la pile, les variables système et la page I/O $03xx (une
  lecture GPU de la page I/O déclencherait des effets de bord VIA/FDC).
- Codes d'erreur : **$80** opcode inconnu, **$81** adresse invalide,
  **$82** WAIT_VBL appelé sans le bit bloquant.
- COPY fait un snapshot complet avant d'écrire : recouvrement sûr dans
  les deux sens.
- WAIT_VBL posté pendant le blanking : retour immédiat (on y est déjà).

### Note d'émulation (Phosphoric)

Les commandes s'exécutent instantanément du point de vue du 6502
(émulateur mono-thread) : STATUS n'est observé occupé que pour
WAIT_VBL. Le matériel réel pourra être plus lent — un logiciel bien
écrit **doit** toujours poller GPU_STATUS. Pendant WAIT_VBL, Phosphoric
gèle le 6502 et tous les périphériques cadencés par PHI0 (VIA, FDC,
ACIA) tandis que l'ULA continue son balayage — conforme à l'étirement
d'horloge du matériel.

### Ordre de réalisation proposé (rapport valeur/coût)

1. **SCROLL** — quasi gratuit, débloque le défilement fluide impossible
   à 1 MHz ; 2. **WAIT_VBL** — sync raster, manque historique de
   l'Oric ; 3. **FILL/COPY** — blitter de base ; 4. v2 : sprites au
   scanout (zéro RAM Oric, zéro scintillement), LINE, page flipping.

## Implémentation de référence

- Émulateur : Phosphoric `--ula ocula` (src/video/video.c,
  `render_80col_scanline`, latch dans `video_render_scanline`)
- Tests : `make test-ocula` (tests/unit/test_ocula.c)
- Matériel : à venir (firmware RP2350B du projet OCULA)
