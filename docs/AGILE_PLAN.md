# PLAN AGILE - Phosphoric
## Document de Planification Agile Complet

**Projet**: Phosphoric — Emulateur ORIC-1/Atmos précis au cycle bus
**Version du document**: 1.0.0
**Date de création**: 2026-02-22
**Responsable**: bmarty <bmarty@mailo.com>
**Méthodologie**: Scrum / Agile
**Durée des sprints**: 2 semaines

---

## Vision Produit

> Créer Phosphoric, un émulateur ORIC-1/Atmos précis au cycle bus, écrit en C,
> intégrant des fonctionnalités modernes (partage de fichiers hôte, outils de
> conversion) pour préserver et faire revivre l'écosystème logiciel de l'ORIC (1983).

---

## Product Backlog - Vue d'ensemble des Epics

| # | Epic | Priorité | Phase | Sprints | Version cible |
|---|------|----------|-------|---------|---------------|
| E0 | Infrastructure & Build | Critique | 0 | S0 | 0.1.0-alpha |
| E1 | CPU 6502 | Critique | 1 | S1-S2 | 0.2.0-alpha |
| E2 | Système Mémoire | Critique | 1 | S3 | 0.3.0-alpha |
| E3 | Système I/O (VIA 6522) | Critique | 1 | S3-S4 | 0.3.0-alpha |
| E4 | Système Vidéo | Haute | 2 | S5-S6 | 0.4.0-alpha |
| E5 | Système Audio (AY-3-8910) | Haute | 2 | S7 | 0.5.0-alpha |
| E6 | Stockage Cassette (.TAP) | Haute | 3 | S8 | 0.6.0-alpha |
| E7 | Stockage Disque (Sedoric) | Moyenne | 3 | S9 | 0.7.0-alpha |
| E8 | Système de Fichiers Hôte | Moyenne | 3 | S10 | 0.8.0-alpha |
| E9 | Outils de Conversion | Moyenne | 4 | S11 | 0.9.0-alpha |
| E10 | Débogueur & Outils Dev | Basse | 4 | S12 | 0.9.5-beta |
| E11 | Optimisation & Stabilisation | Haute | 5 | S13-S14 | 0.9.9-rc |
| E12 | Release v1.0.0 | Critique | 5 | S15 | 1.0.0 |
| E13 | Extensions Post-Release | Basse | 6+ | S16+ | 1.x.x |

---

## Diagramme de dépendances entre Epics

```
E0 (Infrastructure)
 └──► E1 (CPU 6502)
       ├──► E2 (Mémoire)
       │     ├──► E3 (I/O VIA 6522)
       │     │     ├──► E4 (Vidéo)
       │     │     ├──► E5 (Audio)
       │     │     └──► E6 (Stockage Cassette)
       │     │           └──► E7 (Stockage Disque)
       │     └──► E8 (HostFS)
       └──► E10 (Débogueur)
 E6 + E7 ──► E9 (Outils Conversion)
 E4 + E5 + E6 + E7 + E8 ──► E11 (Optimisation)
 E11 ──► E12 (Release)
 E12 ──► E13 (Extensions)
```

---

# EPIC E0 : Infrastructure & Build System

**Priorité**: Critique
**Statut**: En cours (30%)
**Version cible**: 0.1.0-alpha
**Sprint**: S0 (Sprint 0)

## Description
Mise en place de l'infrastructure du projet : dépôt Git, système de build,
structure de répertoires, documentation agile, CI/CD.

## User Stories

### US-E0-01 : Structure du projet
> En tant que développeur, je veux une structure de projet modulaire pour
> organiser le code par composant (CPU, mémoire, I/O, vidéo, audio, stockage).

**Story Points**: 3
**Statut**: Done

**Critères d'acceptation**:
- [x] Répertoires src/, include/, tests/, tools/, docs/, roms/ créés
- [x] Sous-répertoires par module (cpu, memory, io, video, audio, storage, hostfs, utils)
- [x] Headers et fichiers source squelettes en place

**Tâches**:
- [x] T-001 : Créer l'arborescence de répertoires
- [x] T-002 : Créer les fichiers header (.h) avec interfaces
- [x] T-003 : Créer les fichiers source (.c) squelettes
- [x] T-004 : Créer le .gitignore

### US-E0-02 : Système de build
> En tant que développeur, je veux un système de build fiable pour compiler
> l'émulateur et les tests rapidement.

**Story Points**: 5
**Statut**: En cours

**Critères d'acceptation**:
- [x] Makefile fonctionnel (compilation sans CMake)
- [x] CMakeLists.txt configuré
- [ ] Compilation sans warnings (-Wall -Wextra -Wpedantic)
- [ ] Targets : all, tests, tools, clean, coverage
- [ ] Build debug et release

**Tâches**:
- [x] T-005 : Créer le Makefile
- [x] T-006 : Créer CMakeLists.txt
- [ ] T-007 : Corriger les warnings de compilation (12 warnings actuels)
- [ ] T-008 : Ajouter target coverage dans Makefile

### US-E0-03 : Documentation agile
> En tant que chef de projet, je veux une documentation agile complète pour
> suivre l'avancement du projet.

**Story Points**: 3
**Statut**: Done

**Critères d'acceptation**:
- [x] README.md complet
- [x] CHANGELOG initialisé
- [x] ROADMAP définie
- [x] VERSION_TRACKING en place
- [x] CIRRUS_OS status file
- [x] AGILE_PLAN (ce document)

**Tâches**:
- [x] T-009 : Rédiger README.md
- [x] T-010 : Créer CHANGELOG
- [x] T-011 : Créer ROADMAP
- [x] T-012 : Créer VERSION_TRACKING
- [x] T-013 : Créer CIRRUS_OS
- [x] T-014 : Créer AGILE_PLAN.md

### US-E0-04 : Dépôt Git
> En tant que développeur, je veux un dépôt Git configuré avec un remote
> pour versionner et sauvegarder le code.

**Story Points**: 2
**Statut**: Partiel

**Critères d'acceptation**:
- [x] Dépôt local initialisé
- [x] Configuration user/email (bmarty / bmarty@mailo.com)
- [ ] Remote configuré et fonctionnel
- [ ] Branche main comme branche principale
- [ ] Convention de commit définie

**Tâches**:
- [x] T-015 : Initialiser le dépôt Git
- [ ] T-016 : Configurer le remote
- [ ] T-017 : Fusionner master vers main
- [ ] T-018 : Définir convention de nommage des branches

### US-E0-05 : Framework de tests
> En tant que développeur, je veux un framework de tests unitaires et
> d'intégration pour valider chaque composant.

**Story Points**: 3
**Statut**: Partiel

**Critères d'acceptation**:
- [x] Structure tests/unit/ et tests/integration/
- [x] Tests CPU basiques (init, reset)
- [x] Tests mémoire basiques (init, read/write)
- [x] Tests I/O basiques (init)
- [ ] Macro ASSERT personnalisée avec messages clairs
- [ ] Rapport de tests automatisé
- [ ] Couverture de code mesurable

**Tâches**:
- [x] T-019 : Créer la structure de tests
- [x] T-020 : Écrire test_cpu.c initial
- [x] T-021 : Écrire test_memory.c initial
- [x] T-022 : Écrire test_io.c initial
- [ ] T-023 : Créer un mini-framework de test (assert macros, reporting)
- [ ] T-024 : Intégrer gcov/lcov pour la couverture

### Definition of Done - Epic E0
- [x] Structure projet complète
- [x] Build fonctionnel
- [ ] 0 warnings de compilation
- [x] Tests de base passent
- [x] Documentation agile en place
- [ ] Remote Git configuré

---

# EPIC E1 : CPU 6502 - Coeur du processeur

**Priorité**: Critique (bloquant pour tous les autres epics)
**Statut**: A faire
**Version cible**: 0.2.0-alpha
**Sprints**: S1, S2

## Description
Implémentation complète et précise au cycle bus du processeur MOS Technology
6502 cadencé à 1 MHz tel qu'utilisé dans l'ORIC-1 (comptage de cycles exact
par opcode et accès bus cadencés au bon cycle ; les cycles internes non-bus
sont rattrapés en fin d'instruction plutôt que micro-cyclés). C'est le composant le plus
critique de l'émulateur.

## User Stories

### US-E1-01 : Modes d'adressage
> En tant que développeur, je veux les 13 modes d'adressage du 6502
> implémentés pour que les opcodes puissent résoudre leurs opérandes.

**Story Points**: 8
**Statut**: A faire
**Sprint**: S1

**Critères d'acceptation**:
- [ ] Les 13 modes d'adressage fonctionnels
- [ ] Tests unitaires pour chaque mode
- [ ] Gestion du page boundary crossing (cycle supplémentaire)

**Modes d'adressage**:
| # | Mode | Syntaxe | Exemple |
|---|------|---------|---------|
| 1 | Implicit | impl | `CLC` |
| 2 | Accumulator | A | `ASL A` |
| 3 | Immediate | #val | `LDA #$42` |
| 4 | Zero Page | zpg | `LDA $42` |
| 5 | Zero Page,X | zpg,X | `LDA $42,X` |
| 6 | Zero Page,Y | zpg,Y | `LDX $42,Y` |
| 7 | Absolute | abs | `LDA $1234` |
| 8 | Absolute,X | abs,X | `LDA $1234,X` |
| 9 | Absolute,Y | abs,Y | `LDA $1234,Y` |
| 10 | Indirect | (ind) | `JMP ($1234)` |
| 11 | (Indirect,X) | (zpg,X) | `LDA ($42,X)` |
| 12 | (Indirect),Y | (zpg),Y | `LDA ($42),Y` |
| 13 | Relative | rel | `BEQ label` |

**Tâches**:
- [ ] T-100 : Implémenter addr_implicit()
- [ ] T-101 : Implémenter addr_accumulator()
- [ ] T-102 : Implémenter addr_immediate()
- [ ] T-103 : Implémenter addr_zero_page()
- [ ] T-104 : Implémenter addr_zero_page_x()
- [ ] T-105 : Implémenter addr_zero_page_y()
- [ ] T-106 : Implémenter addr_absolute()
- [ ] T-107 : Implémenter addr_absolute_x()
- [ ] T-108 : Implémenter addr_absolute_y()
- [ ] T-109 : Implémenter addr_indirect()
- [ ] T-110 : Implémenter addr_indexed_indirect() (Indirect,X)
- [ ] T-111 : Implémenter addr_indirect_indexed() (Indirect),Y
- [ ] T-112 : Implémenter addr_relative()
- [ ] T-113 : Écrire tests unitaires modes d'adressage

### US-E1-02 : Instructions de chargement/stockage
> En tant que développeur, je veux les instructions Load/Store pour
> déplacer des données entre registres et mémoire.

**Story Points**: 5
**Statut**: A faire
**Sprint**: S1

**Critères d'acceptation**:
- [ ] LDA, LDX, LDY fonctionnels
- [ ] STA, STX, STY fonctionnels
- [ ] Flags N et Z mis à jour correctement
- [ ] Tests unitaires pour chaque instruction

**Instructions**:
| Opcode | Nom | Description | Flags |
|--------|-----|-------------|-------|
| LDA | Load Accumulator | A ← M | N, Z |
| LDX | Load X Register | X ← M | N, Z |
| LDY | Load Y Register | Y ← M | N, Z |
| STA | Store Accumulator | M ← A | - |
| STX | Store X Register | M ← X | - |
| STY | Store Y Register | M ← Y | - |

**Tâches**:
- [ ] T-114 : Implémenter LDA (8 modes d'adressage)
- [ ] T-115 : Implémenter LDX (5 modes d'adressage)
- [ ] T-116 : Implémenter LDY (5 modes d'adressage)
- [ ] T-117 : Implémenter STA (7 modes d'adressage)
- [ ] T-118 : Implémenter STX (3 modes d'adressage)
- [ ] T-119 : Implémenter STY (3 modes d'adressage)
- [ ] T-120 : Tests unitaires Load/Store

### US-E1-03 : Instructions arithmétiques
> En tant que développeur, je veux les instructions arithmétiques pour
> effectuer additions, soustractions et comparaisons.

**Story Points**: 8
**Statut**: A faire
**Sprint**: S1

**Critères d'acceptation**:
- [ ] ADC, SBC fonctionnels (mode binaire et décimal)
- [ ] CMP, CPX, CPY fonctionnels
- [ ] INC, DEC, INX, INY, DEX, DEY fonctionnels
- [ ] Flags correctement mis à jour (N, Z, C, V)

**Instructions**:
| Opcode | Description | Flags |
|--------|-------------|-------|
| ADC | Add with Carry | N, V, Z, C |
| SBC | Subtract with Carry | N, V, Z, C |
| CMP | Compare Accumulator | N, Z, C |
| CPX | Compare X Register | N, Z, C |
| CPY | Compare Y Register | N, Z, C |
| INC | Increment Memory | N, Z |
| DEC | Decrement Memory | N, Z |
| INX | Increment X | N, Z |
| INY | Increment Y | N, Z |
| DEX | Decrement X | N, Z |
| DEY | Decrement Y | N, Z |

**Tâches**:
- [ ] T-121 : Implémenter ADC (mode binaire)
- [ ] T-122 : Implémenter ADC (mode décimal BCD)
- [ ] T-123 : Implémenter SBC (mode binaire)
- [ ] T-124 : Implémenter SBC (mode décimal BCD)
- [ ] T-125 : Implémenter CMP, CPX, CPY
- [ ] T-126 : Implémenter INC, DEC
- [ ] T-127 : Implémenter INX, INY, DEX, DEY
- [ ] T-128 : Tests arithmétiques complets (overflow, carry, BCD)

### US-E1-04 : Instructions logiques
> En tant que développeur, je veux les instructions logiques (AND, OR, XOR,
> shifts, rotations) pour la manipulation de bits.

**Story Points**: 5
**Statut**: A faire
**Sprint**: S1

**Critères d'acceptation**:
- [ ] AND, ORA, EOR fonctionnels
- [ ] ASL, LSR, ROL, ROR fonctionnels
- [ ] BIT fonctionnel
- [ ] Flags correctement mis à jour

**Instructions**:
| Opcode | Description | Flags |
|--------|-------------|-------|
| AND | Logical AND | N, Z |
| ORA | Logical OR | N, Z |
| EOR | Exclusive OR | N, Z |
| ASL | Arithmetic Shift Left | N, Z, C |
| LSR | Logical Shift Right | N, Z, C |
| ROL | Rotate Left | N, Z, C |
| ROR | Rotate Right | N, Z, C |
| BIT | Bit Test | N, V, Z |

**Tâches**:
- [ ] T-129 : Implémenter AND, ORA, EOR
- [ ] T-130 : Implémenter ASL, LSR (accumulateur et mémoire)
- [ ] T-131 : Implémenter ROL, ROR (accumulateur et mémoire)
- [ ] T-132 : Implémenter BIT
- [ ] T-133 : Tests logiques complets

### US-E1-05 : Instructions de branchement
> En tant que développeur, je veux les instructions de branchement conditionnel
> pour le contrôle de flux.

**Story Points**: 5
**Statut**: A faire
**Sprint**: S1

**Critères d'acceptation**:
- [ ] Les 8 branches conditionnelles fonctionnelles
- [ ] Cycle supplémentaire si branchement pris
- [ ] Cycle supplémentaire si franchissement de page

**Instructions**:
| Opcode | Condition | Signification |
|--------|-----------|---------------|
| BCC | C=0 | Branch if Carry Clear |
| BCS | C=1 | Branch if Carry Set |
| BEQ | Z=1 | Branch if Equal (Zero set) |
| BNE | Z=0 | Branch if Not Equal |
| BMI | N=1 | Branch if Minus |
| BPL | N=0 | Branch if Plus |
| BVS | V=1 | Branch if Overflow Set |
| BVC | V=0 | Branch if Overflow Clear |

**Tâches**:
- [ ] T-134 : Implémenter les 8 instructions de branchement
- [ ] T-135 : Gérer cycle supplémentaire (branch taken)
- [ ] T-136 : Gérer cycle supplémentaire (page crossing)
- [ ] T-137 : Tests de branchement complets

### US-E1-06 : Instructions de saut et sous-programme
> En tant que développeur, je veux JMP, JSR, RTS, RTI pour la gestion
> des sauts et des appels de sous-programmes.

**Story Points**: 5
**Statut**: A faire
**Sprint**: S1

**Critères d'acceptation**:
- [ ] JMP (absolute et indirect) fonctionnel
- [ ] JSR/RTS fonctionnels avec pile
- [ ] RTI fonctionnel (retour d'interruption)
- [ ] Bug JMP indirect à la frontière de page reproduit (hardware bug)

**Instructions**:
| Opcode | Description | Notes |
|--------|-------------|-------|
| JMP | Jump | Abs et (Indirect) |
| JSR | Jump to Subroutine | Push PC-1 sur pile |
| RTS | Return from Subroutine | Pull PC+1 de pile |
| RTI | Return from Interrupt | Pull P puis PC |

**Tâches**:
- [ ] T-138 : Implémenter JMP absolute
- [ ] T-139 : Implémenter JMP indirect (avec bug hardware page boundary)
- [ ] T-140 : Implémenter JSR
- [ ] T-141 : Implémenter RTS
- [ ] T-142 : Implémenter RTI
- [ ] T-143 : Tests sauts et sous-programmes

### US-E1-07 : Instructions de pile
> En tant que développeur, je veux les instructions de manipulation de pile
> pour sauvegarder/restaurer registres et flags.

**Story Points**: 3
**Statut**: A faire
**Sprint**: S2

**Critères d'acceptation**:
- [ ] PHA, PLA fonctionnels
- [ ] PHP, PLP fonctionnels
- [ ] TXS, TSX fonctionnels
- [ ] Pile en page $01xx

**Instructions**:
| Opcode | Description | Flags |
|--------|-------------|-------|
| PHA | Push Accumulator | - |
| PLA | Pull Accumulator | N, Z |
| PHP | Push Processor Status | - |
| PLP | Pull Processor Status | All |
| TXS | Transfer X to SP | - |
| TSX | Transfer SP to X | N, Z |

**Tâches**:
- [ ] T-144 : Implémenter PHA, PLA
- [ ] T-145 : Implémenter PHP, PLP
- [ ] T-146 : Implémenter TXS, TSX
- [ ] T-147 : Tests pile complets

### US-E1-08 : Instructions de transfert registre
> En tant que développeur, je veux les instructions de transfert entre
> registres (TAX, TXA, TAY, TYA).

**Story Points**: 2
**Statut**: A faire
**Sprint**: S2

**Critères d'acceptation**:
- [ ] TAX, TXA, TAY, TYA fonctionnels
- [ ] Flags N, Z mis à jour

**Tâches**:
- [ ] T-148 : Implémenter TAX, TXA, TAY, TYA
- [ ] T-149 : Tests transferts registres

### US-E1-09 : Instructions de contrôle flags
> En tant que développeur, je veux les instructions de manipulation des flags
> processeur (SEC, CLC, SEI, CLI, SED, CLD, CLV).

**Story Points**: 2
**Statut**: A faire
**Sprint**: S2

**Critères d'acceptation**:
- [ ] SEC, CLC, SEI, CLI, SED, CLD, CLV fonctionnels
- [ ] NOP fonctionnel
- [ ] BRK fonctionnel (software interrupt)

**Instructions**:
| Opcode | Description |
|--------|-------------|
| CLC | Clear Carry |
| SEC | Set Carry |
| CLI | Clear Interrupt Disable |
| SEI | Set Interrupt Disable |
| CLD | Clear Decimal Mode |
| SED | Set Decimal Mode |
| CLV | Clear Overflow |
| NOP | No Operation |
| BRK | Force Break (IRQ) |

**Tâches**:
- [ ] T-150 : Implémenter CLC, SEC, CLI, SEI, CLD, SED, CLV
- [ ] T-151 : Implémenter NOP
- [ ] T-152 : Implémenter BRK (vecteur $FFFE)
- [ ] T-153 : Tests flags et contrôle

### US-E1-10 : Système d'interruptions
> En tant que développeur, je veux la gestion des interruptions (IRQ, NMI,
> RESET) pour que le CPU réagisse aux événements matériels.

**Story Points**: 5
**Statut**: A faire
**Sprint**: S2

**Critères d'acceptation**:
- [ ] IRQ fonctionnel (maskable, vecteur $FFFE)
- [ ] NMI fonctionnel (non-maskable, vecteur $FFFA)
- [ ] RESET fonctionnel (vecteur $FFFC)
- [ ] Priorité correcte : RESET > NMI > IRQ
- [ ] Flag I respecté pour IRQ

**Tâches**:
- [ ] T-154 : Implémenter le mécanisme IRQ complet
- [ ] T-155 : Implémenter le mécanisme NMI complet
- [ ] T-156 : Implémenter la séquence RESET
- [ ] T-157 : Implémenter la priorité des interruptions
- [ ] T-158 : Tests interruptions complets

### US-E1-11 : Table de décodage des opcodes
> En tant que développeur, je veux une table de décodage des 256 opcodes
> pour exécuter les instructions efficacement.

**Story Points**: 8
**Statut**: A faire
**Sprint**: S2

**Critères d'acceptation**:
- [ ] Table de 256 entrées (opcode → handler)
- [ ] 151 opcodes officiels mappés
- [ ] 105 opcodes illégaux gérés (NOP ou trap)
- [ ] Comptage de cycles correct pour chaque opcode

**Tâches**:
- [ ] T-159 : Créer la structure opcode_entry_t (handler, mode, cycles, nom)
- [ ] T-160 : Remplir la table des 256 opcodes
- [ ] T-161 : Implémenter cpu_step() avec fetch-decode-execute
- [ ] T-162 : Vérifier les cycle counts contre la documentation hardware
- [ ] T-163 : Tests de la table de décodage

### US-E1-12 : Boucle d'exécution principale
> En tant que développeur, je veux une boucle d'exécution CPU fonctionnelle
> pour que l'émulateur puisse exécuter des programmes.

**Story Points**: 5
**Statut**: A faire
**Sprint**: S2

**Critères d'acceptation**:
- [ ] cpu_step() exécute un cycle complet fetch-decode-execute
- [ ] cpu_execute_cycles() exécute N cycles
- [ ] Timing correct (1 MHz = 1 000 000 cycles/seconde)
- [ ] Passe les test ROMs de validation 6502

**Tâches**:
- [ ] T-164 : Implémenter le cycle fetch-decode-execute
- [ ] T-165 : Intégrer les modes d'adressage dans l'exécution
- [ ] T-166 : Valider avec Klaus Dormann's 6502 test suite
- [ ] T-167 : Tests d'intégration CPU

### US-E1-13 : Désassembleur
> En tant que développeur, je veux un désassembleur intégré pour afficher
> les instructions en mnémoniques lisibles.

**Story Points**: 3
**Statut**: A faire
**Sprint**: S2

**Critères d'acceptation**:
- [ ] cpu_disassemble() retourne le mnémonique correct
- [ ] Affichage des opérandes selon le mode d'adressage
- [ ] Format standard : "ADDR  HEX  MNEMONIC OPERAND"

**Tâches**:
- [ ] T-168 : Implémenter la table des mnémoniques
- [ ] T-169 : Formater la sortie du désassembleur
- [ ] T-170 : Tests désassembleur

### Definition of Done - Epic E1
- [ ] 151 opcodes officiels implémentés et testés
- [ ] 13 modes d'adressage fonctionnels
- [ ] Cycle counts vérifiés (±0 cycle vs hardware)
- [ ] Interruptions (IRQ, NMI, RESET) fonctionnelles
- [ ] Passe Klaus Dormann's 6502 test suite
- [ ] 100% couverture du code CPU
- [ ] Documentation API complète
- [ ] 0 warnings de compilation

---

# EPIC E2 : Système Mémoire

**Priorité**: Critique
**Statut**: Partiel (70% base)
**Version cible**: 0.3.0-alpha
**Sprint**: S3

## Description
Gestion complète de l'espace mémoire 64KB de l'ORIC-1, incluant le banking
ROM/RAM, l'espace I/O mappé en mémoire, et la carte mémoire spécifique.

## Carte mémoire ORIC-1
```
$FFFF ┌──────────────────────┐
      │   ROM BASIC (16KB)   │
$C000 ├──────────────────────┤
      │   I/O Space          │
$BF00 │   VIA 6522 ($B00-$BF)│
$B000 ├──────────────────────┤
      │   Screen Memory      │
$BB80 │   (Text: $BB80-$BFE0)│
$A000 │   (Hires: $A000-$BF3F)│
      ├──────────────────────┤
      │   RAM (User)         │
$0500 ├──────────────────────┤
      │   System Variables   │
$0200 ├──────────────────────┤
      │   Stack ($0100-$01FF)│
$0100 ├──────────────────────┤
      │   Zero Page          │
$0000 └──────────────────────┘
```

## User Stories

### US-E2-01 : Accès mémoire complet
> En tant que développeur, je veux un accès lecture/écriture correct à tout
> l'espace d'adressage 64KB avec le mapping ORIC-1.

**Story Points**: 5
**Statut**: Partiel

**Critères d'acceptation**:
- [x] Lecture/écriture RAM fonctionnelle
- [ ] Espace I/O ($B000-$BFFF) routé vers les périphériques
- [ ] Protection écriture ROM
- [ ] Wrapping Zero Page correct

**Tâches**:
- [x] T-200 : Lecture/écriture RAM basique
- [ ] T-201 : Routage I/O space vers callbacks VIA
- [ ] T-202 : Protection ROM en écriture (avec logging optionnel)
- [ ] T-203 : Tests d'accès mémoire complets

### US-E2-02 : Banking ROM/RAM
> En tant que développeur, je veux le mécanisme de banking pour commuter
> entre ROM et RAM dans la zone $C000-$FFFF.

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Overlay RAM sous la ROM possible
- [ ] Commutation ROM/RAM via registre
- [ ] Vecteurs d'interruption toujours accessibles en ROM

**Tâches**:
- [ ] T-204 : Implémenter le mécanisme d'overlay
- [ ] T-205 : Implémenter la commutation via registre I/O
- [ ] T-206 : Gérer l'accès aux vecteurs d'interruption
- [ ] T-207 : Tests banking complets

### US-E2-03 : Chargement ROM
> En tant qu'utilisateur, je veux charger les ROMs ORIC-1 (BASIC 1.0,
> charset) pour faire fonctionner l'émulateur.

**Story Points**: 3
**Statut**: Partiel

**Critères d'acceptation**:
- [x] Chargement ROM depuis fichier
- [x] Chargement charset depuis fichier
- [ ] Vérification checksum ROM
- [ ] ROM BASIC 1.0 par défaut si disponible

**Tâches**:
- [x] T-208 : Implémenter memory_load_rom()
- [x] T-209 : Implémenter memory_load_charset()
- [ ] T-210 : Ajouter vérification checksum
- [ ] T-211 : Tests chargement ROM

### US-E2-04 : Tracing mémoire
> En tant que développeur, je veux tracer les accès mémoire pour le
> débogage de programmes ORIC.

**Story Points**: 3
**Statut**: Partiel

**Critères d'acceptation**:
- [x] Infrastructure de tracing en place
- [ ] Callbacks sur lecture/écriture par plage d'adresses
- [ ] Breakpoints mémoire (watch)
- [ ] Log des accès I/O

**Tâches**:
- [ ] T-212 : Implémenter callbacks par plage d'adresses
- [ ] T-213 : Implémenter les breakpoints mémoire
- [ ] T-214 : Logger les accès I/O
- [ ] T-215 : Tests tracing

### Definition of Done - Epic E2
- [ ] Carte mémoire ORIC-1 complète et correcte
- [ ] Banking ROM/RAM fonctionnel
- [ ] I/O space correctement routé
- [ ] Tracing mémoire opérationnel
- [ ] Tests couvrant tous les cas limites
- [ ] Documentation carte mémoire

---

# EPIC E3 : Système I/O (VIA 6522)

**Priorité**: Critique
**Statut**: Stub (15%)
**Version cible**: 0.3.0-alpha
**Sprints**: S3, S4

## Description
Émulation du contrôleur d'entrées/sorties MOS 6522 VIA (Versatile Interface
Adapter) utilisé dans l'ORIC-1 pour le clavier, la cassette, l'imprimante,
et comme pont vers le PSG AY-3-8910.

## User Stories

### US-E3-01 : Registres VIA 6522
> En tant que développeur, je veux les 16 registres du VIA 6522 correctement
> émulés pour que les périphériques fonctionnent.

**Story Points**: 8
**Statut**: A faire

**Registres**:
| Offset | Nom | Description |
|--------|-----|-------------|
| $00 | ORB/IRB | Port B Output/Input |
| $01 | ORA/IRA | Port A Output/Input |
| $02 | DDRB | Data Direction Register B |
| $03 | DDRA | Data Direction Register A |
| $04 | T1C-L | Timer 1 Counter Low |
| $05 | T1C-H | Timer 1 Counter High |
| $06 | T1L-L | Timer 1 Latch Low |
| $07 | T1L-H | Timer 1 Latch High |
| $08 | T2C-L | Timer 2 Counter Low |
| $09 | T2C-H | Timer 2 Counter High |
| $0A | SR | Shift Register |
| $0B | ACR | Auxiliary Control Register |
| $0C | PCR | Peripheral Control Register |
| $0D | IFR | Interrupt Flag Register |
| $0E | IER | Interrupt Enable Register |
| $0F | ORA-NH | Port A (no handshake) |

**Tâches**:
- [ ] T-300 : Implémenter lecture des 16 registres
- [ ] T-301 : Implémenter écriture des 16 registres
- [ ] T-302 : Implémenter les DDR (data direction)
- [ ] T-303 : Tests registres complets

### US-E3-02 : Timers VIA
> En tant que développeur, je veux les deux timers du VIA fonctionnels
> pour le timing des périphériques et la génération d'interruptions.

**Story Points**: 8
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Timer 1 : mode one-shot et free-running
- [ ] Timer 2 : mode one-shot et pulse counting
- [ ] Génération d'IRQ sur timeout
- [ ] Latches fonctionnels

**Tâches**:
- [ ] T-304 : Implémenter Timer 1 (one-shot)
- [ ] T-305 : Implémenter Timer 1 (free-running)
- [ ] T-306 : Implémenter Timer 2 (one-shot)
- [ ] T-307 : Implémenter Timer 2 (pulse counting)
- [ ] T-308 : Implémenter la génération d'IRQ timer
- [ ] T-309 : Tests timers complets

### US-E3-03 : Shift Register
> En tant que développeur, je veux le shift register du VIA pour la
> communication série (cassette, imprimante).

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] 8 modes du shift register fonctionnels
- [ ] Génération IRQ sur transfert complet

**Tâches**:
- [ ] T-310 : Implémenter les 8 modes du shift register
- [ ] T-311 : Implémenter IRQ shift register
- [ ] T-312 : Tests shift register

### US-E3-04 : Clavier ORIC
> En tant qu'utilisateur, je veux taper sur mon clavier PC et voir les
> touches correspondantes sur l'ORIC.

**Story Points**: 8
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Matrice clavier 8x8 émulée
- [ ] Mapping PC → ORIC correct
- [ ] Scan par colonnes via Port B
- [ ] Lecture par Port A
- [ ] Touches spéciales (FUNCT, CTRL, SHIFT)

**Matrice clavier ORIC-1** (8 colonnes x 8 lignes):
```
        Col 0   Col 1   Col 2   Col 3   Col 4   Col 5   Col 6   Col 7
Row 0:  7       N       5       V       1       X       3       -
Row 1:  J       T       R       F       (none)  (none)  (none)  (none)
Row 2:  M       6       B       4       C       2       Z       CTRL
Row 3:  K       9       ;       -       /       ¥       .       (none)
Row 4:  SPC     ,       .       UP      LEFT    DOWN    RIGHT   DEL
Row 5:  U       I       O       P       FUNCT   (none)  (none)  (none)
Row 6:  Y       H       G       E       D       W       S       A
Row 7:  8       L       0       ESC     RET     (none)  SHIFT   (none)
```

**Tâches**:
- [ ] T-313 : Implémenter la matrice clavier 8x8
- [ ] T-314 : Créer la table de mapping PC → ORIC
- [ ] T-315 : Intégrer le scan clavier avec le VIA (Port A/B)
- [ ] T-316 : Gérer les touches spéciales
- [ ] T-317 : Intégrer SDL2 pour la capture clavier
- [ ] T-318 : Tests clavier

### US-E3-05 : Interface cassette via VIA
> En tant que développeur, je veux l'interface cassette du VIA pour
> charger et sauvegarder des programmes sur bande.

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Signal cassette via CB1/CB2
- [ ] Contrôle moteur cassette
- [ ] Lecture/écriture bits série

**Tâches**:
- [ ] T-319 : Implémenter le signal cassette CB1/CB2
- [ ] T-320 : Implémenter le contrôle moteur
- [ ] T-321 : Implémenter la sérialisation bits
- [ ] T-322 : Tests interface cassette

### US-E3-06 : Interruptions VIA
> En tant que développeur, je veux le système d'interruptions complet du
> VIA pour les événements asynchrones.

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] IFR (Interrupt Flag Register) fonctionnel
- [ ] IER (Interrupt Enable Register) fonctionnel
- [ ] Génération IRQ vers CPU
- [ ] Sources : Timer 1, Timer 2, CB1, CB2, SR, CA1, CA2

**Tâches**:
- [ ] T-323 : Implémenter IFR (lecture et clear)
- [ ] T-324 : Implémenter IER (set et clear)
- [ ] T-325 : Connecter les sources d'interruption
- [ ] T-326 : Implémenter via_update() pour le tick par cycle
- [ ] T-327 : Tests interruptions VIA

### Definition of Done - Epic E3
- [ ] 16 registres VIA lus/écrits correctement
- [ ] Timers 1 et 2 fonctionnels dans tous les modes
- [ ] Shift register opérationnel
- [ ] Clavier fonctionnel avec mapping complet
- [ ] Interface cassette fonctionnelle
- [ ] Système d'interruptions complet
- [ ] Tests couvrant tous les modes
- [ ] Documentation VIA 6522

---

# EPIC E4 : Système Vidéo

**Priorité**: Haute
**Statut**: A faire (0%)
**Version cible**: 0.4.0-alpha
**Sprints**: S5, S6

## Description
Émulation du système vidéo de l'ORIC-1 basé sur l'ULA (Uncommitted Logic
Array), supportant le mode texte 40x28 et le mode HIRES 240x200, avec
gestion des attributs de couleur et rendu via SDL2.

## User Stories

### US-E4-01 : Mode texte (40x28)
> En tant qu'utilisateur, je veux voir l'affichage texte de l'ORIC pour
> interagir avec le BASIC et les programmes textuels.

**Story Points**: 8
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Affichage 40 colonnes x 28 lignes
- [ ] Police de caractères ORIC (charset ROM 6x8)
- [ ] Jeu de caractères standard et alternatif
- [ ] Curseur clignotant

**Tâches**:
- [ ] T-400 : Implémenter le rendu texte 40x28
- [ ] T-401 : Charger et utiliser le charset ROM
- [ ] T-402 : Implémenter le jeu alternatif
- [ ] T-403 : Implémenter le curseur clignotant
- [ ] T-404 : Tests mode texte

### US-E4-02 : Mode HIRES (240x200)
> En tant qu'utilisateur, je veux voir les graphiques haute résolution
> pour les jeux et programmes graphiques ORIC.

**Story Points**: 8
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Résolution 240x200 pixels
- [ ] 6 couleurs (noir, rouge, vert, jaune, bleu, magenta, cyan, blanc)
- [ ] Mode mixte texte+hires possible
- [ ] Adressage correct de la mémoire vidéo ($A000-$BF3F)

**Tâches**:
- [ ] T-405 : Implémenter le rendu HIRES 240x200
- [ ] T-406 : Implémenter le décodage des lignes vidéo
- [ ] T-407 : Gérer le mode mixte
- [ ] T-408 : Tests mode HIRES

### US-E4-03 : Attributs de couleur
> En tant qu'utilisateur, je veux les couleurs correctes à l'écran avec
> le système d'attributs sérialisés de l'ORIC.

**Story Points**: 8
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Attributs sérialisés en début de ligne
- [ ] Couleurs encre et papier (foreground/background)
- [ ] Attributs inversés
- [ ] Clignotement (blink)
- [ ] Double hauteur

**Attributs ORIC**:
| Code | Signification |
|------|---------------|
| 0-7 | Couleur encre (ink) |
| 8-15 | Style (normal, alt charset, double, blink) |
| 16-23 | Couleur papier (paper) |
| 24-31 | Contrôle (60Hz, flash, etc.) |

**Tâches**:
- [ ] T-409 : Implémenter le parsing des attributs sérialisés
- [ ] T-410 : Implémenter ink/paper
- [ ] T-411 : Implémenter inverse et blink
- [ ] T-412 : Implémenter double hauteur
- [ ] T-413 : Tests attributs couleur

### US-E4-04 : Backend SDL2 vidéo
> En tant que développeur, je veux un rendu SDL2 performant pour afficher
> la sortie vidéo de l'émulateur.

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Fenêtre SDL2 avec taille configurable
- [ ] Rendu 50Hz (PAL)
- [ ] Mode plein écran (F11)
- [ ] Capture d'écran (F12)
- [ ] Filtres optionnels (scanlines, CRT)

**Tâches**:
- [ ] T-414 : Créer la fenêtre SDL2 et le renderer
- [ ] T-415 : Implémenter le rendu framebuffer → texture
- [ ] T-416 : Implémenter le vsync à 50Hz
- [ ] T-417 : Implémenter le mode plein écran
- [ ] T-418 : Implémenter la capture d'écran (PNG)
- [ ] T-419 : Tests rendu vidéo

### Definition of Done - Epic E4
- [ ] Mode texte 40x28 avec charset correct
- [ ] Mode HIRES 240x200 fonctionnel
- [ ] Couleurs et attributs corrects
- [ ] Rendu SDL2 à 50Hz stable
- [ ] Screenshots fonctionnels
- [ ] Tests vidéo complets
- [ ] Documentation système vidéo

---

# EPIC E5 : Système Audio (AY-3-8910)

**Priorité**: Haute
**Statut**: A faire (0%)
**Version cible**: 0.5.0-alpha
**Sprint**: S7

## Description
Émulation du chip sonore General Instrument AY-3-8910 PSG (Programmable Sound
Generator), connecté via le VIA 6522 Port A.

## User Stories

### US-E5-01 : Générateurs de tonalité
> En tant qu'utilisateur, je veux entendre les sons et musiques des programmes
> ORIC avec les 3 canaux de tonalité du PSG.

**Story Points**: 8
**Statut**: A faire

**Critères d'acceptation**:
- [ ] 3 canaux de tonalité indépendants (A, B, C)
- [ ] Fréquence réglable (12 bits par canal)
- [ ] Mixage correct des canaux

**Tâches**:
- [ ] T-500 : Implémenter les 3 générateurs de tonalité
- [ ] T-501 : Implémenter le réglage de fréquence (registres R0-R5)
- [ ] T-502 : Implémenter le mixage des canaux
- [ ] T-503 : Tests tonalité

### US-E5-02 : Générateur de bruit
> En tant qu'utilisateur, je veux le générateur de bruit pour les effets
> sonores (explosions, tirs, etc.).

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Générateur de bruit pseudo-aléatoire
- [ ] Fréquence réglable (5 bits)
- [ ] Mixable avec les canaux de tonalité

**Tâches**:
- [ ] T-504 : Implémenter le LFSR (Linear Feedback Shift Register)
- [ ] T-505 : Implémenter le réglage de fréquence bruit (R6)
- [ ] T-506 : Implémenter le mixer tonalité/bruit (R7)
- [ ] T-507 : Tests bruit

### US-E5-03 : Enveloppes
> En tant qu'utilisateur, je veux les enveloppes de volume pour des sons
> dynamiques et expressifs.

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] 16 formes d'enveloppe (registre R13)
- [ ] Période réglable (16 bits, R11-R12)
- [ ] Volume fixe ou enveloppe par canal (R8-R10)

**Tâches**:
- [ ] T-508 : Implémenter les 16 formes d'enveloppe
- [ ] T-509 : Implémenter le contrôle de période
- [ ] T-510 : Implémenter le sélecteur volume fixe/enveloppe
- [ ] T-511 : Tests enveloppes

### US-E5-04 : Interface VIA → PSG
> En tant que développeur, je veux la connexion VIA → AY-3-8910 pour
> que le CPU puisse contrôler le PSG.

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Communication via Port A du VIA
- [ ] Protocole BDIR/BC1 (Inactive, Read, Write, Latch Address)
- [ ] 14 registres PSG accessibles

**Tâches**:
- [ ] T-512 : Implémenter le protocole BDIR/BC1
- [ ] T-513 : Connecter VIA Port A au PSG
- [ ] T-514 : Implémenter lecture/écriture des 14 registres PSG
- [ ] T-515 : Tests interface VIA-PSG

### US-E5-05 : Backend SDL2 audio
> En tant que développeur, je veux la sortie audio via SDL2 pour produire
> le son sur le système hôte.

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Sortie audio SDL2 (44100 Hz, 16 bits, stéréo)
- [ ] Buffer audio avec latence minimale
- [ ] Contrôle du volume global
- [ ] Mute/unmute

**Tâches**:
- [ ] T-516 : Initialiser SDL2 Audio
- [ ] T-517 : Implémenter le callback audio (génération samples)
- [ ] T-518 : Implémenter le ring buffer audio
- [ ] T-519 : Implémenter volume et mute
- [ ] T-520 : Tests sortie audio

### Definition of Done - Epic E5
- [ ] 3 canaux de tonalité fonctionnels
- [ ] Générateur de bruit fonctionnel
- [ ] 16 enveloppes fonctionnelles
- [ ] Interface VIA-PSG correcte
- [ ] Sortie SDL2 sans craquements
- [ ] Tests audio complets
- [ ] Documentation PSG

---

# EPIC E6 : Stockage Cassette (.TAP)

**Priorité**: Haute
**Statut**: Partiel (30% structure)
**Version cible**: 0.6.0-alpha
**Sprint**: S8

## Description
Support complet du format de fichier cassette .TAP pour charger et sauvegarder
des programmes ORIC, incluant le mode turbo (fast load).

## User Stories

### US-E6-01 : Lecture de fichiers .TAP
> En tant qu'utilisateur, je veux charger des fichiers .TAP pour exécuter
> des programmes et jeux ORIC.

**Story Points**: 8
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Parsing complet du format .TAP
- [ ] Support multi-programmes par fichier
- [ ] Lecture des headers (nom, type, adresses)
- [ ] Chargement des données en mémoire

**Format TAP**:
```
[Sync bytes: $16 x N] [Header: $24 bytes] [Data: variable] [Checksum]
Header: sync | type | autorun | end_addr_hi | end_addr_lo |
        start_addr_hi | start_addr_lo | $00 | name[16]
```

**Tâches**:
- [ ] T-600 : Implémenter tap_open_read() (parsing fichier)
- [ ] T-601 : Implémenter tap_read_header() (décodage header)
- [ ] T-602 : Implémenter tap_read_data() (lecture données)
- [ ] T-603 : Implémenter la vérification checksum
- [ ] T-604 : Gérer les fichiers multi-programmes
- [ ] T-605 : Tests lecture TAP

### US-E6-02 : Écriture de fichiers .TAP
> En tant qu'utilisateur, je veux sauvegarder mes programmes au format .TAP.

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Création de fichiers .TAP valides
- [ ] Écriture des headers corrects
- [ ] Calcul et écriture checksum

**Tâches**:
- [ ] T-606 : Implémenter tap_open_write()
- [ ] T-607 : Implémenter tap_write_header()
- [ ] T-608 : Implémenter tap_write_data()
- [ ] T-609 : Implémenter le calcul checksum à l'écriture
- [ ] T-610 : Tests écriture TAP

### US-E6-03 : Fast Load (Turbo Tape)
> En tant qu'utilisateur, je veux un mode de chargement rapide pour ne
> pas attendre les temps de chargement réels de la cassette.

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Chargement instantané en mémoire (bypass timing cassette)
- [ ] Patch des routines ROM de chargement
- [ ] Toggle fast/normal load

**Tâches**:
- [ ] T-611 : Implémenter le chargement direct en mémoire
- [ ] T-612 : Implémenter le patch ROM pour fast load
- [ ] T-613 : Ajouter l'option --fast-load
- [ ] T-614 : Tests fast load

### US-E6-04 : Simulation timing cassette
> En tant que développeur, je veux simuler le timing réel de la cassette
> pour la compatibilité avec les protections anti-copie.

**Story Points**: 5
**Statut**: A faire

**Critères d'acceptation**:
- [ ] Timing réaliste des bits (2400/1200 baud)
- [ ] Signaux CB1/CB2 via VIA corrects
- [ ] Compatible avec les loaders custom

**Tâches**:
- [ ] T-615 : Implémenter le timing 2400/1200 baud
- [ ] T-616 : Générer les signaux via VIA CB1/CB2
- [ ] T-617 : Tests timing cassette

### Definition of Done - Epic E6
- [ ] Lecture/écriture .TAP fonctionnelle
- [ ] Fast load opérationnel
- [ ] Timing cassette réaliste
- [ ] Compatible avec les principaux programmes ORIC
- [ ] Tests complets
- [ ] Documentation format TAP

---

# EPIC E7 : Stockage Disque (Sedoric)

**Priorité**: Moyenne
**Statut**: A faire (0%)
**Version cible**: 0.7.0-alpha
**Sprint**: S9

## Description
Support des images disque (.DSK) avec le système de fichiers Sedoric et
l'émulation du contrôleur Microdisc.

## User Stories

### US-E7-01 : Contrôleur Microdisc
> En tant que développeur, je veux émuler le contrôleur Microdisc pour
> accéder aux lecteurs de disquettes virtuels.

**Story Points**: 8
**Statut**: A faire

**Tâches**:
- [ ] T-700 : Implémenter les registres du contrôleur FDC (WD1793)
- [ ] T-701 : Implémenter les commandes de lecture secteur
- [ ] T-702 : Implémenter les commandes d'écriture secteur
- [ ] T-703 : Implémenter seek/step/restore
- [ ] T-704 : Implémenter la gestion des pistes et secteurs
- [ ] T-705 : Tests contrôleur FDC

### US-E7-02 : Format .DSK
> En tant qu'utilisateur, je veux charger des images disque .DSK pour
> accéder aux programmes sur disquette.

**Story Points**: 5
**Statut**: A faire

**Tâches**:
- [ ] T-706 : Implémenter le parsing du format .DSK
- [ ] T-707 : Implémenter la lecture de secteurs
- [ ] T-708 : Implémenter l'écriture de secteurs
- [ ] T-709 : Gérer les formats simple/double face
- [ ] T-710 : Tests format DSK

### US-E7-03 : Système de fichiers Sedoric
> En tant qu'utilisateur, je veux naviguer et accéder aux fichiers
> sur les disquettes Sedoric.

**Story Points**: 8
**Statut**: A faire

**Tâches**:
- [ ] T-711 : Implémenter la lecture du répertoire Sedoric
- [ ] T-712 : Implémenter la lecture de fichiers
- [ ] T-713 : Implémenter l'écriture de fichiers
- [ ] T-714 : Implémenter la gestion de l'espace libre (FAT)
- [ ] T-715 : Implémenter le formatage de disque
- [ ] T-716 : Tests Sedoric

### Definition of Done - Epic E7
- [ ] Contrôleur FDC WD1793 fonctionnel
- [ ] Lecture/écriture .DSK
- [ ] Navigation Sedoric fonctionnelle
- [ ] Compatible avec les images courantes
- [ ] Tests complets
- [ ] Documentation Sedoric

---

# EPIC E8 : Système de Fichiers Hôte (HostFS)

**Priorité**: Moyenne
**Statut**: Partiel (40% structure)
**Version cible**: 0.8.0-alpha
**Sprint**: S10

## Description
Partage de fichiers entre le système hôte (Linux) et l'émulateur ORIC,
permettant un échange transparent de programmes et données.

## User Stories

### US-E8-01 : Montage de répertoire hôte
> En tant qu'utilisateur, je veux monter un répertoire de mon PC pour
> y accéder depuis l'émulateur ORIC.

**Story Points**: 5
**Statut**: Partiel

**Tâches**:
- [x] T-800 : Implémenter hostfs_mount() / hostfs_unmount()
- [ ] T-801 : Implémenter la vérification du répertoire
- [ ] T-802 : Gérer les permissions
- [ ] T-803 : Tests montage

### US-E8-02 : Opérations sur fichiers
> En tant qu'utilisateur, je veux lire, écrire, supprimer et renommer
> des fichiers partagés.

**Story Points**: 8
**Statut**: A faire

**Tâches**:
- [ ] T-804 : Implémenter hostfs_open()
- [ ] T-805 : Implémenter hostfs_read() / hostfs_write()
- [ ] T-806 : Implémenter hostfs_seek() / hostfs_close()
- [ ] T-807 : Implémenter hostfs_delete() / hostfs_rename()
- [ ] T-808 : Implémenter hostfs_list() (listing répertoire)
- [ ] T-809 : Tests opérations fichiers

### US-E8-03 : Conversion de chemins
> En tant que développeur, je veux une conversion transparente entre
> les noms de fichiers ORIC et les chemins hôte.

**Story Points**: 5
**Statut**: A faire

**Tâches**:
- [ ] T-810 : Implémenter oric_to_host_path()
- [ ] T-811 : Implémenter host_to_oric_name()
- [ ] T-812 : Gérer les caractères spéciaux et longueurs de noms
- [ ] T-813 : Tests conversion chemins

### US-E8-04 : Couche VFS (Virtual File System)
> En tant que développeur, je veux une couche d'abstraction VFS pour
> unifier l'accès aux fichiers (cassette, disque, hôte).

**Story Points**: 8
**Statut**: A faire

**Tâches**:
- [ ] T-814 : Définir l'interface VFS abstraite
- [ ] T-815 : Implémenter le backend HostFS
- [ ] T-816 : Implémenter le backend TAP
- [ ] T-817 : Implémenter le backend Sedoric
- [ ] T-818 : Tests VFS

### Definition of Done - Epic E8
- [ ] Montage/démontage de répertoires
- [ ] CRUD fichiers fonctionnel
- [ ] Conversion de chemins
- [ ] VFS unifié
- [ ] Tests complets
- [ ] Documentation HostFS

---

# EPIC E9 : Outils de Conversion

**Priorité**: Moyenne
**Statut**: CLI fait, backends A faire
**Version cible**: 0.9.0-alpha
**Sprint**: S11

## Description
Outils en ligne de commande pour convertir des programmes entre les
différents formats de l'ORIC (BASIC, binaire, .TAP, Sedoric).

## User Stories

### US-E9-01 : bas2tap - Convertisseur BASIC → TAP
> En tant qu'utilisateur, je veux convertir mes programmes BASIC en
> fichier .TAP pour les charger dans l'émulateur.

**Story Points**: 8
**Statut**: CLI fait, backend A faire

**Tâches**:
- [x] T-900 : Créer le CLI bas2tap (argument parsing)
- [ ] T-901 : Implémenter le tokenizer BASIC ORIC
- [ ] T-902 : Implémenter tap_from_basic()
- [ ] T-903 : Gérer l'option auto-run
- [ ] T-904 : Tests bas2tap

### US-E9-02 : bin2tap - Convertisseur binaire → TAP
> En tant qu'utilisateur, je veux convertir du code machine en .TAP
> avec les adresses de chargement et d'exécution.

**Story Points**: 5
**Statut**: CLI fait, backend A faire

**Tâches**:
- [x] T-905 : Créer le CLI bin2tap (argument parsing)
- [ ] T-906 : Implémenter tap_from_binary()
- [ ] T-907 : Gérer adresses start/exec
- [ ] T-908 : Tests bin2tap

### US-E9-03 : tap2sedoric - Convertisseur TAP → Sedoric
> En tant qu'utilisateur, je veux convertir des fichiers .TAP en image
> disque Sedoric pour les utiliser avec le lecteur de disquettes.

**Story Points**: 5
**Statut**: CLI fait, backend A faire

**Tâches**:
- [x] T-909 : Créer le CLI tap2sedoric (argument parsing)
- [ ] T-910 : Implémenter la conversion TAP → Sedoric
- [ ] T-911 : Créer l'image disque avec fichier
- [ ] T-912 : Tests tap2sedoric

### US-E9-04 : Support hybride BASIC + code machine
> En tant qu'utilisateur, je veux créer des programmes hybrides
> BASIC + machine code en un seul .TAP.

**Story Points**: 5
**Statut**: A faire

**Tâches**:
- [ ] T-913 : Implémenter l'attachement de binaire au BASIC
- [ ] T-914 : Implémenter le multi-bloc dans un .TAP
- [ ] T-915 : Tests programmes hybrides

### Definition of Done - Epic E9
- [ ] bas2tap convertit correctement les programmes BASIC
- [ ] bin2tap convertit correctement les binaires
- [ ] tap2sedoric crée des images disque valides
- [ ] Support hybride fonctionnel
- [ ] Tests complets pour chaque outil
- [ ] Documentation et man pages

---

# EPIC E10 : Débogueur & Outils Développeur

**Priorité**: Basse
**Statut**: A faire (0%)
**Version cible**: 0.9.5-beta
**Sprint**: S12

## Description
Outils de débogage intégrés pour les développeurs de programmes ORIC :
breakpoints, step, visualisation mémoire, trace.

## User Stories

### US-E10-01 : Breakpoints et Step
> En tant que développeur ORIC, je veux poser des breakpoints et
> exécuter le code pas à pas.

**Story Points**: 8
**Statut**: A faire

**Tâches**:
- [ ] T-1000 : Implémenter les breakpoints par adresse
- [ ] T-1001 : Implémenter step (1 instruction)
- [ ] T-1002 : Implémenter step over (passer les JSR)
- [ ] T-1003 : Implémenter run until (exécuter jusqu'à adresse)
- [ ] T-1004 : Implémenter les breakpoints conditionnels
- [ ] T-1005 : Tests breakpoints

### US-E10-02 : Visualisation mémoire
> En tant que développeur ORIC, je veux visualiser la mémoire en temps
> réel pour comprendre le comportement des programmes.

**Story Points**: 5
**Statut**: A faire

**Tâches**:
- [ ] T-1006 : Implémenter l'affichage hex dump
- [ ] T-1007 : Implémenter la recherche en mémoire
- [ ] T-1008 : Implémenter les watchpoints (break on read/write)
- [ ] T-1009 : Tests visualisation mémoire

### US-E10-03 : Inspecteur de registres
> En tant que développeur ORIC, je veux voir l'état des registres CPU
> et VIA en temps réel.

**Story Points**: 3
**Statut**: A faire

**Tâches**:
- [ ] T-1010 : Affichage registres CPU (A, X, Y, SP, PC, P)
- [ ] T-1011 : Affichage flags détaillé (N V - B D I Z C)
- [ ] T-1012 : Affichage registres VIA
- [ ] T-1013 : Affichage registres PSG
- [ ] T-1014 : Tests inspecteur

### US-E10-04 : Trace et logging
> En tant que développeur ORIC, je veux tracer l'exécution pour analyser
> le comportement d'un programme.

**Story Points**: 5
**Statut**: A faire

**Tâches**:
- [ ] T-1015 : Implémenter le trace logging (chaque instruction)
- [ ] T-1016 : Implémenter le filtrage par plage d'adresses
- [ ] T-1017 : Implémenter l'export vers fichier
- [ ] T-1018 : Implémenter le compteur de cycles
- [ ] T-1019 : Tests trace

### US-E10-05 : Interface débogueur
> En tant que développeur ORIC, je veux une interface de débogage
> accessible via F9 ou ligne de commande.

**Story Points**: 8
**Statut**: A faire

**Tâches**:
- [ ] T-1020 : Créer l'interface console du débogueur
- [ ] T-1021 : Implémenter le parser de commandes
- [ ] T-1022 : Implémenter les commandes (break, step, mem, reg, trace, etc.)
- [ ] T-1023 : Intégrer le désassembleur en temps réel
- [ ] T-1024 : Tests interface débogueur

### Definition of Done - Epic E10
- [ ] Breakpoints fonctionnels (adresse, condition)
- [ ] Step / Step Over / Run Until
- [ ] Visualisation mémoire et registres
- [ ] Trace logging avec filtres
- [ ] Interface débogueur utilisable
- [ ] Documentation débogueur

---

# EPIC E11 : Optimisation & Stabilisation

**Priorité**: Haute
**Statut**: A faire
**Version cible**: 0.9.9-rc
**Sprints**: S13, S14

## Description
Optimisation des performances, correction des bugs, stabilisation de
l'ensemble de l'émulateur avant la release v1.0.0.

## User Stories

### US-E11-01 : Optimisation CPU
> En tant qu'utilisateur, je veux que l'émulateur soit fluide et ne
> consomme pas trop de ressources.

**Story Points**: 8
**Statut**: A faire

**Tâches**:
- [ ] T-1100 : Profiler l'exécution CPU
- [ ] T-1101 : Optimiser la boucle fetch-decode-execute
- [ ] T-1102 : Optimiser les accès mémoire
- [ ] T-1103 : Benchmark : cible <5% CPU usage

### US-E11-02 : Optimisation vidéo
> En tant qu'utilisateur, je veux un rendu vidéo fluide sans saccades.

**Story Points**: 5
**Statut**: A faire

**Tâches**:
- [ ] T-1104 : Optimiser le rendu (dirty rectangles)
- [ ] T-1105 : Optimiser la copie framebuffer → texture
- [ ] T-1106 : Benchmark : 50 FPS constant

### US-E11-03 : Système de configuration
> En tant qu'utilisateur, je veux pouvoir configurer l'émulateur
> (chemins ROM, résolution, audio, contrôles).

**Story Points**: 5
**Statut**: A faire

**Tâches**:
- [ ] T-1107 : Implémenter le parsing de fichier .ini
- [ ] T-1108 : Implémenter les options en ligne de commande
- [ ] T-1109 : Sauvegarder/charger la configuration
- [ ] T-1110 : Tests configuration

### US-E11-04 : Gestion d'erreurs robuste
> En tant que développeur, je veux une gestion d'erreurs complète
> pour que l'émulateur ne crashe pas.

**Story Points**: 5
**Statut**: A faire

**Tâches**:
- [ ] T-1111 : Audit de toutes les fonctions pour les cas d'erreur
- [ ] T-1112 : Ajouter les vérifications manquantes
- [ ] T-1113 : Implémenter la récupération gracieuse
- [ ] T-1114 : Tests de robustesse (fuzzing)

### US-E11-05 : Audit qualité
> En tant que chef de projet, je veux un audit complet avant la release.

**Story Points**: 8
**Statut**: A faire

**Tâches**:
- [ ] T-1115 : Analyse statique (cppcheck, clang-tidy)
- [ ] T-1116 : Détection fuites mémoire (Valgrind)
- [ ] T-1117 : Audit sécurité (buffer overflows, format strings)
- [ ] T-1118 : Couverture de code > 90%
- [ ] T-1119 : Correction de tous les bugs connus

### Definition of Done - Epic E11
- [ ] Performance : <5% CPU, 50 FPS stable
- [ ] 0 fuite mémoire (Valgrind clean)
- [ ] 0 warning compilation
- [ ] Analyse statique clean
- [ ] Couverture > 90%
- [ ] Configuration fonctionnelle
- [ ] Tous bugs critiques corrigés

---

# EPIC E12 : Release v1.0.0

**Priorité**: Critique
**Statut**: A faire
**Version cible**: 1.0.0
**Sprint**: S15

## Description
Préparation et publication de la version 1.0.0 stable de l'émulateur.

## User Stories

### US-E12-01 : Documentation utilisateur
> En tant qu'utilisateur, je veux un guide complet pour utiliser l'émulateur.

**Story Points**: 8
**Statut**: A faire

**Tâches**:
- [ ] T-1200 : Rédiger le guide utilisateur complet
- [ ] T-1201 : Créer les pages de manuel (man pages)
- [ ] T-1202 : Documenter les raccourcis clavier
- [ ] T-1203 : Créer un FAQ

### US-E12-02 : Empaquetage
> En tant qu'utilisateur, je veux installer l'émulateur facilement sur ma
> distribution Linux.

**Story Points**: 5
**Statut**: A faire

**Tâches**:
- [ ] T-1204 : Créer le paquet .deb (Debian/Ubuntu)
- [ ] T-1205 : Créer le paquet .rpm (Fedora/RHEL)
- [ ] T-1206 : Créer l'archive .tar.gz
- [ ] T-1207 : Créer l'AppImage
- [ ] T-1208 : Tests d'installation sur distributions cibles

### US-E12-03 : Programmes d'exemple
> En tant qu'utilisateur, je veux des exemples pour découvrir les
> capacités de l'ORIC.

**Story Points**: 3
**Statut**: A faire

**Tâches**:
- [ ] T-1209 : Créer un programme BASIC de démonstration
- [ ] T-1210 : Créer un programme graphique HIRES
- [ ] T-1211 : Créer un programme sonore
- [ ] T-1212 : Packager les exemples avec l'émulateur

### US-E12-04 : Publication
> En tant que chef de projet, je veux publier la v1.0.0 officiellement.

**Story Points**: 3
**Statut**: A faire

**Tâches**:
- [ ] T-1213 : Créer le tag Git v1.0.0
- [ ] T-1214 : Rédiger les release notes
- [ ] T-1215 : Publier sur GitHub
- [ ] T-1216 : Annoncer sur les forums ORIC (Defence Force, etc.)

### Definition of Done - Epic E12
- [ ] Documentation complète et relue
- [ ] Paquets testés sur 3+ distributions
- [ ] Exemples fonctionnels inclus
- [ ] Release publiée sur GitHub
- [ ] Annonce communautaire

---

# EPIC E13 : Extensions Post-Release

**Priorité**: Basse
**Statut**: Planifié
**Version cible**: 1.x.x
**Sprints**: S16+

## Description
Fonctionnalités additionnelles prévues après la v1.0.0, guidées par les
retours de la communauté.

## Features planifiées

### Compatibilité étendue
| Feature | Priorité | Story Points |
|---------|----------|-------------|
| Support ORIC Atmos | Haute | 13 |
| Support Telestrat | Moyenne | 21 |
| Support Pravetz-8D | Basse | 8 |

### Fonctionnalités avancées
| Feature | Priorité | Story Points |
|---------|----------|-------------|
| Save States | Haute | 8 |
| Joystick support | Moyenne | 3 |
| Printer emulation | Basse | 5 |
| Network (TCP/IP overlay) | Basse | 13 |

### Outils développeur
| Feature | Priorité | Story Points |
|---------|----------|-------------|
| Profiler de performance | Moyenne | 8 |
| ROM analysis tools | Basse | 5 |
| Sprite editor | Basse | 8 |

### Communauté
| Feature | Priorité | Story Points |
|---------|----------|-------------|
| Plugin system | Moyenne | 13 |
| Game compatibility DB | Haute | 5 |
| Contribution guide | Haute | 3 |

---

# Planification des Sprints

## Timeline globale

```
2026
├── Fév    S0  ████░░░░░░ Infrastructure (E0) ← NOUS SOMMES ICI
├── Mars   S1  ░░░░░░░░░░ CPU Modes d'adressage + Load/Store/Arith (E1)
│          S2  ░░░░░░░░░░ CPU Logique/Branch/Pile/Interruptions (E1)
├── Avr    S3  ░░░░░░░░░░ Mémoire complète + VIA registres (E2+E3)
│          S4  ░░░░░░░░░░ VIA timers + clavier + cassette (E3)
├── Mai    S5  ░░░░░░░░░░ Vidéo texte + attributs (E4)
│          S6  ░░░░░░░░░░ Vidéo HIRES + SDL2 backend (E4)
├── Juin   S7  ░░░░░░░░░░ Audio AY-3-8910 + SDL2 audio (E5)
├── Juil   S8  ░░░░░░░░░░ Stockage cassette .TAP (E6)
│          S9  ░░░░░░░░░░ Stockage disque Sedoric (E7)
├── Août   S10 ░░░░░░░░░░ Filesystem hôte (E8)
├── Sept   S11 ░░░░░░░░░░ Outils de conversion (E9)
│          S12 ░░░░░░░░░░ Débogueur (E10)
├── Oct    S13 ░░░░░░░░░░ Optimisation (E11)
│          S14 ░░░░░░░░░░ Stabilisation (E11)
├── Nov    S15 ░░░░░░░░░░ Release v1.0.0 (E12)
├── Déc+   S16 ░░░░░░░░░░ Extensions (E13)
```

## Vélocité estimée

| Sprint | Story Points planifiés | Cumulé |
|--------|----------------------|--------|
| S0 | 16 | 16 |
| S1 | 31 | 47 |
| S2 | 25 | 72 |
| S3 | 21 | 93 |
| S4 | 26 | 119 |
| S5 | 24 | 143 |
| S6 | 13 | 156 |
| S7 | 28 | 184 |
| S8 | 23 | 207 |
| S9 | 21 | 228 |
| S10 | 26 | 254 |
| S11 | 23 | 277 |
| S12 | 29 | 306 |
| S13 | 21 | 327 |
| S14 | 13 | 340 |
| S15 | 19 | 359 |
| **Total** | **359** | |

## Burndown Chart (projection)

```
SP
360 |*
330 |  *
300 |    *
270 |      *
240 |        *
210 |          *
180 |            *
150 |              *
120 |                *
 90 |                  *
 60 |                    *
 30 |                      *
  0 |________________________*___
    S0 S1 S2 S3 S4 S5 S6 S7 S8 S9 S10 S11 S12 S13 S14 S15
```

---

# Métriques de suivi

## KPIs du projet

| Métrique | Cible | Actuel |
|----------|-------|--------|
| Vélocité moyenne/sprint | 24 SP | N/A |
| Couverture de code | >90% | 0% |
| Bugs ouverts critiques | 0 | 0 |
| Bugs ouverts totaux | <10 | 0 |
| Warnings compilation | 0 | 12 |
| Tests unitaires | >200 | 5 |
| Tests d'intégration | >20 | 0 |
| Documentation pages | >30 | 5 |

## Définition des priorités

| Priorité | Signification |
|----------|---------------|
| **Critique** | Bloquant - sans ce composant l'émulateur ne fonctionne pas |
| **Haute** | Essentiel - fonctionnalité core attendue par les utilisateurs |
| **Moyenne** | Important - valeur ajoutée significative |
| **Basse** | Souhaitable - amélioration de l'expérience |

## Risques et mitigations

| Risque | Impact | Probabilité | Mitigation |
|--------|--------|-------------|------------|
| Timing CPU incorrect | Critique | Moyenne | Test suite Klaus Dormann |
| Incompatibilité programmes | Haute | Haute | Base de test large |
| Performance insuffisante | Moyenne | Basse | Profiling régulier |
| Scope creep | Moyenne | Haute | Sprint planning strict |
| Dépendance SDL2 | Basse | Basse | Abstraction renderer |

---

# Conventions du projet

## Git
- **Branches**: feature/<epic>-<description>, bugfix/<description>
- **Commits**: type: description (feat, fix, refactor, test, docs)
- **Tags**: v<MAJOR>.<MINOR>.<PATCH>-<label>

## Code
- **Langage**: C11
- **Style**: snake_case fonctions, UPPER_CASE constantes
- **Headers**: include guards, documentation Doxygen
- **Tests**: 1 fichier test par module, nommage test_<module>.c

## Documentation
- **CHANGELOG**: Mis à jour à chaque commit
- **ROADMAP**: Revu à chaque fin de sprint
- **CIRRUS_OS**: Mis à jour après chaque build/test
- **VERSION_TRACKING**: Mis à jour à chaque release
- **AGILE_PLAN**: Mis à jour à chaque sprint planning

---

**Document généré le**: 2026-02-22
**Prochain Sprint Planning**: Sprint 1 (S1) - CPU 6502
**Prochaine rétrospective**: Fin Sprint 0
