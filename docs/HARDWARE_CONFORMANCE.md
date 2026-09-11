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

### Corrigé (2.0.0-alpha.6, épic V2-E6)
| Écart | Détail | Correctif |
|-------|--------|-----------|
| **LOST DATA (S2) jamais posé** | À 250 kbit/s en MFM, un octet défile toutes les **32 µs** : au-delà, un DRQ non servi signifie que l'octet suivant est déjà là. Le WD1793 lève S2 et poursuit — c'est ainsi qu'un logiciel sait qu'il a raté le train. Le bit n'était jamais levé. | Chien de garde sur l'âge du DRQ (`FDC_BYTE_CYCLES` = 32, justifié par le débit) : au-delà d'un temps d'octet, S2 est levé et compté (`lost_data_count`, rapporté en fin de session). Tests `test_fdc_lost_data_when_cpu_too_slow` et sa contre-épreuve `test_fdc_no_lost_data_when_cpu_keeps_up`. Vérifié : **aucun faux positif** sur les 6 disquettes du corpus. |
| **Write-protect (S6) non modélisé** | Les écritures étaient toujours acceptées, même sur un support protégé. | `fdc_set_write_protect()` : Write Sector et Write Track sont refusées sans rien modifier, statut bit 6 + interruption ; le bit apparaît aussi dans le statut Type I. Câblé sur `--disk-write-protect` **et déduit du fichier lui-même** (un `.dsk` en lecture seule sur l'hôte se comporte comme une disquette dont la languette est ouverte). 3 tests. |
| **Multi-secteur : pas de RNF terminal** | Une commande multi-secteur s'arrêtait silencieusement en fin de piste. Le WD1793 continue de chercher le secteur suivant et termine sur **RECORD NOT FOUND** : c'est ce qui indique au logiciel où la piste s'achève. | RNF ajouté au statut de fin, en lecture comme en écriture. |
| **Disque absent en Type II/III → RNF au lieu de NOT READY** | — | Déjà corrigé avant ce sprint (`fdc_not_ready()`, appelé par les quatre commandes Type II/III) ; la ligne de ce document était **périmée**. |

### Déviations assumées
| Écart | Raison |
|-------|--------|
| **L'octet perdu n'est pas réellement perdu** : S2 est signalé au bon moment, mais les données restent intactes | Notre modèle d'image plate n'a pas de flux MFM continu : les octets sont servis à la demande du CPU, pas par la rotation. Le logiciel qui teste S2 voit la bonne condition ; celui qui l'ignore obtient des données correctes là où le matériel lui en donnerait de fausses. Faire défiler réellement le flux exige le modèle de piste MFM (backlog V2-E6/US6.3) — et ferait courir un risque au chargement disque pour un gain théorique : aucun logiciel Oric connu n'est trop lent. |
| READ TRACK sans complétion (pas de case dans `fdc_read`) | Commande quasi inutilisée par le logiciel Oric, et une implémentation correcte demanderait de **synthétiser une piste MFM** (gaps, marques d'adresse) — c'est le modèle de piste du backlog. |
| S3 CRC ERROR jamais posé | Impossible structurellement sur image plate ; les secteurs endommagés remontent en RNF (S4). |
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
| **PB7 sortie timer : condition DDRB.7 manquante** | Datasheet p.9 : PB7 n'est la sortie Timer 1 que si **DDRB.7 ET ACR.7 = 1** ; le code ne testait que ACR.7. | Gate `(acr & 0x80) && (ddrb & 0x80)` sur les 4 sites (lecture ORB, pull-low T1CH, toggle underflow, `via_get_pb7`). Preuves : **test unitaire `test_pb7_timer_requires_ddrb7`** (DDRB.7=0 → pin normal ; =1 → sortie timer) **+ preuve d'intégration forte** (v1.99.1) : le roundtrip CSAVE→`--tape-out-capture`→CLOAD passe de bout en bout (`test-tape-roundtrip`), ce qui exerce réellement la sortie PB7 Timer 1 sous le gate DDRB.7 pendant une vraie CSAVE ROM. |

### Corrigé (2.0.0-alpha.2, épic V2-E3)
| Écart | Détail | Correctif |
|-------|--------|-----------|
| **Période du Timer 1 trop courte de 2 cycles** (ancienne déviation assumée n° 2) | Fig 16 : période **N+2**. Le sous-dépassement du 6522 n'est pas l'atteinte de zéro mais le passage de `$0000` à `$FFFF` (un cycle plus tard), et le rechargement depuis le latch consomme encore un cycle. Le code tirait dès zéro et rechargeait dans le même cycle : période **N**. Erreur de 0,02 % à 100 Hz, mais **20 % pour N=10** — audible sur les sons courts et les digidrums. | Décompte **cycle par cycle** avec sous-dépassement sur `$0000 → $FFFF` et nouveau champ `t1_reload` pour le cycle de rechargement (`via6522.c`). Idem Timer 2 (sans rechargement). **La raison invoquée pour ne pas corriger s'est révélée infondée** : les baselines byte-exact sont intactes — boot Atmos identique, **13 programmes réels** (6 disquettes, 7 cassettes) identiques à l'écran, `make tests` intégralement vert dont le roundtrip cassette qui exerce PB7 à 416/624 cycles. Ce qui a rendu la correction sûre : la machine avance désormais cycle par cycle (épics V2-E1/E2), donc le timer n'est plus approché à ±6 cycles par les paquets. Vecteurs : `test_via_t1_freerun_period_is_n_plus_2` (N = 1, 2, 5, 10, 100, 999, 9998), `test_via_t1_oneshot_timeout_cycle`, `test_via_pb7_square_wave_period`, `test_via_t1_counter_readback`, `test_via_ca2_pulse_lasts_one_cycle`, et l'intégration `test_via_t1_frame_rate_over_50_frames` (exactement 50 interruptions en 50 trames — un seul cycle de dérive la ferait échouer). |

### Déviations assumées (restantes)
| # | Écart | Datasheet | Raison |
|---|-------|-----------|--------|
| 2b | Le demi-cycle du time-out one-shot (N+1,5) n'est pas modélisé : le flag est posé au cycle N+1 | « N+1,5 cycles après l'écriture de T1C-H » | Un demi-cycle n'est pas représentable à la granularité du cycle entier (il faudrait le niveau N4, cf. `docs/ACCURACY.md`). La **période** du mode continu, elle, est exacte — c'est elle qui fixe les fréquences. |
| 4 | Écriture T1L-H (reg 7) efface le flag T1 | Fig 12/13 : seule l'écriture T1C-H (reg 5) l'efface explicitement | Comportement exact de reg 7 sur le flag **incertain** (divergence entre datasheets MOS et Rockwell) → pas de correction sans confirmation (principe : ne pas inventer). |
| 5 | RESET efface compteurs/latches/SR | La datasheet dit qu'ils sont **préservés** | Sans conséquence (état power-on indéfini) ; `test_via_reset` verrouille l'état actuel. |

---

---

## 3. PSG AY-3-8912 (`src/audio/ay3891x.c`) vs datasheet *General Instrument AY-3-8910/8912*

Depuis la 2.0.0-alpha.4 : machine cadencée à **clock/8** (125 kHz), sortie
**intégrée** sur les pas couverts par chaque échantillon (avant : accumulateurs
fractionnaires au taux d'échantillonnage de 44,1 kHz).

### Conforme (vérifié par la MESURE du signal produit)
- **Ton** = `clock/(16×TP)` ✓ — mesuré à ±0,1 % pour TP = 4, 8, 16, 50, 100, 284, 500.
- **Enveloppe** : pas = `clock/(8×EP)`, cycle de **32** états = `clock/(256×EP)` ✓ —
  mesuré à ±2 % pour EP = 100, 200, 500, 1000.
- **LFSR bruit** 17 bits, taps bit0 ⊕ bit3 ✓ (séquence comparée pas à pas à une
  référence ; jamais bloqué à zéro ; sortie équilibrée à ±5 %).
- **Mixer** R7 : bit=1 désactive ton/bruit du canal ✓.
- Largeurs : ton 12 bits, bruit 5 bits, enveloppe 16 bits ✓ ; `TP=0→1` ✓ ;
  formes d'enveloppe 0-15 ✓ ; table de volume log 16 niveaux ✓.

### Corrigé (2.0.0-alpha.4, épic V2-E5)
| Écart | Détail | Correctif |
|-------|--------|-----------|
| **Enveloppe 2× trop lente** | Un pas tous les `clock/(16×EP)` au lieu de `clock/(8×EP)`. Ce document la déclarait pourtant **conforme** : son recalcul supposait un cycle de **16 états**, alors que le compteur d'enveloppe en a **32**. Avec 32 états, `clock/(16×EP)` par pas donne un cycle en `clock/(512×EP)` — deux fois trop lent par rapport aux `clock/(256×EP)` de la datasheet. **Une erreur d'hypothèse invisible au recalcul, révélée par la mesure du signal.** | Compteur d'enveloppe cadencé à l'horloge interne `clock/8`, un pas tous les `EP`. Mesuré : durée de décroissance conforme à ±2 % sur EP = 100…1000 (test `test_ay_envelope_period_matches_datasheet`). |
| **Repliement au-dessus de Nyquist** | Les compteurs étaient cadencés par accumulateurs au taux d'échantillonnage : toute transition plus rapide que 44,1 kHz se repliait en bruit audible au lieu de s'atténuer. Mesuré : un ton à TP=1 (62,5 kHz) ressortait à 18,4 kHz avec sa pleine amplitude. | Machine cadencée à `clock/8` et sortie **intégrée** sur les pas couverts par chaque échantillon (filtre boîte). TP=1 s'atténue désormais (RMS divisé par ~3) au lieu de replier — ce que fait aussi le haut-parleur d'un vrai ORIC. Test `test_ay_no_aliasing_above_nyquist`. |

### Corrigé (v1.99.0-alpha)
| Écart | Détail | Correctif |
|-------|--------|-----------|
| **Bruit 2× trop rapide** | Le générateur de bruit réutilisait `tone_rate` (`clock/8` = cadence de **bascule** du ton) pour cadencer le LFSR. Or le bruit n'a pas de bascule ÷2 : la datasheet donne `clock/(16×NP)` (même prescaler /16 que le ton). MAME modélise ce ÷2 manquant via son `prescale_noise`. | Nouveau `noise_rate = clock/16` passé à `ay_step_sample()` (distinct de `tone_rate`). Test `test_ay_noise_rate_clock_div16` (LFSR de référence sur le nombre de pas exact). **Preuve empirique** : capture `--audio-wav` — `PING` (ton) byte-à-byte identique au binaire HEAD, `EXPLODE` (bruit) diffère → seul le bruit change, aucune régression du ton/enveloppe. |

### L'étage de sortie de l'ORIC (relevé sur le schéma officiel)

Ces valeurs viennent du schéma Oric-1/Atmos (feuille *PSG 1 Keyboard*, `audio.sch`,
révision Issue 6.1) — voir `docs/architecture/oric-audio-output.md` pour le détail
et les calculs. Elles ont permis de **lever deux déviations** précédemment
« assumées », et d'en préciser une troisième.

| Élément | Valeur | Rôle | Modélisé ? |
|---------|--------|------|------------|
| `R4` | 1 kΩ | charge commune sur laquelle CH_A, CH_B et CH_C sont **reliés ensemble** | **oui** — c'est ce mixage parallèle qui *moyenne* les trois canaux |
| `R2` / `R3` | 4,7 kΩ / 470 Ω | diviseur de tension (−20,8 dB) | non — c'est du **gain**, repris par l'ampli LM386 (×20) |
| `C5` | 10 nF | passe-bas sur `R2 ∥ R3` = 427 Ω → **f_c = 37,2 kHz** | non — **hors bande** : au-dessus de Nyquist à 44,1 kHz, et l'intégration par échantillon couvre déjà cette zone |
| `C4` | *ambiguë* | couplage vers l'ampli → **bloque le continu** | **oui**, pour le blocage du continu seulement (voir ci-dessous) |
| `LM386` + `SP1` | — | ampli et haut-parleur interne | non — réponse non mesurée |

### Corrigé (2.0.0-alpha.5)
| Écart | Détail | Correctif |
|-------|--------|-----------|
| **Composante continue en sortie** | Le signal du PSG est unipolaire (0 → +max) : mesuré, un continu de **+8188** pour trois canaux à plein volume, soit la moitié de l'amplitude. Sur la machine, `C4` couple le PSG à l'ampli et ne laisse **pas** passer ce continu. On envoyait donc au DAC un décalage permanent — dynamique gaspillée et « clic » à chaque début et fin de son. | Blocage du continu en virgule fixe (estimateur Q16, constante de temps 4096 échantillons ≈ **1,7 Hz**, donc inaudible), désarmable par `dc_block_off` pour observer le générateur nu. Mesuré après correction : continu **+8188 → +15**, et signal **symétrique** (−8232 / +8252 au lieu de 0 / +16383). Tests `test_ay_output_has_no_dc_offset`, `test_ay_dc_block_preserves_audio`. |

### Déviations levées (2.0.0-alpha.5)
| Ancienne déviation | Verdict |
|--------------------|---------|
| « Mixage des trois canaux par somme divisée par 3 — le vrai AY somme des **courants**, la somme n'est pas parfaitement linéaire » | **Infondée.** Le schéma montre CH_A/CH_B/CH_C **reliés ensemble** sur `R4` : en parallèle, les sorties ne s'additionnent pas, elles se **moyennent**. La mesure rapportée sur le forum Defence Force le confirme : un canal seul à 1 V donne ≈ 0,33 V, pas 1 V. Notre `somme / 3` **est** ce comportement. Verrouillé par `test_ay_parallel_mixing_averages_channels` (la dynamique de 3 canaux vaut 3× celle d'un seul, à 5 % près). |
| « Pas de filtre passe-bas analogique » | **Sans objet à 44,1 kHz.** Le seul passe-bas du circuit (`C5` sur `R2 ∥ R3`) coupe à **37,2 kHz**, au-dessus de la bande représentable. |

### Déviations assumées
| Écart | Raison |
|-------|--------|
| Le **couplage `C4` n'est pas modélisé comme passe-haut audible** : seul le blocage du continu l'est, à 1,7 Hz | La valeur de `C4` n'est **pas déterminable** sur ce document : elle y est notée « 2k2 », sans unité, alors que les autres condensateurs de la feuille portent la leur (`10n`, `47n`, `220uF`). Les deux lectures plausibles donnent des circuits très différents — **2,2 nF → passe-haut à ≈ 3,3 kHz** (le son perdrait tous ses graves) ou **2,2 µF → ≈ 3,3 Hz** (simple blocage du continu). On ne tranche pas au jugé : seul l'effet **certain** est modélisé. Une mesure sur machine réelle (ou une photo du PCB) lèverait le doute. |
| Réponse du LM386 et du haut-parleur interne | Non mesurée. Ce que l'émulateur restitue est le **signal électrique**, pas la réponse du petit haut-parleur — c'est aussi ce qu'attend quiconque écoute au casque. |
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
