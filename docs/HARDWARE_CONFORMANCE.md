# Conformité matérielle — audits datasheet

Ce document trace la conformité des cœurs d'émulation de Phosphoric vis-à-vis
des datasheets constructeur de référence. Chaque écart est classé **corrigé**
ou **déviation assumée** (avec la raison). Les déviations assumées sont des
choix de modélisation délibérés (modèle fonctionnel plutôt que bit-à-bit, ou
comportement verrouillé par des tests/sémantique existants) ; elles ne sont pas
des bugs oubliés.

Méthode : lecture intégrale de la datasheet, croisement ligne à ligne avec le
code, tests unitaires ajoutés pour chaque correction.

---

## 1. FDC WD1793 (`src/storage/disk.c`) vs *Western Digital FD179X-02*

Modèle **fonctionnel sur image plate** (`side·tracks·spt + track·spt + (sec-1)) × 256`),
pas bit-à-bit MFM. CRC réel, séparateur PLL, gaps et write-precomp sont donc
absents par conception.

### Conforme
Décodage registres (A1A0), RESET (sector←1, track←0), effacement INTRQ à la
lecture status / écriture commande, step-rates 6/12/20/30 ms, latence
rotationnelle 200 ms/rev, bit BUSY, patch live INDEX/TRK0 en status Type I.

### Corrigé (v1.97.0-alpha)
| Écart | Détail | Correctif |
|-------|--------|-----------|
| **Read Address → registre secteur** | La datasheet (p.13) écrit la **piste** (octet ID 1) dans le registre secteur ; le code y mettait le numéro de secteur. | `disk.c` : `fdc->sector = addr_field[0]`. Test `test_fdc_read_address_loads_track`. |
| **STEP/STEP-IN/STEP-OUT : flag T ignoré** | Table 2 (Type I bit 4 = Track Update) : le registre piste n'est mis à jour que si T=1 ; le code l'écrivait toujours. | `fdc_seek_track(...args, bool update_track)` ; Restore/Seek passent `true`, Step\* passent `(value & 0x10) != 0`. La tête (c_track) bouge toujours. Test `test_fdc_step_track_update_flag`. |
| **Force Interrupt : INTRQ inconditionnel** | Datasheet p.15 : HEX **D0** (i3-i0=0) termine la commande **sans** interruption ; seul **D8** (i3) génère un INTRQ immédiat. Le code déclenchait toujours l'INTRQ. | `if (value & 0x08) set_intrq(...)`. Tests `test_fdc_force_interrupt` (D0 = pas d'INTRQ) + `test_fdc_force_interrupt_immediate` (D8). |

### Déviations assumées
| Écart | Raison |
|-------|--------|
| Disque absent en Type II/III → RNF au lieu de NOT READY (bit 7) | Faible impact ; à traiter dans un sprint FDC dédié. |
| READ TRACK sans complétion (pas de case dans `fdc_read`) | Commande quasi inutilisée par le logiciel Oric. |
| Write-protect (S6) non modélisé | Aucune notion de WP dans le modèle d'image ; écritures toujours acceptées. |
| Multi-secteur : pas de RNF terminal en fin de piste | Terminé par Force Interrupt en pratique. |
| S3 CRC ERROR / S2 LOST DATA jamais posés | Impossibles structurellement sur image plate + I/O programmée octet-par-octet ; secteurs endommagés remontent en RNF (S4). |
| Flags C/S (side compare) et a0 (Deleted DAM) ignorés | Le side est piloté par le registre de contrôle Microdisc. |

---

## 2. VIA 6522 (`src/io/via6522.c`) vs *Rockwell R6522 Rev.9*

### Conforme
Carte des 16 registres (Table 1) ; bits IFR/IER (Fig 29) ; sémantique IER bit7
set/clear + lecture bit7=1 ; effacement des flags par accès ORA/ORB ; modes
d'interruption indépendants CA2/CB2 ; latch d'entrée (ACR 0-1) ;
handshake/pulse CA2/CB2 ; les 8 modes du registre à décalage (rotation
MSB→bit0, cadence T2=N+2, φ2=÷2, mode 4 free-run sans flag).

### Déviations assumées (non corrigées ce round — voir raison)
Ces écarts ripplent tous sur du **comportement délibérément modélisé et
verrouillé par des tests existants** ; les corriger relève d'un sprint VIA
dédié (avec réécriture de tests et vérification d'intégration), pas d'un patch
opportuniste.

| # | Écart | Datasheet | Raison du report |
|---|-------|-----------|------------------|
| 1 | One-shot T1/T2 : le compteur **gèle** après timeout | p.8/p.9 : il continue à décrémenter (seul le flag cesse) pour permettre de lire le temps écoulé | `t1_running/t2_running=false` est une sémantique utilisée par `savestate.c`, `control.c`, `debugger.c` et affirmée par `test_timer1_one_shot`/`test_timer2_one_shot`. |
| 2 | T1 free-run : période ≈ N+1 (rechargement immédiat), `==0` déclenche 1 cycle tôt, résidu de dépassement perdu | Fig 16 : période N+2 | Le pas est groupé par instruction (choix documenté du projet, « bénéfice per-cycle quasi nul sur ORIC ») ; toucher au décompte risque la calibration de l'IRQ 100 Hz. |
| 3 | Sortie PB7 par Timer 1 : condition **DDRB.7** manquante (le code ne teste que ACR.7) | p.9 : PB7 = sortie timer seulement si DDRB.7 **et** ACR.7 = 1 | En pratique nul (la ROM Oric configure DDRB.7=1 pour CSAVE) ; mais `via_get_pb7` n'est exercé que par le chemin de capture CSAVE, **non couvert par `make tests`** → correction risquée sans test d'intégration. |
| 4 | Écriture T1L-H (reg 7) efface le flag T1 | Fig 12/13 : seule l'écriture T1C-H (reg 5) doit l'effacer | Comportement exact de reg 7 sur le flag **incertain** selon les sources → pas de correction sans confirmation (principe : ne pas inventer). |
| 5 | RESET efface compteurs/latches/SR | La datasheet dit qu'ils sont **préservés** | Sans conséquence (état power-on indéfini) ; `test_via_reset` verrouille l'état actuel. |

---

## Historique
- **v1.97.0-alpha** (2026-08-12) — Audit WD1793 (datasheet FD179X-02) + VIA 6522
  (datasheet R6522 Rev.9). 3 corrections WD1793 (Read Address, flag T, Force
  Interrupt D0/D8) + 3 tests. Déviations VIA documentées.
