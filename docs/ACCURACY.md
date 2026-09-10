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
| **CPU 6502** (`src/cpu/`) | **N2** | Accès **factices** absents (page-cross indexé, `zp,X`, RMW `abs,X`, store `abs,X/Y`, re-fetch de branche) ; cycles internes bourrés en bloc (`cpu_step`) ; IRQ échantillonnée **avant** l'instruction, pas au cycle pénultième ; pas de sémantique retardée du drapeau I (`CLI`/`SEI`/`PLP`) ; pas de détournement NMI/BRK ; aucun oracle cycle-par-cycle dans la suite de tests. Acquis : 256/256 opcodes, double-écriture RMW, bourrage exact du total. |
| **VIA 6522** (`src/io/via6522.c`) | **N2 / N1 mixte** | `via_update(cycles)` reçoit des **paquets** (1 cycle par accès bus, puis le reste de l'instruction d'un coup) : les compteurs T1/T2 atterrissent juste, mais l'instant de pose de l'IFR à l'intérieur du paquet est approché (≤ 6 cycles). Conformité datasheet déjà auditée (`docs/HARDWARE_CONFORMANCE.md` §2). |
| **ULA vidéo** (`src/video/video.c`) | **N1 (ligne)** | Rendu par **scanline de 64 cycles**, la ligne entière échantillonnée à un instant unique → une écriture en milieu de ligne s'applique à toute la ligne. Pas de modèle de *fetch* octet par cycle, pas de bordure/blanking temporisés, 224 lignes rendues sur 312. |
| **PSG AY-3-8910** (`src/audio/ay3891x.c`) | **N1 (échantillon)** | Machine d'état cadencée à **44,1 kHz** (accumulateurs de débit) au lieu de `horloge/16` = 62,5 kHz ; LFSR de bruit et enveloppe cadencés au taux d'échantillonnage. Acquis : écritures registres **horodatées en cycles CPU** (file d'événements) → digidrums corrects. |
| **FDC WD1793** (`src/storage/disk.c`) | **N1** | Délais DRQ/INTRQ **forfaitaires** (ex. 60 cycles) au lieu d'être dérivés de la position rotationnelle ; modèle image plate, donc LOST DATA / CRC structurellement impossibles (`docs/HARDWARE_CONFORMANCE.md` §1). Le label « cycle-accurate » utilisé dans les CR LOCI est **abusif** — il désigne le fait d'être cadencé en cycles, pas d'être exact au cycle. |
| **Cassette** | **N2 en mode signal** | `--tape-signal` échantillonne PB7 au tick bus ; le chemin par défaut reste le **patch ROM** (fast-load), hors modèle temporel. |
| **Bus d'extension (LOCI/MIA)** | **N4 partiel** | Grille 30 sous-ticks φ2, prédicat de course, jitter seedé (Épic B phases 1-2) — le seul endroit du projet réellement sous-cycle, mais les constantes ne sont pas calibrées sur matériel réel (phases 3-4 ouvertes). |

## Formulation autorisée

Tant que V2 n'a pas livré son harnais de preuve :

- ✅ « précis au cycle bus » / « bus-cycle accurate », **accompagné** de la définition N2.
- ✅ « compteurs de cycles exacts par opcode (256/256) ».
- ❌ « cycle-accurate » **seul**, « cycle exact », « cycle-par-cycle », « émulation au cycle près ».
- ❌ « WD1793 cycle-accurate » → dire « WD1793 cadencé en cycles, modèle image plate ».

La bascule de vocabulaire est conditionnée à un critère unique et mesurable :
**la suite `make test-cycle` passe** (oracle 65x02 cycle par cycle + Dormann +
vecteurs VIA + images ULA de référence). Voir [le plan V2](specs/V2_CYCLE_ACCURACY.md).
