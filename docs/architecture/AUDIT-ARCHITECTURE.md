# Audit architectural — Phosphoric

> Audit critique du 2026-07-17 (v1.84.0-alpha). Appuyé sur des mesures réelles
> (graphe d'includes, ventilation de `main.c`, ordre des ticks, threads).
> Références `fichier:ligne` vérifiées ou marquées « approx. ». Ce document est
> volontairement **critique** : il liste ce qui va, mais surtout la dette.

## 0. Verdict en une phrase

Le **cœur** de l'émulateur est proprement modularisé (couches découplées, zéro
cycle d'includes, IRQ/timing per-cycle corrects), mais la **périphérie** est
concentrée dans un `main.c` de 4690 lignes (dont `main()` ≈ 1487) et le contrat
`io_device_t` est **incomplet** (dispatch migré, mais lifecycle et savestate
seulement partiels). La dette est *périphérique*, pas *centrale* — c'est une
bonne nouvelle : elle s'attaque par extraction, sans toucher au moteur.

## 1. Ce qui est solide (mesuré)

### 1.1 Découplage des modules de base
`cpu`, `memory`, `video`, `audio`, `storage` **n'incluent pas** `emulator.h`
(0 dépendance) : chacun opère sur sa propre struct passée en pointeur. Sur ~166
fichiers, seuls **18** incluent `emulator.h` (~11 %). C'est ce qui rend le
framework de test « un exe par module » possible.

### 1.2 Zéro cycle d'includes
Graphe en DAG. Les cycles potentiels sont cassés par **forward-declarations**
délibérées : `cpu ↔ memory` (`cpu6502.h`), `video ↔ ula_ng` (`video.h:12`),
`cassette ↔ via6522` (`cassette.h`). `io_device.h` forward-déclare `emulator_s`.

### 1.3 Fidélité temporelle per-cycle
Chaque accès mémoire du 6502 déclenche `cpu_cycle_tick()` (src/main.c ≈ 1291),
qui avance VIA, FDC, ACIA, DTL2000, Mageco **au cycle** — pas en lot post-
instruction. IRQ **level-triggered** multi-sources via bitfield `IRQF_*`
(cpu6502.h) : correct, chaque source gère son bit.

### 1.4 Concurrence défensive
3 threads auxiliaires (cast MJPEG, HTTP API, heartbeat CASTV2). **Aucun** ne mute
`emulator_t` directement : tout passe par `control_queue` (MPSC, mutex +
condition-var par commande), drainé au *frame boundary* par la boucle principale
(consommateur unique). Le framebuffer est **copié sous mutex** une fois/trame
vers le cast (`cast_server_push_frame`), jamais partagé brut. Backends série en
I/O **non-bloquant** (pas de thread). Aucune race identifiée.

### 1.5 Conventions homogènes
Include guards, `snake_case`/`_t`/`_s`, en-têtes Doxygen, K&R 4 espaces :
cohérents sur tout le dépôt.

## 2. La dette (priorisée)

### 2.1 🔴 `main.c` = god-object périphérique (4690 L, `main()` ≈ 1487 L)
`main()` fait tout : ~79 options CLI (getopt_long), init des sous-systèmes,
création des backends série (≈ 390 L pour modem/digitelec/picowifi), boucle
principale, événements SDL, touches de fonction, teardown. 79 fonctions dans un
seul fichier, dont beaucoup de **callbacks de câblage** (io_read/write_callback,
keyboard_matrix_read, les 12 wrappers `*_dev_*`, les paires `*_cpu_irq_set/clr`).

**Nuance honnête :** ce n'est pas un god-object *comportemental* (la logique vit
dans les modules) mais un god-object *d'assemblage*. Le risque est la
lisibilité/maintenabilité, pas la correction.

### 2.2 🔴 État émulé et ressource hôte mélangés dans la même struct
`dtl2000_t`, `mageco_t` (et `serial_backend_t`) mêlent, dans **une seule
struct**, des registres POD sérialisables **et** des handles non sérialisables
(`serial_backend_t* backend`, `FILE* trace`, callbacks `irq_*`, `sockfd`,
`master_fd`, handles ALSA). `pia6821_t` porte 7 pointeurs de callback,
`acia6850_t` en porte 2.

**Conséquence concrète (vécue) :** c'est ce qui a rendu le savestate
DTL2000/Mageco/LOCI **impossible proprement** — un blob corromprait les
pointeurs, et le transport (connexion TCP/PTY, octets en vol) n'est de toute
façon pas restaurable. C'est la dette structurelle la plus « rentable » à traiter.

### 2.3 🟠 Contrat `io_device_t` incomplet (lifecycle absent)
Le contrat couvre `claims/read/write/save/load` mais **pas** `init/reset/tick`.
Donc :
- le dispatch I/O est bien passé sur le bus, mais **init/reset/tick restent
  câblés en dur** dans `main.c` (`cpu_cycle_tick`, chemins de reset) ;
- `save/load` n'est implémenté que pour **l'ULA-NG** ; les 5 autres entrées de
  `io_bus[]` ont `save_tag=NULL`.

**Incohérence à assumer :** l'ordre des ticks (`VIA→cassette→microdisc→loci→acia→
dtl→mageco`) **diffère** de l'ordre de la table `io_bus[]`
(`loci→acia→mageco→microdisc→dtl→ula-ng`). Unifier tick + dispatch dans une seule
boucle exigerait de prouver l'équivalence byte-identique d'abord (le VIA doit
être tické en premier — il pilote les timers).

### 2.4 🟠 Monolithes secondaires
`debugger.c` (2382 L) et `control.c` (1290 L) sont des blocs monolithiques
(REPL de debug ; parseur + 30+ handlers de commandes + dispatch).

### 2.5 🟡 Includes `emulator.h` inutilisés (vérifié)
`src/io/microdisc.c:17` et `src/io/loci_core.c:16` incluent `emulator.h` avec
**0 usage** de `emulator_t`/`emu->`/`EMU_VERSION` → couplage inutile à retirer
(sous réserve de recompilation).

### 2.6 🟡 Résolution raster ULA-NG à la scanline
L'IRQ raster et le copper sont évalués par **scanline logique** (toutes les 64
cycles), pas mid-cycle. Acceptable pour du PAL 50 Hz, mais moins fin que l'ULA
matérielle — à documenter comme limite connue.

## 3. Ce qu'il ne faut PAS refactorer (risque > bénéfice)
- Le découplage des modules de base : déjà bon.
- La struct racine `emulator_t` (agrégation par valeur) : pattern légitime.
- La duplication des macros de test : choix « zéro dépendance » assumé.
- `emulator_run()` (boucle principale, timing-critique) : ne pas la « moderniser »
  en event-loop sans nécessité — le gain est théorique, le risque de régression
  temporelle est réel.

## 4. Zones non auditées (honnêteté)
Non couvert par cet audit, donc **non jugé** : précision cycle *intra-instruction*
(phase des accès), robustesse du gating cassette signal-mode sur PC ROM, design
détaillé du protocole de contrôle au-delà de sa forme, sécurité des serveurs
réseau exposés.

## 5. Plan d'action → voir Epic 7 (ROADMAP)
La dette étant périphérique, elle s'attaque par **extraction incrémentale** à
iso-comportement (chaque étape vérifiée byte-identique), dans l'esprit du chantier
`io_device_t` déjà mené. Voir l'Epic 7 « Assainissement architectural » dans
`ROADMAP`.
