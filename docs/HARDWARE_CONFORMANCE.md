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

### Corrigé (v1.98.0-alpha)
| Écart | Détail | Correctif |
|-------|--------|-----------|
| **One-shot T1/T2 gelait après timeout** | Datasheet p.8/p.9 : le compteur continue à décrémenter (seul le flag cesse de se réarmer) pour laisser l'hôte lire le temps écoulé depuis l'interruption. | Nouveau champ `t1_active/t2_active` (compteur qui compte) distinct de `t1_running/t2_running` (tir encore possible). Le décompte est gaté sur `*_active` (reste vrai après timeout) ; le tir/reload sur `*_running`. Sémantique `t1_running=false` après timeout **préservée** (savestate/control/debugger + tests existants intacts). Tests `test_timer1/2_one_shot_counter_continues`. |
| **PB7 sortie timer : condition DDRB.7 manquante** | Datasheet p.9 : PB7 n'est la sortie Timer 1 que si **DDRB.7 ET ACR.7 = 1** ; le code ne testait que ACR.7. | Gate `(acr & 0x80) && (ddrb & 0x80)` sur les 4 sites (lecture ORB, pull-low T1CH, toggle underflow, `via_get_pb7`). Test unitaire `test_pb7_timer_requires_ddrb7` + **preuve d'intégration** : capture CSAVE `--tape-out-capture` **byte-à-byte identique** au binaire HEAD (md5 égal) → aucune régression du chemin cassette. |

### Déviations assumées (restantes)
| # | Écart | Datasheet | Raison |
|---|-------|-----------|--------|
| 2 | T1 free-run : période ≈ N+1 (rechargement immédiat), `==0` déclenche 1 cycle tôt, résidu de dépassement perdu | Fig 16 : période N+2 | Le pas est groupé par instruction (choix documenté du projet, « bénéfice per-cycle quasi nul sur ORIC ») ; toucher au décompte **décalerait toutes les baselines byte-exact** (screenshots/captures calées sur le cycle) et risquerait la calibration de l'IRQ 100 Hz. |
| 4 | Écriture T1L-H (reg 7) efface le flag T1 | Fig 12/13 : seule l'écriture T1C-H (reg 5) l'efface explicitement | Comportement exact de reg 7 sur le flag **incertain** (divergence entre datasheets MOS et Rockwell) → pas de correction sans confirmation (principe : ne pas inventer). |
| 5 | RESET efface compteurs/latches/SR | La datasheet dit qu'ils sont **préservés** | Sans conséquence (état power-on indéfini) ; `test_via_reset` verrouille l'état actuel. |

---

---

## 3. PSG AY-3-8912 (`src/audio/ay3891x.c`) vs datasheet *General Instrument AY-3-8910/8912*

Rendu par accumulateurs fractionnaires à 44,1 kHz, horloge maître 1 MHz.

### Conforme (vérifié par recalcul)
- **Ton** = `clock/(16×TP)` ✓ (`tone_rate = clock/8` avec bascule = demi-cycle).
- **Enveloppe** : pas = `clock/(16×EP)`, cycle 16 états = `clock/(256×EP)` ✓.
- **LFSR bruit** 17 bits, taps bit0 ⊕ bit3 ✓.
- **Mixer** R7 : bit=1 désactive ton/bruit du canal ✓.
- Largeurs : ton 12 bits, bruit 5 bits, enveloppe 16 bits ✓ ; `TP=0→1` ✓ ;
  formes d'enveloppe 0-15 ✓ ; table de volume log 16 niveaux ✓.

### Corrigé (v1.99.0-alpha)
| Écart | Détail | Correctif |
|-------|--------|-----------|
| **Bruit 2× trop rapide** | Le générateur de bruit réutilisait `tone_rate` (`clock/8` = cadence de **bascule** du ton) pour cadencer le LFSR. Or le bruit n'a pas de bascule ÷2 : la datasheet donne `clock/(16×NP)` (même prescaler /16 que le ton). MAME modélise ce ÷2 manquant via son `prescale_noise`. | Nouveau `noise_rate = clock/16` passé à `ay_step_sample()` (distinct de `tone_rate`). Test `test_ay_noise_rate_clock_div16` (LFSR de référence sur le nombre de pas exact). **Preuve empirique** : capture `--audio-wav` — `PING` (ton) byte-à-byte identique au binaire HEAD, `EXPLODE` (bruit) diffère → seul le bruit change, aucune régression du ton/enveloppe. |

### Déviations assumées
| Écart | Raison |
|-------|--------|
| L'AY-3-8912 n'a **pas de Port B** (reg 15) mais le code le modélise (`audio.h`, init 0xFF) | Sans effet sur Oric (Port B inutilisé) ; cosmétique. |
| Sémantique R7 bit 6 (direction Port A) : le code renvoie l'entrée clavier quand bit6=1, alors que la datasheet décrit R7 bits 6/7 comme la direction des ports | **Empiriquement validé** sur Oric (clavier testé de façon extensive) → modèle Oric-spécifique délibéré ; non modifié sans confirmation (principe : ne pas inventer). |

---

## Historique
- **v1.99.0-alpha** (2026-08-12) — Correction PSG AY-3-8912 : cadence du bruit
  ramenée à `clock/(16×NP)` (était 2× trop rapide). +1 test `test-audio` (13).
  Preuve empirique `--audio-wav` (ton inchangé, bruit modifié). Déviations
  assumées : Port B absent sur 8912, sémantique R7 bit6.
- **v1.98.0-alpha** (2026-08-12) — Corrections VIA 6522 : one-shot T1/T2 qui
  continue à décrémenter après timeout (`t1_active/t2_active`), et gating DDRB.7
  sur la sortie PB7 timer. +3 tests `test-io` (46). Restent en déviation assumée
  T1 free-run N+2 (risque baselines), flag T1L-H (incertain), reset compteurs.
- **v1.97.0-alpha** (2026-08-12) — Audit WD1793 (datasheet FD179X-02) + VIA 6522
  (datasheet R6522 Rev.9). 3 corrections WD1793 (Read Address, flag T, Force
  Interrupt D0/D8) + 3 tests. Déviations VIA documentées.
