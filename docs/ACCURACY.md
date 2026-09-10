# Niveaux de précision temporelle de Phosphoric — état des lieux

**Date** : 2026-09-10 · **Version auditée** : 1.120.0-alpha

Ce document existe parce que le projet a communiqué « cycle-accurate » alors que
l'implémentation ne l'est pas au sens strict du terme. Il fixe un vocabulaire
vérifiable, classe chaque composant, et sert de référence d'acceptation au plan
[V2](specs/V2_CYCLE_ACCURACY.md).

## Échelle de référence

| Niveau | Nom | Définition opérationnelle | Test qui le prouve |
|--------|-----|---------------------------|--------------------|
| **N1** | Compté à l'instruction | Le total de cycles par opcode est exact ; les périphériques avancent par paquets après l'instruction. | Comparaison de `cpu->cycles` à la table officielle. |
| **N2** | Ordonné au cycle bus | Chaque **accès bus** (lecture/écriture réelle) tombe au bon cycle intra-instruction ; les cycles **internes** (non-bus) sont rattrapés par bourrage en fin d'instruction ; IRQ échantillonnée aux frontières d'instruction. | Observation d'un registre I/O à effet de bord (ex. double-écriture RMW sur le VIA). |
| **N3** | Pas-à-pas au cycle | L'unité d'avancement de la machine est **le cycle** : à chaque cycle le CPU fait exactement une action de bus (y compris les accès factices) ou un cycle interne explicite, et tous les périphériques avancent d'un cycle en verrou. IRQ/NMI échantillonnées au cycle pénultième. | Comparaison **trace bus cycle par cycle** contre un oracle externe (SingleStepTests/65x02). |
| **N4** | Sous-cycle (phases φ1/φ2) | Le cycle est subdivisé ; les courses setup/hold entre cartes et bus sont modélisées. | Prédicat de course + cas limites reproductibles. |

## Classement actuel, composant par composant

| Composant | Niveau réel | Ce qui manque pour N3 |
|-----------|-------------|------------------------|
| **CPU 6502** (`src/cpu/`) | **N3 atteint** (défaut depuis v1.124.0) | Plus rien d'identifié. Séquence bus exacte **100,00 %** sur 2 440 000 cas de l'oracle 65x02 ; tous les accès factices du NMOS ; interruptions échantillonnées au **cycle pénultième** (donc drapeau I retardé de `CLI`/`SEI`/`PLP`) ; détournement NMI pendant `BRK`. Divergence assumée : les 12 opcodes **JAM** arrêtent le CPU au lieu de bloquer le bus. Le moteur historique (N2) reste disponible par `--cpu-legacy`. |
| **VIA 6522** (`src/io/via6522.c`) | **cadencé au cycle** depuis la 2.0.0-alpha.1 ; arêtes internes non vérifiées | Les paquets ont disparu : `via_update()` reçoit toujours **1** cycle (vérifié par `test-clock`). Ce qui reste pour N3 : les **timings d'arête** exacts — instant de pose de l'IFR après sous-dépassement, valeur lue du compteur pendant le cycle de rechargement, cadence du registre à décalage, délais de handshake CA1/CA2/CB1/CB2 — non vérifiés contre les chronogrammes. Objet de l'épic **V2-E3**. Conformité registre/fonction déjà auditée (`docs/HARDWARE_CONFORMANCE.md` §2). |
| **ULA vidéo** (`src/video/video.c`) | **N1 (ligne)** | Rendu par **scanline de 64 cycles**, la ligne entière échantillonnée à un instant unique → une écriture en milieu de ligne s'applique à toute la ligne. Pas de modèle de *fetch* octet par cycle, pas de bordure/blanking temporisés, 224 lignes rendues sur 312. |
| **PSG AY-3-8910** (`src/audio/ay3891x.c`) | **N1 (échantillon)** | Machine d'état cadencée à **44,1 kHz** (accumulateurs de débit) au lieu de `horloge/16` = 62,5 kHz ; LFSR de bruit et enveloppe cadencés au taux d'échantillonnage. Acquis : écritures registres **horodatées en cycles CPU** (file d'événements) → digidrums corrects. |
| **FDC WD1793** (`src/storage/disk.c`) | **N1** (cadencé au cycle, modèle interne forfaitaire) | Délais DRQ/INTRQ **forfaitaires** (ex. 60 cycles) au lieu d'être dérivés de la position rotationnelle ; modèle image plate, donc LOST DATA / CRC structurellement impossibles (`docs/HARDWARE_CONFORMANCE.md` §1). Le label « cycle-accurate » utilisé dans les CR LOCI est **abusif** — il désigne le fait d'être cadencé en cycles, pas d'être exact au cycle. |
| **Cassette** | **N2 en mode signal** | `--tape-signal` échantillonne PB7 au tick bus ; le chemin par défaut reste le **patch ROM** (fast-load), hors modèle temporel. |
| **Bus d'extension (LOCI/MIA)** | **N4 partiel** | Grille 30 sous-ticks φ2, prédicat de course, jitter seedé (Épic B phases 1-2) — le seul endroit du projet réellement sous-cycle, mais les constantes ne sont pas calibrées sur matériel réel (phases 3-4 ouvertes). |

## Comment mesurer (V2-S1)

Les instruments existent depuis la v1.122.0-alpha ; les vecteurs, volumineux et
tiers, ne sont pas versionnés :

```bash
tools/fetch_vectors.sh dormann     # ~800 Ko
tools/fetch_vectors.sh 65x02       # ~1 Go, une seule fois
make test-cycle                    # oracle cycle par cycle (200 cas/opcode)
make test-cycle CYCLE_MAX_CASES=0  # les 10 000 cas par opcode
make test-dormann                  # test fonctionnel de Klaus Dormann
```

Sans vecteurs, les deux cibles se mettent en **SKIP** (elles restent donc dans
`make tests` et en CI). `make test-cycle` mesure quatre propriétés distinctes,
de la plus faible à la plus forte :

| Propriété | Ce qu'elle prouve | Niveau |
|-----------|-------------------|--------|
| état final (registres + RAM) | le calcul est juste | indépendant du timing |
| total de cycles par instruction | les compteurs sont exacts | **N1** |
| sous-séquence bus | aucun accès parasite, aucune inversion d'ordre | **N2** |
| séquence bus exacte | un accès au bon cycle, pour **chaque** cycle | **N3** |

### Score de référence V2-S1 (2026-09-10, v1.122.0-alpha)

Exécution **exhaustive** : 244 opcodes (hors les 12 JAM) × 10 000 cas =
**2 440 000 cas**, en 6,4 s.

| Propriété | Score | Lecture |
|-----------|-------|---------|
| état final (registres) | **100,00 %** | 2 440 000 / 2 440 000 |
| état final (RAM) | **100,00 %** | 2 440 000 / 2 440 000 |
| total de cycles | **100,00 %** | le niveau N1 est prouvé |
| sous-séquence bus | **100,00 %** | **le niveau N2 est prouvé** |
| séquence bus exacte | **44,26 %** | l'écart qui reste à combler pour N3 |

244/244 opcodes non-JAM sont à 100 % sur état + RAM + cycles. Le chiffre de
44,26 % est le **point de départ chiffré de la V2** : il mesure exactement ce
qui manque — les accès factices et les cycles internes explicites. Le socle
`BUS_EXACT_FLOOR_BP` du test interdit toute régression sous ce taux ; V2-E1 doit
le faire monter. Le taux est stable à ±0,1 % dès 100 cas par opcode, donc
l'exécution échantillonnée par défaut (200) suffit pour surveiller.

L'oracle a par ailleurs révélé **5 défauts réels** du cœur, corrigés dans la même
version (ordre des accès de `JSR`, drapeaux `ADC`/`SBC` en mode décimal, `ARR`
décimal, stores instables `SHA`/`SHX`/`SHY`/`SHS` en traversée de page) — voir le
CHANGELOG. Le test fonctionnel de Klaus Dormann passe intégralement
(`make test-dormann`), ce qui confirme que le déficit du cœur est **temporel, pas
logique**.

### Deux cœurs, un seul calcul (V2-S2/S3)

Le CPU a **deux moteurs** qui partagent la même sémantique (mêmes fonctions de
calcul : drapeaux, BCD, opcodes illégaux) et ne diffèrent que par
l'ordonnancement des cycles. Depuis la v1.124.0-alpha, le **micro-séquencé est
le moteur par défaut** ; l'historique reste accessible par `--cpu-legacy`.

| | historique (`--cpu-legacy`) | micro-séquencé (**défaut**) |
|---|---|---|
| séquence bus exacte (N3) | 44,26 % | **100,00 %** |
| cycles sans adresse | 12,8 % des cycles au boot | **0** |
| accès factices du NMOS | absents | tous émis |
| prise des interruptions | frontière d'instruction | **cycle pénultième** |
| drapeau I retardé (`CLI`/`SEI`/`PLP`) | non | **oui** |
| détournement NMI pendant `BRK` | non | **oui** |
| coût (trame, budget 20 ms) | 1,9 % | 2,5 % |

Preuves d'intégration à l'identique entre les deux moteurs : boots **ORIC-1** et
**Atmos** byte-identiques, **13 programmes réels** (6 disquettes Sedoric dont
Citadelle, OricChess, L'Aigle d'Or, HHGG + 7 cassettes dont Manic Miner,
Atlantis, Acheron) identiques à l'écran, test Dormann réussi au **même nombre de
cycles** (96 241 367).

**Conséquence observable de la fidélité retrouvée** : une interruption ne peut
plus être prise *avant* l'instruction en cours (le matériel n'en est pas
capable) ; une ligne qui s'active pendant le **dernier** cycle d'une instruction
est vue trop tard et n'est honorée qu'après l'instruction suivante ; `SEI` ne
protège pas l'instruction qui le suit d'une IRQ déjà pendante, et `CLI`/`PLP`
retardent symétriquement son arrivée d'une instruction.

La trace `--cycle-trace FICHIER` (une ligne par cycle : type d'accès, adresse,
donnée, registres, lignes d'interruption) sert à diffuser un écart ligne à ligne
contre un autre émulateur ou du matériel instrumenté. Les lignes marquées `i`
sont les cycles internes bourrés du niveau N2 : sans adresse aujourd'hui, elles
porteront leur accès réel quand V2-E1 aura livré le cœur micro-séquencé.

## Formulation autorisée

Tant que V2 n'a pas livré son harnais de preuve :

- ✅ **« cœur CPU exact au cycle » / « cycle-stepped CPU core »** — acquis depuis
  la v1.124.0-alpha : 100 % de séquence bus exacte sur 2 440 000 cas de l'oracle,
  interruptions au cycle pénultième. La portée doit rester **le CPU** : le reste
  de la machine n'y est pas encore.
- ✅ « précis au cycle bus » / « bus-cycle accurate » pour la **machine entière**,
  **accompagné** de la définition N2 (le VIA reçoit encore des paquets, l'ULA
  rend par ligne, le PSG tourne au taux d'échantillonnage).
- ✅ « compteurs de cycles exacts par opcode (256/256) ».
- ❌ « cycle-accurate » **seul**, « cycle exact », « cycle-par-cycle », « émulation au cycle près ».
- ❌ « WD1793 cycle-accurate » → dire « WD1793 cadencé en cycles, modèle image plate ».

La bascule de vocabulaire est conditionnée à un critère unique et mesurable :
**la suite `make test-cycle` passe** (oracle 65x02 cycle par cycle + Dormann +
vecteurs VIA + images ULA de référence). Voir [le plan V2](specs/V2_CYCLE_ACCURACY.md).
