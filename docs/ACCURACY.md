# Niveaux de précision temporelle de Phosphoric — état des lieux

**Première version** : 2026-09-10 (1.120.0-alpha, audit) · **État courant** :
2026-09-11, 2.0.0 (V2 terminée, épics E0 à E8 livrés)

Ce document existe parce que le projet a communiqué « cycle-accurate » alors que
l'implémentation ne l'était pas au sens strict du terme. Il fixe un vocabulaire
vérifiable, classe chaque composant, et a servi de référence d'acceptation au
plan [V2](specs/V2_CYCLE_ACCURACY.md). Il est conservé **avec son historique** :
le point de départ chiffré (44,26 %) et les étapes restent lisibles, parce que
c'est la trajectoire qui rend le résultat crédible.

## Résumé (état final V2)

| Ce qui est vrai | Preuve qui le ferait tomber |
|---|---|
| Le **CPU** est exact au cycle : un accès bus par cycle, accès factices du NMOS compris, interruptions au cycle pénultième — **100,00 %** de séquence bus exacte sur 2 440 000 cas | `make test-cycle CYCLE_MAX_CASES=0`, `make test-dormann` |
| La **machine** est cadencée au cycle : `emu_cycle()` fait avancer ULA, CPU et périphériques d'exactement un cycle, **jamais à vide** | `make test-clock` (14 tests, dont compteur CPU = position raster sur une trame) |
| Le **VIA** compte au cycle : sous-dépassement `$0000 → $FFFF`, période N+2, PB7, CA2 | `make test-io` (vecteurs de timing) |
| L'**ULA** fetche une cellule de 6 pixels par cycle : une écriture en milieu de ligne n'atteint que les cellules pas encore balayées | `make test-raster-split`, `make test-clock` |
| Le **PSG** tourne à `horloge/8`, sortie intégrée (pas de repliement), étage de sortie relevé sur le schéma | `make test-audio` (mesure du signal : fréquences, enveloppe, LFSR) |
| Un **savestate** est un point de reprise exact, même pris en pleine trame | `make test-savestate-determinism` |
| À cycle égal, le **corpus** local donne la même image | `make test-corpus` |

Ce qui **n'est pas** vrai, et qu'on ne dit donc pas : « l'émulateur est exact au
cycle ». Le FDC reste N1+ (délais DRQ/INTRQ forfaitaires, image plate), le cycle
exact où l'ULA fetche la colonne 0 n'est pas calibré contre du matériel, le
demi-cycle du one-shot du VIA n'est pas représenté, et le mode cassette par
défaut reste le patch ROM. Détail composant par composant ci-dessous.

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
| **VIA 6522** (`src/io/via6522.c`) | **N3 pour les timers** (2.0.0-alpha.2) | Cadencé au cycle (`via_update()` reçoit toujours **1**, vérifié par `test-clock`) et **sous-dépassement exact** : `$0000 → $FFFF` puis cycle de rechargement, donc période **N+2** conforme (l'ancien modèle donnait N — 20 % d'erreur de fréquence à N=10). Vérifiés par vecteurs : période continue, time-out one-shot, compteur qui continue après time-out, signal carré PB7, relecture du compteur, impulsion CA2 d'un cycle, et 50 interruptions en 50 trames. Reste hors modèle : le **demi-cycle** du time-out one-shot (N+1,5 → posé à N+1 ; il faudrait le niveau N4). Conformité registre/fonction auditée (`docs/HARDWARE_CONFORMANCE.md` §2). |
| **ULA vidéo** (`src/video/video.c`) | **fetch au cycle** (2.0.0-alpha.3) ; position horizontale non calibrée | Une **cellule de 6 pixels fetchée par cycle**, à l'instant où le faisceau la lit : une écriture du CPU en milieu de ligne n'affecte plus que les cellules pas encore balayées (**splits raster**). Encre, papier et attributs texte sont un état de ligne persistant entre les cellules. Ce qui reste : le **cycle exact où la colonne 0 est fetchée** n'est pas calibré contre du matériel réel (défaut 0, réglable par `--ula-fetch-offset`) — seule la *structure* est exacte, pas le calage horizontal absolu ; bordure et blanking ne sont toujours pas rendus (224 lignes visibles sur 312) ; les modes étendus ULA-NG plein écran restent rendus par ligne. Repli : `--ula-line`. |
| **PSG AY-3-8910** (`src/audio/ay3891x.c`) | **cadencé au matériel** (2.0.0-alpha.4) | Machine cadencée à `horloge/8` = 125 kHz, le pas interne réel du chip : ton `clock/(16·TP)`, LFSR `clock/(16·NP)`, enveloppe `clock/(8·EP)` — cette dernière était **2× trop lente**. La sortie est **intégrée** sur les pas couverts par chaque échantillon : au-dessus de Nyquist le signal s'atténue au lieu de **replier** (un ton à 62,5 kHz ressortait à 18,4 kHz à pleine amplitude). Vérifié par **mesure du signal** (fréquences ±0,1 %, enveloppe ±2 %, LFSR équilibré), pas par comparaison à des octets figés. Acquis conservé : écritures registres **horodatées en cycles CPU** → digidrums. **Étage de sortie** relevé sur le schéma officiel (`docs/architecture/oric-audio-output.md`) : le mixage parallèle **moyenne** les canaux (notre somme/3 est donc juste — l'ancienne « déviation » était fausse), le seul passe-bas du circuit coupe à **37 kHz** (hors bande), et le couplage capacitif **bloque le continu** — désormais modélisé (continu +8188 → +15, signal symétrique). Hors modèle et documenté : la coupure exacte du couplage `C4` (valeur illisible sur le schéma : 2,2 nF ⇒ 4,7 kHz ou 2,2 µF ⇒ 4,7 Hz, un facteur mille), la réponse du LM386 et du haut-parleur interne. |
| **FDC WD1793** (`src/storage/disk.c`) | **N1+** (cadencé au cycle ; latence rotationnelle réelle par défaut, `LOST DATA` et write-protect modélisés depuis la 2.0.0-alpha.6 — voir `docs/HARDWARE_CONFORMANCE.md` §1) | Délais DRQ/INTRQ **forfaitaires** (ex. 60 cycles) au lieu d'être dérivés de la position rotationnelle ; modèle image plate, donc LOST DATA / CRC structurellement impossibles (`docs/HARDWARE_CONFORMANCE.md` §1). Le label « cycle-accurate » utilisé dans les CR LOCI est **abusif** — il désigne le fait d'être cadencé en cycles, pas d'être exact au cycle. |
| **Cassette** | **N3 en mode signal** (cadencée au cycle par l'horloge maître, parité de trame corrigée en 2.0.0-alpha.7) | Le chemin par défaut reste le **patch ROM** (fast-load), hors modèle temporel — choix assumé (US6.1) : même contenu chargé, 2,4× moins de cycles. |
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

**Conséquences observables de la fidélité retrouvée** : une interruption ne peut
plus être prise *avant* l'instruction en cours (le matériel n'en est pas
capable) ; une ligne qui s'active pendant le **dernier** cycle d'une instruction
est vue trop tard et n'est honorée qu'après l'instruction suivante ; `SEI` ne
protège pas l'instruction qui le suit d'une IRQ déjà pendante, et `CLI`/`PLP`
retardent symétriquement son arrivée d'une instruction.

### Les accès factices atteignent les registres à effet de bord

C'est la conséquence la plus surprenante, et elle est **voulue** : un cycle
factice est un vrai cycle de bus, et une puce ne sait pas qu'il est factice.

Cas rencontré en 2.0.0-alpha.3 : `POKE 1021,C` en BASIC écrit dans le registre de
données d'un ACIA. La routine POKE de la ROM utilise `STA (zp),Y`, dont le 6502
NMOS fait une **lecture factice de l'adresse avant d'écrire** — et lire le
registre de données d'un ACIA **consomme l'octet reçu**. Un programme d'écho
écrit en BASIC perd donc des octets, sur l'émulateur **comme sur une machine
réelle**. La trace le montre en deux lignes :

```
R $03FD $52    <- lecture factice du POKE : l'octet « R » est mangé
W $03FD $4F    <- l'écriture voulue
```

Un pilote série s'écrit donc en assembleur, avec des `STA`/`STY` **absolus**, qui
n'ont pas de cycle factice. Les mêmes précautions valent pour tout registre à
lecture destructive (ACIA 6551/6850, registres FIFO). Avant la V2, Phosphoric
n'émettait pas ces accès et laissait passer ces programmes : il était plus
permissif que le matériel.

La trace `--cycle-trace FICHIER` (une ligne par cycle : type d'accès, adresse,
donnée, registres, lignes d'interruption) sert à diffuser un écart ligne à ligne
contre un autre émulateur ou du matériel instrumenté. Les lignes marquées `i`
(cycles internes bourrés, sans adresse) n'apparaissent plus qu'avec
`--cpu-legacy` : le cœur par défaut émet un accès réel à chaque cycle.

## Formulation autorisée

État à la fin de V2-S9 (2.0.0-alpha.8) :

- ✅ **« cœur CPU exact au cycle » / « cycle-stepped CPU core »** — acquis depuis
  la v1.124.0-alpha : 100 % de séquence bus exacte sur 2 440 000 cas de l'oracle,
  interruptions au cycle pénultième.
- ✅ **« machine cadencée au cycle » / « cycle-stepped machine »** — acquis depuis
  la 2.0.0-alpha.8 : l'horloge maître fait avancer **tous** les composants d'un
  cycle par cycle, **jamais à vide** (le cycle fantôme du branchement non pris,
  qui faisait dériver l'ULA de ~410 cycles par trame, est corrigé et verrouillé
  par `test-clock`), et un savestate est un point de reprise exact
  (`test-savestate-determinism`). VIA timers, fetch ULA et PSG sont exacts **à
  leur niveau documenté dans le tableau ci-dessus**.
- ✅ « précis au cycle bus » / « bus-cycle accurate » pour la **machine entière**.
- ✅ « compteurs de cycles exacts par opcode (256/256) ».
- ❌ « cycle-accurate » **seul** ou « exacte au cycle » pour la **machine entière** :
  le FDC reste N1+ (délais DRQ/INTRQ forfaitaires), le calage horizontal de
  l'ULA n'est pas calibré contre du matériel, le demi-cycle du one-shot du VIA
  n'est pas modélisé.
- ❌ « WD1793 cycle-accurate » → dire « WD1793 cadencé en cycles, modèle image plate ».

Chaque formulation est adossée à un test qui la ferait tomber : `make test-cycle`
(oracle 65x02 + Dormann), `make test-clock` (un appel = un cycle, jamais à vide),
`make test-io` (vecteurs VIA), `make test-raster-split` (fetch ULA au cycle),
`make test-savestate-determinism`, `make test-corpus` (empreintes d'écran du
corpus local). Voir [le plan V2](specs/V2_CYCLE_ACCURACY.md).
