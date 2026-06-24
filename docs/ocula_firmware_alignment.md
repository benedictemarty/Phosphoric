# Alignement Phosphoric ↔ firmware OCULA officiel

**Date d'analyse** : 2026-06-13 — firmware analysé :
[sodiumlb/ocula-pivic-firmware](https://github.com/sodiumlb/ocula-pivic-firmware)
v0.1.4 (mai 2026), fichier clé `src/firmware/oric/ula.c`.
Manuels : [wiki ocula-docs](https://github.com/sodiumlb/ocula-docs/wiki)
(Rev 1.0 prototype et Rev 1.1).

## Verdict sur notre spécification (docs/ocula_extensions.md)

### ✔ L'attribut 25 (bit 0 de vid_mode) est libre dans le firmware officiel

`ula.c` définit :

```c
#define ULA_HIRES  0x04
#define ULA_50HZ   0x02
```

Le mode est stocké sur 3 bits (`ULA_MASK_ATTRIB_VALUE 0x07`) mais **seuls
les bits 1 (50 Hz) et 2 (HIRES) sont lus**. Le bit 0 est un don't-care
dans l'OCULA réel comme dans le HCS 10017 : notre extension « attribut
25 = texte 80 colonnes » n'entre en conflit ni avec le matériel
d'origine ni avec le firmware OCULA actuel. Le splash screen officiel
écrit d'ailleurs l'attribut `0x1A` (26 = TEXT 50 Hz), cohérent avec
notre choix de 24/26 pour le retour 40 colonnes.

### ✔ Le décodage des attributs série est identique

Firmware : marqueur `(byte & 0x60) == 0`, index `(byte & 0x18) >> 3`
(INK/STYLE/PAPER/MODE), valeur `byte & 0x07`, inversion `byte & 0x80`,
papier affiché sur les colonnes d'attribut, réarmement ink=blanc /
paper=noir / style=0 en fin de ligne (compteur horizontal 64). C'est
le modèle qu'implémente Phosphoric.

### ✔ Aucun mode vidéo étendu n'existe encore côté firmware

`SET MODE` du moniteur USB ne désigne **pas** des modes vidéo mais le
mode d'installation : `0 - DRAM-is-the-RAM` (non implémenté) /
`1 - OCULA-is-the-RAM` (fonctionnel, SRAM interne + ponts MUX).
`SET DVI` choisit le timing de sortie (640x480@60 seul implémenté).
Notre spec d'extensions in-band (attributs série) est donc une
**proposition en avance sur le firmware**, orthogonale à son moniteur
USB out-of-band — à proposer upstream à sodiumlightbaby.

## Capacités du firmware réel à connaître

| Commande moniteur | Effet | Équivalent Phosphoric |
|---|---|---|
| `SET MODE 0|1` | DRAM-is-the-RAM / OCULA-is-the-RAM | (sans objet — RAM toujours interne) |
| `SET DVI n` | Timing DVI (640x480@60…) | sortie SDL2 native |
| `SET PHI2 kHz` | **Fréquence d'horloge CPU** (l'OCULA génère Phi0) | candidat backlog : option turbo |
| `SET BOOT rom` | **Remplacement de ROM** (littlefs interne, pin nROMSEL) | `--rom` |
| `SET SPLASH 0|1` | Écran d'accueil « Oric OCULA <version> » | (sans objet) |
| `SET VOLT/BIAS` | Réglages électriques RP2350 | (sans objet) |

Autre point notable : le firmware émet les données écran sur une sortie
dédiée (PIO « XULA ») pendant le balayage, **pour la détection de mode
écran par LOCI** — y compris l'algorithme d'adresse texte exact de
l'ULA pendant le blanking (`ula_text_address_algo`). À garder en tête
pour la cohérence du snoop ULA `$03A3` du mode `--loci` de Phosphoric.

## Divergences fines Phosphoric ↔ firmware (à vérifier sur matériel)

1. **Période de clignotement (FLASH)** : firmware `flashCounter & 0x20`
   → période 64 trames (~0,8 Hz) ; Phosphoric `frame_counter & 0x10`
   → 32 trames (~1,6 Hz, aligné Oricutron). À trancher sur Oric réel.
2. **Double hauteur** : firmware `(verticalCounter >> 1) & 0x7` — la
   moitié affichée dépend de la **parité de la rangée absolue** ;
   Phosphoric suit une phase chaînée (`dbl_phase`, modèle Oricutron).
   Les deux divergent pour des rangées DOUBLE non contiguës.
3. **Timing 60 Hz** : firmware implémente le 60 Hz réel (264 lignes,
   vsync 236-240) ; Phosphoric est PAL-only (312 lignes). Candidat
   backlog si des programmes testent le bit 1 de vid_mode.
4. **Reset** : démarrage horloge ~70 µs après power-on ; le circuit de
   reset Oric d'origine est limite (manuel Rev 1.1 : augmenter la
   capacité ou moderniser le reset). Sans objet pour l'émulateur.

## Actions

- [x] Spec attribut 25 confirmée sans conflit (ce document)
- [x] Proposer l'extension attribut 25/27 upstream : RFC ouverte le
      2026-06-13 → [ocula-pivic-firmware#53](https://github.com/sodiumlb/ocula-pivic-firmware/issues/53)
- [ ] Backlog : timing 60 Hz 264 lignes (vid_mode bit 1)
- [ ] Backlog : option turbo type `SET PHI2` (`--phi2 kHz`)
- [ ] Vérifier période FLASH et modèle double hauteur sur matériel réel

## Référence locale

Clone d'analyse : `~/Occula/ocula-pivic-firmware` (lecture seule,
upstream GitHub fait foi).
