# Phosphoric timing-accuracy levels — current status

**First version**: 2026-09-10 (1.120.0-alpha, audit) · **Current status**:
2026-09-11, 2.0.0 (V2 complete, epics E0 to E8 delivered)

This document exists because the project advertised itself as accurate to the
cycle while the implementation was not, in the strict sense of the term. It sets
a verifiable vocabulary, ranks each component, and served as the acceptance
reference for the [V2](specs/V2_CYCLE_ACCURACY.md) plan. It is kept **with its
history**: the quantified starting point (44.26 %) and the milestones remain
readable, because it is the trajectory that makes the result credible.

## Summary (final V2 status)

| What is true | Evidence that would disprove it |
|---|---|
| The **CPU** is exact to the cycle: one bus access per cycle, NMOS dummy accesses included, interrupts at the penultimate cycle — **100.00 %** exact bus sequence over 2,440,000 cases | `make test-cycle CYCLE_MAX_CASES=0`, `make test-dormann` |
| The **machine** is clocked per cycle: `emu_cycle()` advances the ULA, CPU and peripherals by exactly one cycle, **never idle** | `make test-clock` (14 tests, including CPU counter = raster position over one frame) |
| The **VIA** counts per cycle: underflow `$0000 → $FFFF`, period N+2, PB7, CA2 | `make test-io` (timing vectors) |
| The **ULA** fetches one 6-pixel cell per cycle, after the CPU access of the same cycle (order and reference measured by Mike Brown): a write at cycle *c* reaches cell *c* and the following ones | `make test-raster-split`, `make test-clock` |
| The **PSG** runs at `clock/8`, integrated output (no aliasing), output stage taken from the schematic | `make test-audio` (signal measurement: frequencies, envelope, LFSR) |
| A **savestate** is an exact resume point, even when taken mid-frame | `make test-savestate-determinism` |
| At equal cycle count, the local **corpus** produces the same image | `make test-corpus` |

What is **not** true, and is therefore not claimed: "the emulator is exact to the
cycle". The FDC remains N1+ (flat-rate DRQ/INTRQ delays, flat image), the
half-cycle of the VIA one-shot is not represented, the default cassette mode
remains the ROM patch, and the absolute raster/CPU phase is not modelled — it
is unobservable on an unmodified ORIC (VSYNC hack not emulated). Component-by-component details below.

## Reference scale

| Level | Name | Operational definition | Test that proves it |
|--------|-----|---------------------------|--------------------|
| **N1** | Counted per instruction | The total cycle count per opcode is exact; peripherals advance in batches after the instruction. | Comparison of `cpu->cycles` with the official table. |
| **N2** | Ordered at bus-cycle level | Each **bus access** (actual read/write) lands on the right intra-instruction cycle; **internal** (non-bus) cycles are caught up by padding at the end of the instruction; IRQ sampled at instruction boundaries. | Observation of an I/O register with side effects (e.g. RMW double write on the VIA). |
| **N3** | Cycle-stepped | The machine's unit of progress is **the cycle**: on each cycle the CPU performs exactly one bus action (including dummy accesses) or one explicit internal cycle, and all peripherals advance by one cycle in lockstep. IRQ/NMI sampled at the penultimate cycle. | Comparison of a **cycle-by-cycle bus trace** against an external oracle (SingleStepTests/65x02). |
| **N4** | Sub-cycle (φ1/φ2 phases) | The cycle is subdivided; setup/hold races between cards and bus are modelled. | Race predicate + reproducible edge cases. |

## Current ranking, component by component

| Component | Actual level | What is missing for N3 |
|-----------|-------------|------------------------|
| **CPU 6502** (`src/cpu/`) | **N3 atteint** (défaut depuis v1.124.0) | Plus rien d'identifié. Séquence bus exacte **100,00 %** sur 2 440 000 cas de l'oracle 65x02 ; tous les accès factices du NMOS ; interruptions échantillonnées au **cycle pénultième** (donc drapeau I retardé de `CLI`/`SEI`/`PLP`) ; détournement NMI pendant `BRK`. Divergence assumée : les 12 opcodes **JAM** arrêtent le CPU au lieu de bloquer le bus. Le moteur historique (N2) reste disponible par `--cpu-legacy`. |
| **VIA 6522** (`src/io/via6522.c`) | **N3 pour les timers** (2.0.0-alpha.2) | Cadencé au cycle (`via_update()` reçoit toujours **1**, vérifié par `test-clock`) et **sous-dépassement exact** : `$0000 → $FFFF` puis cycle de rechargement, donc période **N+2** conforme (l'ancien modèle donnait N — 20 % d'erreur de fréquence à N=10). Vérifiés par vecteurs : période continue, time-out one-shot, rechargement du one-shot, signal carré PB7, relecture du compteur, impulsion CA2 d'un cycle, et 50 interruptions en 50 trames. **Depuis 2.6.0, comportement mesuré sur vrai 6522** (report de Neo6502Vic20, validé là-bas par 61/61 programmes de test VIC-20 de VICE à références matérielles : décompte au cycle suivant l'écriture, one-shot rechargé, mode de T2 retardé, T2 8 bits pour le SR, PB7 sans DDRB, registre à décalage) — ces programmes ne tournent pas dans la CI de Phosphoric, où les règles sont verrouillées par `test-io`. Le **demi-cycle** du time-out (N+1,5) n'est pas représenté. Conformité registre/fonction auditée (`docs/HARDWARE_CONFORMANCE.md` §2). |
| **ULA vidéo** (`src/video/video.c`) | **N3 pour le fetch** (2.0.0-alpha.3, ordre intra-cycle corrigé en 2.0.2) | Une **cellule de 6 pixels fetchée par cycle**, à l'instant où le faisceau la lit : une écriture du CPU en milieu de ligne n'affecte plus que les cellules pas encore scannées (**splits raster**). Encre, papier et attributs texte sont un état de ligne persistant entre les cellules. **Repère et ordre pris sur le matériel** (Mike Brown, *Unofficial ULA Guide* 1.02, mesures oscilloscope) : colonnes 0-39 aux counts 0-39 du compteur horizontal, blanking 40-63, sync 49-52 ; dans le cycle, le 6502 accède d'abord, l'ULA fetche ensuite le même count → une écriture au cycle *c* est vue par la cellule *c* (jusqu'en 2.0.1 : *c+1*, une cellule trop à droite). Ce qui reste : bordure et blanking non rendus (224 lignes visibles sur 312) ; phase absolue raster/CPU non modélisée (inobservable sans VSYNC hack) ; modes étendus ULA-NG plein écran rendus par ligne. Repli : `--ula-line`. |
| **PSG AY-3-8910** (`src/audio/ay3891x.c`) | **cadencé au matériel** (2.0.0-alpha.4) | Machine cadencée à `horloge/8` = 125 kHz, le pas interne réel du chip : ton `clock/(16·TP)`, LFSR `clock/(16·NP)`, enveloppe `clock/(8·EP)` — cette dernière était **2× trop lente**. La sortie est **intégrée** sur les pas couverts par chaque échantillon : au-dessus de Nyquist le signal s'atténue au lieu de **replier** (un ton à 62,5 kHz ressortait à 18,4 kHz à pleine amplitude). Vérifié par **mesure du signal** (fréquences ±0,1 %, enveloppe ±2 %, LFSR équilibré), pas par comparaison à des octets figés. Acquis conservé : écritures registres **horodatées en cycles CPU** → digidrums. **Étage de sortie** relevé sur le schéma officiel (`docs/architecture/oric-audio-output.md`) : le mixage parallèle **moyenne** les canaux (notre somme/3 est donc juste — l'ancienne « déviation » était fausse), le seul passe-bas du circuit coupe à **37 kHz** (hors bande), et le couplage capacitif **bloque le continu** — désormais modélisé (continu +8188 → +15, signal symétrique). Hors modèle et documenté : la coupure exacte du couplage `C4` (valeur illisible sur le schéma : 2,2 nF ⇒ 4,7 kHz ou 2,2 µF ⇒ 4,7 Hz, un facteur mille), la réponse du LM386 et du haut-parleur interne. |
| **FDC WD1793** (`src/storage/disk.c`) | **N1+** (cadencé au cycle ; latence rotationnelle réelle par défaut, `LOST DATA` et write-protect modélisés depuis la 2.0.0-alpha.6 — voir `docs/HARDWARE_CONFORMANCE.md` §1) | Délais DRQ/INTRQ **forfaitaires** (ex. 60 cycles) au lieu d'être dérivés de la position rotationnelle ; modèle image plate, donc LOST DATA / CRC structurellement impossibles (`docs/HARDWARE_CONFORMANCE.md` §1). Le label « cycle-accurate » utilisé dans les CR LOCI est **abusif** — il désigne le fait d'être cadencé en cycles, pas d'être exact au cycle. |
| **Cassette** | **N3 en mode signal** (cadencée au cycle par l'horloge maître, parité de trame corrigée en 2.0.0-alpha.7) | Le chemin par défaut reste le **patch ROM** (fast-load), hors modèle temporel — choix assumé (US6.1) : même contenu chargé, 2,4× moins de cycles. |
| **Bus d'extension (LOCI/MIA)** | **N4 partiel** | Grille 30 sous-ticks φ2, prédicat de course, jitter seedé (Épic B phases 1-2) — le seul endroit du projet réellement sous-cycle, mais les constantes ne sont pas calibrées sur matériel réel (phases 3-4 ouvertes). |

## How to measure (V2-S1)

The instruments have existed since v1.122.0-alpha; the vectors, large and
third-party, are not versioned:

```bash
tools/fetch_vectors.sh dormann     # ~800 KB
tools/fetch_vectors.sh 65x02       # ~1 GB, only once
make test-cycle                    # cycle-by-cycle oracle (200 cases/opcode)
make test-cycle CYCLE_MAX_CASES=0  # all 10,000 cases per opcode
make test-dormann                  # Klaus Dormann's functional test
```

Without vectors, both targets report **SKIP** (so they stay in `make tests` and
in CI). `make test-cycle` measures four distinct properties, from weakest to
strongest:

| Property | What it proves | Level |
|-----------|-------------------|--------|
| final state (registers + RAM) | the computation is correct | timing-independent |
| total cycles per instruction | the counters are exact | **N1** |
| bus subsequence | no spurious access, no order inversion | **N2** |
| exact bus sequence | one access at the right cycle, for **every** cycle | **N3** |

### V2-S1 reference score (2026-09-10, v1.122.0-alpha)

**Exhaustive** run: 244 opcodes (excluding the 12 JAM) × 10,000 cases =
**2,440,000 cases**, in 6.4 s.

| Property | Score | Reading |
|-----------|-------|---------|
| final state (registers) | **100.00 %** | 2,440,000 / 2,440,000 |
| final state (RAM) | **100.00 %** | 2,440,000 / 2,440,000 |
| total cycles | **100.00 %** | level N1 is proven |
| bus subsequence | **100.00 %** | **level N2 is proven** |
| exact bus sequence | **44.26 %** | the gap still to be closed for N3 |

244/244 non-JAM opcodes are at 100 % on state + RAM + cycles. The 44.26 % figure
is the **quantified starting point of V2**: it measures exactly what is
missing — the dummy accesses and the explicit internal cycles. The test's
`BUS_EXACT_FLOOR_BP` floor forbids any regression below this rate; V2-E1 must
raise it. The rate is stable to ±0.1 % from 100 cases per opcode onwards, so the
default sampled run (200) is enough for monitoring.

The oracle also revealed **5 real defects** in the core, fixed in the same
version (access order of `JSR`, `ADC`/`SBC` flags in decimal mode, decimal
`ARR`, unstable `SHA`/`SHX`/`SHY`/`SHS` stores on page crossing) — see the
CHANGELOG. Klaus Dormann's functional test passes in full
(`make test-dormann`), which confirms that the core's shortcoming is **temporal,
not logical**.

### Two cores, one computation (V2-S2/S3)

The CPU has **two engines** that share the same semantics (same computation
functions: flags, BCD, illegal opcodes) and differ only in the scheduling of
cycles. Since v1.124.0-alpha, the **micro-sequenced engine is the default**;
the historical one remains available through `--cpu-legacy`.

| | historical (`--cpu-legacy`) | micro-sequenced (**default**) |
|---|---|---|
| exact bus sequence (N3) | 44.26 % | **100.00 %** |
| cycles without address | 12.8 % of cycles at boot | **0** |
| NMOS dummy accesses | absent | all emitted |
| interrupt acceptance | instruction boundary | **penultimate cycle** |
| delayed I flag (`CLI`/`SEI`/`PLP`) | no | **yes** |
| NMI hijacking during `BRK` | no | **yes** |
| cost (frame, 20 ms budget) | 1.9 % | 2.5 % |

Evidence of identical integration between the two engines: byte-identical
**ORIC-1** and **Atmos** boots, **13 real programs** (6 Sedoric disks including
Citadelle, OricChess, L'Aigle d'Or, HHGG + 7 tapes including Manic Miner,
Atlantis, Acheron) identical on screen, Dormann test passed at the **same number
of cycles** (96,241,367).

**Observable consequences of the restored fidelity**: an interrupt can no longer
be taken *before* the current instruction (the hardware cannot do that); a line
that becomes active during the **last** cycle of an instruction is seen too late
and is only honoured after the next instruction; `SEI` does not protect the
instruction that follows it from an already pending IRQ, and `CLI`/`PLP`
symmetrically delay its arrival by one instruction.

### Dummy accesses reach side-effect registers

This is the most surprising consequence, and it is **intended**: a dummy cycle
is a real bus cycle, and a chip does not know that it is a dummy.

Case encountered in 2.0.0-alpha.3: `POKE 1021,C` in BASIC writes to the data
register of an ACIA. The ROM's POKE routine uses `STA (zp),Y`, for which the
NMOS 6502 performs a **dummy read of the address before writing** — and reading
the data register of an ACIA **consumes the received byte**. An echo program
written in BASIC therefore loses bytes, on the emulator **just as on a real
machine**. The trace shows it in two lines:

```
R $03FD $52    <- the POKE's dummy read: the byte "R" is swallowed
W $03FD $4F    <- the intended write
```

A serial driver must therefore be written in assembler, with **absolute**
`STA`/`STY`, which have no dummy cycle. The same precautions apply to any
register with destructive read (ACIA 6551/6850, FIFO registers). Before V2,
Phosphoric did not emit these accesses and let such programs work: it was more
permissive than the hardware.

The `--cycle-trace FICHIER` trace (one line per cycle: access type, address,
data, registers, interrupt lines) is used to diff a discrepancy line by line
against another emulator or instrumented hardware. Lines marked `i`
(padded internal cycles, without address) now only appear with
`--cpu-legacy`: the default core emits a real access on every cycle.

## Permitted wording

Status at the end of V2-S9 (2.0.0-alpha.8):

- ✅ **"CPU core exact to the cycle" / "cycle-stepped CPU core"** — achieved since
  v1.124.0-alpha: 100 % exact bus sequence over 2,440,000 oracle cases,
  interrupts at the penultimate cycle.
- ✅ **"machine clocked per cycle" / "cycle-stepped machine"** — achieved since
  2.0.0-alpha.8: the master clock advances **all** components one cycle at a
  time, **never idle** (the phantom cycle of the untaken branch, which made the
  ULA drift by ~410 cycles per frame, is fixed and locked down by `test-clock`),
  and a savestate is an exact resume point (`test-savestate-determinism`). VIA
  timers, ULA fetch and PSG are exact **at the level documented in the table
  above**.
- ✅ "bus-cycle-accurate" for the **whole machine**.
- ✅ "exact cycle counters per opcode (256/256)".
- ❌ "accurate to the cycle" **on its own** (without the "bus-" qualifier) or
  "exact to the cycle" for the **whole machine**: the FDC remains N1+ (flat-rate
  DRQ/INTRQ delays), the half-cycle of the VIA one-shot is not modelled, and
  neither is the absolute raster/CPU phase (VSYNC hack).
- ❌ "WD1793 accurate to the cycle" → say "WD1793 clocked in cycles, flat image model".

Each wording is backed by a test that would disprove it: `make test-cycle`
(65x02 oracle + Dormann), `make test-clock` (one call = one cycle, never idle),
`make test-io` (VIA vectors), `make test-raster-split` (ULA fetch per cycle),
`make test-savestate-determinism`, `make test-corpus` (screen fingerprints of the
local corpus). See [the V2 plan](specs/V2_CYCLE_ACCURACY.md).
