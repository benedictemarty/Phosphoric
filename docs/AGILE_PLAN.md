# AGILE PLAN - Phosphoric
## Complete Agile Planning Document

**Project**: Phosphoric — bus-cycle-accurate ORIC-1/Atmos emulator
**Document version**: 1.0.0
**Creation date**: 2026-02-22
**Owner**: bmarty <bmarty@mailo.com>
**Methodology**: Scrum / Agile
**Sprint length**: 2 weeks

---

## Product Vision

> Create Phosphoric, a bus-cycle-accurate ORIC-1/Atmos emulator written in C,
> with modern features (host file sharing, conversion
> tools) to preserve and revive the ORIC (1983) software ecosystem.

---

## Product Backlog - Epics Overview

| # | Epic | Priority | Phase | Sprints | Target version |
|---|------|----------|-------|---------|---------------|
| E0 | Infrastructure & Build | Critical | 0 | S0 | 0.1.0-alpha |
| E1 | 6502 CPU | Critical | 1 | S1-S2 | 0.2.0-alpha |
| E2 | Memory System | Critical | 1 | S3 | 0.3.0-alpha |
| E3 | I/O System (VIA 6522) | Critical | 1 | S3-S4 | 0.3.0-alpha |
| E4 | Video System | High | 2 | S5-S6 | 0.4.0-alpha |
| E5 | Audio System (AY-3-8910) | High | 2 | S7 | 0.5.0-alpha |
| E6 | Cassette Storage (.TAP) | High | 3 | S8 | 0.6.0-alpha |
| E7 | Disk Storage (Sedoric) | Medium | 3 | S9 | 0.7.0-alpha |
| E8 | Host File System | Medium | 3 | S10 | 0.8.0-alpha |
| E9 | Conversion Tools | Medium | 4 | S11 | 0.9.0-alpha |
| E10 | Debugger & Dev Tools | Low | 4 | S12 | 0.9.5-beta |
| E11 | Optimisation & Stabilisation | High | 5 | S13-S14 | 0.9.9-rc |
| E12 | Release v1.0.0 | Critical | 5 | S15 | 1.0.0 |
| E13 | Post-Release Extensions | Low | 6+ | S16+ | 1.x.x |

---

## Epic Dependency Diagram

```
E0 (Infrastructure)
 └──► E1 (CPU 6502)
       ├──► E2 (Memory)
       │     ├──► E3 (I/O VIA 6522)
       │     │     ├──► E4 (Video)
       │     │     ├──► E5 (Audio)
       │     │     └──► E6 (Cassette Storage)
       │     │           └──► E7 (Disk Storage)
       │     └──► E8 (HostFS)
       └──► E10 (Debugger)
 E6 + E7 ──► E9 (Conversion Tools)
 E4 + E5 + E6 + E7 + E8 ──► E11 (Optimisation)
 E11 ──► E12 (Release)
 E12 ──► E13 (Extensions)
```

---

# EPIC E0: Infrastructure & Build System

**Priority**: Critical
**Status**: In progress (30%)
**Target version**: 0.1.0-alpha
**Sprint**: S0 (Sprint 0)

## Description
Setting up the project infrastructure: Git repository, build system,
directory structure, agile documentation, CI/CD.

## User Stories

### US-E0-01: Project structure
> As a developer, I want a modular project structure to
> organise the code by component (CPU, memory, I/O, video, audio, storage).

**Story Points**: 3
**Status**: Done

**Acceptance criteria**:
- [x] Directories src/, include/, tests/, tools/, docs/, roms/ created
- [x] Per-module subdirectories (cpu, memory, io, video, audio, storage, hostfs, utils)
- [x] Skeleton headers and source files in place

**Tasks**:
- [x] T-001: Create the directory tree
- [x] T-002: Create the header files (.h) with interfaces
- [x] T-003: Create the skeleton source files (.c)
- [x] T-004: Create the .gitignore

### US-E0-02: Build system
> As a developer, I want a reliable build system to compile
> the emulator and the tests quickly.

**Story Points**: 5
**Status**: In progress

**Acceptance criteria**:
- [x] Working Makefile (builds without CMake)
- [x] CMakeLists.txt configured (removed in 2.1.3, out of sync with the Makefile)
- [ ] Build without warnings (-Wall -Wextra -Wpedantic)
- [ ] Targets: all, tests, tools, clean, coverage
- [ ] Debug and release builds

**Tasks**:
- [x] T-005: Create the Makefile
- [x] T-006: Create CMakeLists.txt
- [ ] T-007: Fix the compilation warnings (12 warnings currently)
- [ ] T-008: Add a coverage target to the Makefile

### US-E0-03: Agile documentation
> As a project manager, I want complete agile documentation to
> track the progress of the project.

**Story Points**: 3
**Status**: Done

**Acceptance criteria**:
- [x] Complete README.md
- [x] CHANGELOG initialised
- [x] ROADMAP defined
- [x] VERSION_TRACKING in place
- [x] CIRRUS_OS status file
- [x] AGILE_PLAN (this document)

**Tasks**:
- [x] T-009: Write README.md
- [x] T-010: Create CHANGELOG
- [x] T-011: Create ROADMAP
- [x] T-012: Create VERSION_TRACKING
- [x] T-013: Create CIRRUS_OS
- [x] T-014: Create AGILE_PLAN.md

### US-E0-04: Git repository
> As a developer, I want a Git repository configured with a remote
> to version and back up the code.

**Story Points**: 2
**Status**: Partial

**Acceptance criteria**:
- [x] Local repository initialised
- [x] user/email configuration (bmarty / bmarty@mailo.com)
- [ ] Remote configured and working
- [ ] main branch as the primary branch
- [ ] Commit convention defined

**Tasks**:
- [x] T-015: Initialise the Git repository
- [ ] T-016: Configure the remote
- [ ] T-017: Merge master into main
- [ ] T-018: Define a branch naming convention

### US-E0-05: Test framework
> As a developer, I want a unit and integration test framework
> to validate each component.

**Story Points**: 3
**Status**: Partial

**Acceptance criteria**:
- [x] tests/unit/ and tests/integration/ structure
- [x] Basic CPU tests (init, reset)
- [x] Basic memory tests (init, read/write)
- [x] Basic I/O tests (init)
- [ ] Custom ASSERT macro with clear messages
- [ ] Automated test report
- [ ] Measurable code coverage

**Tasks**:
- [x] T-019: Create the test structure
- [x] T-020: Write initial test_cpu.c
- [x] T-021: Write initial test_memory.c
- [x] T-022: Write initial test_io.c
- [ ] T-023: Create a mini test framework (assert macros, reporting)
- [ ] T-024: Integrate gcov/lcov for coverage

### Definition of Done - Epic E0
- [x] Complete project structure
- [x] Working build
- [ ] 0 compilation warnings
- [x] Basic tests pass
- [x] Agile documentation in place
- [ ] Git remote configured

---

# EPIC E1: 6502 CPU - Processor core

**Priority**: Critical (blocking for all the other epics)
**Status**: To do
**Target version**: 0.2.0-alpha
**Sprints**: S1, S2

## Description
Complete, bus-cycle-accurate implementation of the MOS Technology 6502
processor clocked at 1 MHz as used in the ORIC-1 (exact cycle counting
per opcode and bus accesses timed on the right cycle; internal non-bus
cycles are caught up at the end of the instruction rather than micro-cycled). It is the most
critical component of the emulator.

## User Stories

### US-E1-01: Addressing modes
> As a developer, I want the 13 addressing modes of the 6502
> implemented so that opcodes can resolve their operands.

**Story Points**: 8
**Status**: To do
**Sprint**: S1

**Acceptance criteria**:
- [ ] All 13 addressing modes working
- [ ] Unit tests for each mode
- [ ] Page boundary crossing handled (extra cycle)

**Addressing modes**:
| # | Mode | Syntax | Example |
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

**Tasks**:
- [ ] T-100: Implement addr_implicit()
- [ ] T-101: Implement addr_accumulator()
- [ ] T-102: Implement addr_immediate()
- [ ] T-103: Implement addr_zero_page()
- [ ] T-104: Implement addr_zero_page_x()
- [ ] T-105: Implement addr_zero_page_y()
- [ ] T-106: Implement addr_absolute()
- [ ] T-107: Implement addr_absolute_x()
- [ ] T-108: Implement addr_absolute_y()
- [ ] T-109: Implement addr_indirect()
- [ ] T-110: Implement addr_indexed_indirect() (Indirect,X)
- [ ] T-111: Implement addr_indirect_indexed() (Indirect),Y
- [ ] T-112: Implement addr_relative()
- [ ] T-113: Write addressing-mode unit tests

### US-E1-02: Load/store instructions
> As a developer, I want the Load/Store instructions to
> move data between registers and memory.

**Story Points**: 5
**Status**: To do
**Sprint**: S1

**Acceptance criteria**:
- [ ] LDA, LDX, LDY working
- [ ] STA, STX, STY working
- [ ] N and Z flags updated correctly
- [ ] Unit tests for each instruction

**Instructions**:
| Opcode | Name | Description | Flags |
|--------|-----|-------------|-------|
| LDA | Load Accumulator | A ← M | N, Z |
| LDX | Load X Register | X ← M | N, Z |
| LDY | Load Y Register | Y ← M | N, Z |
| STA | Store Accumulator | M ← A | - |
| STX | Store X Register | M ← X | - |
| STY | Store Y Register | M ← Y | - |

**Tasks**:
- [ ] T-114: Implement LDA (8 addressing modes)
- [ ] T-115: Implement LDX (5 addressing modes)
- [ ] T-116: Implement LDY (5 addressing modes)
- [ ] T-117: Implement STA (7 addressing modes)
- [ ] T-118: Implement STX (3 addressing modes)
- [ ] T-119: Implement STY (3 addressing modes)
- [ ] T-120: Load/Store unit tests

### US-E1-03: Arithmetic instructions
> As a developer, I want the arithmetic instructions to
> perform additions, subtractions and comparisons.

**Story Points**: 8
**Status**: To do
**Sprint**: S1

**Acceptance criteria**:
- [ ] ADC, SBC working (binary and decimal mode)
- [ ] CMP, CPX, CPY working
- [ ] INC, DEC, INX, INY, DEX, DEY working
- [ ] Flags updated correctly (N, Z, C, V)

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

**Tasks**:
- [ ] T-121: Implement ADC (binary mode)
- [ ] T-122: Implement ADC (BCD decimal mode)
- [ ] T-123: Implement SBC (binary mode)
- [ ] T-124: Implement SBC (BCD decimal mode)
- [ ] T-125: Implement CMP, CPX, CPY
- [ ] T-126: Implement INC, DEC
- [ ] T-127: Implement INX, INY, DEX, DEY
- [ ] T-128: Complete arithmetic tests (overflow, carry, BCD)

### US-E1-04: Logical instructions
> As a developer, I want the logical instructions (AND, OR, XOR,
> shifts, rotations) for bit manipulation.

**Story Points**: 5
**Status**: To do
**Sprint**: S1

**Acceptance criteria**:
- [ ] AND, ORA, EOR working
- [ ] ASL, LSR, ROL, ROR working
- [ ] BIT working
- [ ] Flags updated correctly

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

**Tasks**:
- [ ] T-129: Implement AND, ORA, EOR
- [ ] T-130: Implement ASL, LSR (accumulator and memory)
- [ ] T-131: Implement ROL, ROR (accumulator and memory)
- [ ] T-132: Implement BIT
- [ ] T-133: Complete logical tests

### US-E1-05: Branch instructions
> As a developer, I want the conditional branch instructions
> for flow control.

**Story Points**: 5
**Status**: To do
**Sprint**: S1

**Acceptance criteria**:
- [ ] All 8 conditional branches working
- [ ] Extra cycle if the branch is taken
- [ ] Extra cycle if a page boundary is crossed

**Instructions**:
| Opcode | Condition | Meaning |
|--------|-----------|---------------|
| BCC | C=0 | Branch if Carry Clear |
| BCS | C=1 | Branch if Carry Set |
| BEQ | Z=1 | Branch if Equal (Zero set) |
| BNE | Z=0 | Branch if Not Equal |
| BMI | N=1 | Branch if Minus |
| BPL | N=0 | Branch if Plus |
| BVS | V=1 | Branch if Overflow Set |
| BVC | V=0 | Branch if Overflow Clear |

**Tasks**:
- [ ] T-134: Implement the 8 branch instructions
- [ ] T-135: Handle the extra cycle (branch taken)
- [ ] T-136: Handle the extra cycle (page crossing)
- [ ] T-137: Complete branch tests

### US-E1-06: Jump and subroutine instructions
> As a developer, I want JMP, JSR, RTS, RTI to handle
> jumps and subroutine calls.

**Story Points**: 5
**Status**: To do
**Sprint**: S1

**Acceptance criteria**:
- [ ] JMP (absolute and indirect) working
- [ ] JSR/RTS working with the stack
- [ ] RTI working (return from interrupt)
- [ ] JMP indirect page-boundary bug reproduced (hardware bug)

**Instructions**:
| Opcode | Description | Notes |
|--------|-------------|-------|
| JMP | Jump | Abs and (Indirect) |
| JSR | Jump to Subroutine | Push PC-1 onto stack |
| RTS | Return from Subroutine | Pull PC+1 from stack |
| RTI | Return from Interrupt | Pull P then PC |

**Tasks**:
- [ ] T-138: Implement JMP absolute
- [ ] T-139: Implement JMP indirect (with page-boundary hardware bug)
- [ ] T-140: Implement JSR
- [ ] T-141: Implement RTS
- [ ] T-142: Implement RTI
- [ ] T-143: Jump and subroutine tests

### US-E1-07: Stack instructions
> As a developer, I want the stack manipulation instructions
> to save/restore registers and flags.

**Story Points**: 3
**Status**: To do
**Sprint**: S2

**Acceptance criteria**:
- [ ] PHA, PLA working
- [ ] PHP, PLP working
- [ ] TXS, TSX working
- [ ] Stack in page $01xx

**Instructions**:
| Opcode | Description | Flags |
|--------|-------------|-------|
| PHA | Push Accumulator | - |
| PLA | Pull Accumulator | N, Z |
| PHP | Push Processor Status | - |
| PLP | Pull Processor Status | All |
| TXS | Transfer X to SP | - |
| TSX | Transfer SP to X | N, Z |

**Tasks**:
- [ ] T-144: Implement PHA, PLA
- [ ] T-145: Implement PHP, PLP
- [ ] T-146: Implement TXS, TSX
- [ ] T-147: Complete stack tests

### US-E1-08: Register transfer instructions
> As a developer, I want the instructions that transfer between
> registers (TAX, TXA, TAY, TYA).

**Story Points**: 2
**Status**: To do
**Sprint**: S2

**Acceptance criteria**:
- [ ] TAX, TXA, TAY, TYA working
- [ ] N, Z flags updated

**Tasks**:
- [ ] T-148: Implement TAX, TXA, TAY, TYA
- [ ] T-149: Register transfer tests

### US-E1-09: Flag control instructions
> As a developer, I want the instructions that manipulate the processor
> flags (SEC, CLC, SEI, CLI, SED, CLD, CLV).

**Story Points**: 2
**Status**: To do
**Sprint**: S2

**Acceptance criteria**:
- [ ] SEC, CLC, SEI, CLI, SED, CLD, CLV working
- [ ] NOP working
- [ ] BRK working (software interrupt)

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

**Tasks**:
- [ ] T-150: Implement CLC, SEC, CLI, SEI, CLD, SED, CLV
- [ ] T-151: Implement NOP
- [ ] T-152: Implement BRK (vector $FFFE)
- [ ] T-153: Flag and control tests

### US-E1-10: Interrupt system
> As a developer, I want interrupt handling (IRQ, NMI,
> RESET) so that the CPU reacts to hardware events.

**Story Points**: 5
**Status**: To do
**Sprint**: S2

**Acceptance criteria**:
- [ ] IRQ working (maskable, vector $FFFE)
- [ ] NMI working (non-maskable, vector $FFFA)
- [ ] RESET working (vector $FFFC)
- [ ] Correct priority: RESET > NMI > IRQ
- [ ] I flag honoured for IRQ

**Tasks**:
- [ ] T-154: Implement the complete IRQ mechanism
- [ ] T-155: Implement the complete NMI mechanism
- [ ] T-156: Implement the RESET sequence
- [ ] T-157: Implement interrupt priority
- [ ] T-158: Complete interrupt tests

### US-E1-11: Opcode decoding table
> As a developer, I want a decoding table for the 256 opcodes
> to execute instructions efficiently.

**Story Points**: 8
**Status**: To do
**Sprint**: S2

**Acceptance criteria**:
- [ ] 256-entry table (opcode → handler)
- [ ] 151 official opcodes mapped
- [ ] 105 illegal opcodes handled (NOP or trap)
- [ ] Correct cycle count for every opcode

**Tasks**:
- [ ] T-159: Create the opcode_entry_t structure (handler, mode, cycles, name)
- [ ] T-160: Fill the 256-opcode table
- [ ] T-161: Implement cpu_step() with fetch-decode-execute
- [ ] T-162: Check the cycle counts against the hardware documentation
- [ ] T-163: Decoding table tests

### US-E1-12: Main execution loop
> As a developer, I want a working CPU execution loop
> so that the emulator can run programs.

**Story Points**: 5
**Status**: To do
**Sprint**: S2

**Acceptance criteria**:
- [ ] cpu_step() runs one complete fetch-decode-execute cycle
- [ ] cpu_execute_cycles() runs N cycles
- [ ] Correct timing (1 MHz = 1,000,000 cycles/second)
- [ ] Passes the 6502 validation test ROMs

**Tasks**:
- [ ] T-164: Implement the fetch-decode-execute cycle
- [ ] T-165: Integrate the addressing modes into execution
- [ ] T-166: Validate with Klaus Dormann's 6502 test suite
- [ ] T-167: CPU integration tests

### US-E1-13: Disassembler
> As a developer, I want a built-in disassembler to display
> instructions as readable mnemonics.

**Story Points**: 3
**Status**: To do
**Sprint**: S2

**Acceptance criteria**:
- [ ] cpu_disassemble() returns the correct mnemonic
- [ ] Operands displayed according to the addressing mode
- [ ] Standard format: "ADDR  HEX  MNEMONIC OPERAND"

**Tasks**:
- [ ] T-168: Implement the mnemonic table
- [ ] T-169: Format the disassembler output
- [ ] T-170: Disassembler tests

### Definition of Done - Epic E1
- [ ] 151 official opcodes implemented and tested
- [ ] 13 addressing modes working
- [ ] Cycle counts checked (±0 cycle vs hardware)
- [ ] Interrupts (IRQ, NMI, RESET) working
- [ ] Passes Klaus Dormann's 6502 test suite
- [ ] 100% coverage of the CPU code
- [ ] Complete API documentation
- [ ] 0 compilation warnings

---

# EPIC E2: Memory System

**Priority**: Critical
**Status**: Partial (70% base)
**Target version**: 0.3.0-alpha
**Sprint**: S3

## Description
Complete management of the ORIC-1's 64KB memory space, including ROM/RAM
banking, the memory-mapped I/O space, and the machine-specific memory map.

## ORIC-1 memory map
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

### US-E2-01: Full memory access
> As a developer, I want correct read/write access to the whole
> 64KB address space with the ORIC-1 mapping.

**Story Points**: 5
**Status**: Partial

**Acceptance criteria**:
- [x] RAM read/write working
- [ ] I/O space ($B000-$BFFF) routed to the peripherals
- [ ] ROM write protection
- [ ] Correct Zero Page wrapping

**Tasks**:
- [x] T-200: Basic RAM read/write
- [ ] T-201: Route the I/O space to the VIA callbacks
- [ ] T-202: ROM write protection (with optional logging)
- [ ] T-203: Complete memory access tests

### US-E2-02: ROM/RAM banking
> As a developer, I want the banking mechanism to switch
> between ROM and RAM in the $C000-$FFFF area.

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] RAM overlay under the ROM possible
- [ ] ROM/RAM switching through a register
- [ ] Interrupt vectors always reachable in ROM

**Tasks**:
- [ ] T-204: Implement the overlay mechanism
- [ ] T-205: Implement switching through an I/O register
- [ ] T-206: Handle access to the interrupt vectors
- [ ] T-207: Complete banking tests

### US-E2-03: ROM loading
> As a user, I want to load the ORIC-1 ROMs (BASIC 1.0,
> charset) to make the emulator work.

**Story Points**: 3
**Status**: Partial

**Acceptance criteria**:
- [x] ROM loading from a file
- [x] Charset loading from a file
- [ ] ROM checksum verification
- [ ] BASIC 1.0 ROM by default when available

**Tasks**:
- [x] T-208: Implement memory_load_rom()
- [x] T-209: Implement memory_load_charset()
- [ ] T-210: Add checksum verification
- [ ] T-211: ROM loading tests

### US-E2-04: Memory tracing
> As a developer, I want to trace memory accesses to
> debug ORIC programs.

**Story Points**: 3
**Status**: Partial

**Acceptance criteria**:
- [x] Tracing infrastructure in place
- [ ] Read/write callbacks per address range
- [ ] Memory breakpoints (watch)
- [ ] I/O access log

**Tasks**:
- [ ] T-212: Implement per-address-range callbacks
- [ ] T-213: Implement memory breakpoints
- [ ] T-214: Log I/O accesses
- [ ] T-215: Tracing tests

### Definition of Done - Epic E2
- [ ] Complete and correct ORIC-1 memory map
- [ ] ROM/RAM banking working
- [ ] I/O space correctly routed
- [ ] Memory tracing operational
- [ ] Tests covering all edge cases
- [ ] Memory map documentation

---

# EPIC E3: I/O System (VIA 6522)

**Priority**: Critical
**Status**: Stub (15%)
**Target version**: 0.3.0-alpha
**Sprints**: S3, S4

## Description
Emulation of the MOS 6522 VIA (Versatile Interface Adapter) input/output
controller used in the ORIC-1 for the keyboard, the cassette, the printer,
and as a bridge to the AY-3-8910 PSG.

## User Stories

### US-E3-01: VIA 6522 registers
> As a developer, I want the 16 VIA 6522 registers correctly
> emulated so that the peripherals work.

**Story Points**: 8
**Status**: To do

**Registers**:
| Offset | Name | Description |
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

**Tasks**:
- [ ] T-300: Implement reading of the 16 registers
- [ ] T-301: Implement writing of the 16 registers
- [ ] T-302: Implement the DDRs (data direction)
- [ ] T-303: Complete register tests

### US-E3-02: VIA timers
> As a developer, I want both VIA timers working
> for peripheral timing and interrupt generation.

**Story Points**: 8
**Status**: To do

**Acceptance criteria**:
- [ ] Timer 1: one-shot and free-running modes
- [ ] Timer 2: one-shot and pulse counting modes
- [ ] IRQ generation on timeout
- [ ] Latches working

**Tasks**:
- [ ] T-304: Implement Timer 1 (one-shot)
- [ ] T-305: Implement Timer 1 (free-running)
- [ ] T-306: Implement Timer 2 (one-shot)
- [ ] T-307: Implement Timer 2 (pulse counting)
- [ ] T-308: Implement timer IRQ generation
- [ ] T-309: Complete timer tests

### US-E3-03: Shift Register
> As a developer, I want the VIA shift register for
> serial communication (cassette, printer).

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] 8 shift register modes working
- [ ] IRQ generation on transfer complete

**Tasks**:
- [ ] T-310: Implement the 8 shift register modes
- [ ] T-311: Implement the shift register IRQ
- [ ] T-312: Shift register tests

### US-E3-04: ORIC keyboard
> As a user, I want to type on my PC keyboard and see the
> matching keys on the ORIC.

**Story Points**: 8
**Status**: To do

**Acceptance criteria**:
- [ ] 8x8 keyboard matrix emulated
- [ ] Correct PC → ORIC mapping
- [ ] Column scan through Port B
- [ ] Read through Port A
- [ ] Special keys (FUNCT, CTRL, SHIFT)

**ORIC-1 keyboard matrix** (8 columns x 8 rows):
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

**Tasks**:
- [ ] T-313: Implement the 8x8 keyboard matrix
- [ ] T-314: Create the PC → ORIC mapping table
- [ ] T-315: Integrate the keyboard scan with the VIA (Port A/B)
- [ ] T-316: Handle the special keys
- [ ] T-317: Integrate SDL2 for keyboard capture
- [ ] T-318: Keyboard tests

### US-E3-05: Cassette interface through the VIA
> As a developer, I want the VIA cassette interface to
> load and save programs on tape.

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] Cassette signal through CB1/CB2
- [ ] Cassette motor control
- [ ] Serial bit read/write

**Tasks**:
- [ ] T-319: Implement the CB1/CB2 cassette signal
- [ ] T-320: Implement motor control
- [ ] T-321: Implement bit serialisation
- [ ] T-322: Cassette interface tests

### US-E3-06: VIA interrupts
> As a developer, I want the complete VIA interrupt system
> for asynchronous events.

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] IFR (Interrupt Flag Register) working
- [ ] IER (Interrupt Enable Register) working
- [ ] IRQ generation to the CPU
- [ ] Sources: Timer 1, Timer 2, CB1, CB2, SR, CA1, CA2

**Tasks**:
- [ ] T-323: Implement IFR (read and clear)
- [ ] T-324: Implement IER (set and clear)
- [ ] T-325: Connect the interrupt sources
- [ ] T-326: Implement via_update() for the per-cycle tick
- [ ] T-327: VIA interrupt tests

### Definition of Done - Epic E3
- [ ] 16 VIA registers read/written correctly
- [ ] Timers 1 and 2 working in all modes
- [ ] Shift register operational
- [ ] Keyboard working with complete mapping
- [ ] Cassette interface working
- [ ] Complete interrupt system
- [ ] Tests covering all modes
- [ ] VIA 6522 documentation

---

# EPIC E4: Video System

**Priority**: High
**Status**: To do (0%)
**Target version**: 0.4.0-alpha
**Sprints**: S5, S6

## Description
Emulation of the ORIC-1 video system based on the ULA (Uncommitted Logic
Array), supporting the 40x28 text mode and the 240x200 HIRES mode, with
colour attribute handling and rendering through SDL2.

## User Stories

### US-E4-01: Text mode (40x28)
> As a user, I want to see the ORIC text display in order
> to interact with BASIC and text-based programs.

**Story Points**: 8
**Status**: To do

**Acceptance criteria**:
- [ ] 40 columns x 28 lines display
- [ ] ORIC character font (6x8 ROM charset)
- [ ] Standard and alternate character sets
- [ ] Blinking cursor

**Tasks**:
- [ ] T-400: Implement 40x28 text rendering
- [ ] T-401: Load and use the ROM charset
- [ ] T-402: Implement the alternate charset
- [ ] T-403: Implement the blinking cursor
- [ ] T-404: Text mode tests

### US-E4-02: HIRES mode (240x200)
> As a user, I want to see high-resolution graphics
> for ORIC games and graphics programs.

**Story Points**: 8
**Status**: To do

**Acceptance criteria**:
- [ ] 240x200 pixel resolution
- [ ] 6 colours (black, red, green, yellow, blue, magenta, cyan, white)
- [ ] Mixed text+hires mode possible
- [ ] Correct video memory addressing ($A000-$BF3F)

**Tasks**:
- [ ] T-405: Implement 240x200 HIRES rendering
- [ ] T-406: Implement video line decoding
- [ ] T-407: Handle the mixed mode
- [ ] T-408: HIRES mode tests

### US-E4-03: Colour attributes
> As a user, I want correct colours on screen with
> the ORIC serial attribute system.

**Story Points**: 8
**Status**: To do

**Acceptance criteria**:
- [ ] Serial attributes at the start of a line
- [ ] Ink and paper colours (foreground/background)
- [ ] Inverse attributes
- [ ] Blinking (blink)
- [ ] Double height

**ORIC attributes**:
| Code | Meaning |
|------|---------------|
| 0-7 | Ink colour (ink) |
| 8-15 | Style (normal, alt charset, double, blink) |
| 16-23 | Paper colour (paper) |
| 24-31 | Control (60Hz, flash, etc.) |

**Tasks**:
- [ ] T-409: Implement serial attribute parsing
- [ ] T-410: Implement ink/paper
- [ ] T-411: Implement inverse and blink
- [ ] T-412: Implement double height
- [ ] T-413: Colour attribute tests

### US-E4-04: SDL2 video backend
> As a developer, I want efficient SDL2 rendering to display
> the emulator's video output.

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] SDL2 window with configurable size
- [ ] 50Hz rendering (PAL)
- [ ] Fullscreen mode (F11)
- [ ] Screenshot (F12)
- [ ] Optional filters (scanlines, CRT)

**Tasks**:
- [ ] T-414: Create the SDL2 window and renderer
- [ ] T-415: Implement framebuffer → texture rendering
- [ ] T-416: Implement 50Hz vsync
- [ ] T-417: Implement fullscreen mode
- [ ] T-418: Implement screenshots (PNG)
- [ ] T-419: Video rendering tests

### Definition of Done - Epic E4
- [ ] 40x28 text mode with correct charset
- [ ] Working 240x200 HIRES mode
- [ ] Correct colours and attributes
- [ ] Stable 50Hz SDL2 rendering
- [ ] Working screenshots
- [ ] Complete video tests
- [ ] Video system documentation

---

# EPIC E5: Audio System (AY-3-8910)

**Priority**: High
**Status**: To do (0%)
**Target version**: 0.5.0-alpha
**Sprint**: S7

## Description
Emulation of the General Instrument AY-3-8910 PSG sound chip (Programmable Sound
Generator), connected through Port A of the VIA 6522.

## User Stories

### US-E5-01: Tone generators
> As a user, I want to hear the sounds and music of ORIC
> programs with the 3 tone channels of the PSG.

**Story Points**: 8
**Status**: To do

**Acceptance criteria**:
- [ ] 3 independent tone channels (A, B, C)
- [ ] Adjustable frequency (12 bits per channel)
- [ ] Correct channel mixing

**Tasks**:
- [ ] T-500: Implement the 3 tone generators
- [ ] T-501: Implement frequency setting (registers R0-R5)
- [ ] T-502: Implement channel mixing
- [ ] T-503: Tone tests

### US-E5-02: Noise generator
> As a user, I want the noise generator for sound
> effects (explosions, gunshots, etc.).

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] Pseudo-random noise generator
- [ ] Adjustable frequency (5 bits)
- [ ] Mixable with the tone channels

**Tasks**:
- [ ] T-504: Implement the LFSR (Linear Feedback Shift Register)
- [ ] T-505: Implement noise frequency setting (R6)
- [ ] T-506: Implement the tone/noise mixer (R7)
- [ ] T-507: Noise tests

### US-E5-03: Envelopes
> As a user, I want volume envelopes for dynamic
> and expressive sounds.

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] 16 envelope shapes (register R13)
- [ ] Adjustable period (16 bits, R11-R12)
- [ ] Fixed volume or envelope per channel (R8-R10)

**Tasks**:
- [ ] T-508: Implement the 16 envelope shapes
- [ ] T-509: Implement period control
- [ ] T-510: Implement the fixed volume/envelope selector
- [ ] T-511: Envelope tests

### US-E5-04: VIA → PSG interface
> As a developer, I want the VIA → AY-3-8910 connection so
> that the CPU can control the PSG.

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] Communication through VIA Port A
- [ ] BDIR/BC1 protocol (Inactive, Read, Write, Latch Address)
- [ ] 14 accessible PSG registers

**Tasks**:
- [ ] T-512: Implement the BDIR/BC1 protocol
- [ ] T-513: Connect VIA Port A to the PSG
- [ ] T-514: Implement read/write of the 14 PSG registers
- [ ] T-515: VIA-PSG interface tests

### US-E5-05: SDL2 audio backend
> As a developer, I want audio output through SDL2 to produce
> sound on the host system.

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] SDL2 audio output (44100 Hz, 16 bits, stereo)
- [ ] Audio buffer with minimal latency
- [ ] Global volume control
- [ ] Mute/unmute

**Tasks**:
- [ ] T-516: Initialise SDL2 Audio
- [ ] T-517: Implement the audio callback (sample generation)
- [ ] T-518: Implement the audio ring buffer
- [ ] T-519: Implement volume and mute
- [ ] T-520: Audio output tests

### Definition of Done - Epic E5
- [ ] 3 working tone channels
- [ ] Working noise generator
- [ ] 16 working envelopes
- [ ] Correct VIA-PSG interface
- [ ] SDL2 output without crackling
- [ ] Complete audio tests
- [ ] PSG documentation

---

# EPIC E6: Cassette Storage (.TAP)

**Priority**: High
**Status**: Partial (30% structure)
**Target version**: 0.6.0-alpha
**Sprint**: S8

## Description
Full support for the .TAP cassette file format to load and save
ORIC programs, including turbo mode (fast load).

## User Stories

### US-E6-01: Reading .TAP files
> As a user, I want to load .TAP files to run
> ORIC programs and games.

**Story Points**: 8
**Status**: To do

**Acceptance criteria**:
- [ ] Complete parsing of the .TAP format
- [ ] Support for multiple programs per file
- [ ] Reading of headers (name, type, addresses)
- [ ] Loading of data into memory

**TAP format**:
```
[Sync bytes: $16 x N] [Header: $24 bytes] [Data: variable] [Checksum]
Header: sync | type | autorun | end_addr_hi | end_addr_lo |
        start_addr_hi | start_addr_lo | $00 | name[16]
```

**Tasks**:
- [ ] T-600: Implement tap_open_read() (file parsing)
- [ ] T-601: Implement tap_read_header() (header decoding)
- [ ] T-602: Implement tap_read_data() (data reading)
- [ ] T-603: Implement checksum verification
- [ ] T-604: Handle multi-program files
- [ ] T-605: TAP reading tests

### US-E6-02: Writing .TAP files
> As a user, I want to save my programs in .TAP format.

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] Creation of valid .TAP files
- [ ] Writing of correct headers
- [ ] Checksum computation and writing

**Tasks**:
- [ ] T-606: Implement tap_open_write()
- [ ] T-607: Implement tap_write_header()
- [ ] T-608: Implement tap_write_data()
- [ ] T-609: Implement checksum computation on write
- [ ] T-610: TAP writing tests

### US-E6-03: Fast Load (Turbo Tape)
> As a user, I want a fast loading mode so as not
> to wait for the real cassette loading times.

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] Instant loading into memory (bypassing cassette timing)
- [ ] Patching of the ROM loading routines
- [ ] Fast/normal load toggle

**Tasks**:
- [ ] T-611: Implement direct loading into memory
- [ ] T-612: Implement the ROM patch for fast load
- [ ] T-613: Add the --fast-load option
- [ ] T-614: Fast load tests

### US-E6-04: Cassette timing simulation
> As a developer, I want to simulate real cassette timing
> for compatibility with copy protections.

**Story Points**: 5
**Status**: To do

**Acceptance criteria**:
- [ ] Realistic bit timing (2400/1200 baud)
- [ ] Correct CB1/CB2 signals through the VIA
- [ ] Compatible with custom loaders

**Tasks**:
- [ ] T-615: Implement 2400/1200 baud timing
- [ ] T-616: Generate the signals through VIA CB1/CB2
- [ ] T-617: Cassette timing tests

### Definition of Done - Epic E6
- [ ] Working .TAP read/write
- [ ] Operational fast load
- [ ] Realistic cassette timing
- [ ] Compatible with the main ORIC programs
- [ ] Complete tests
- [ ] TAP format documentation

---

# EPIC E7: Disk Storage (Sedoric)

**Priority**: Medium
**Status**: To do (0%)
**Target version**: 0.7.0-alpha
**Sprint**: S9

## Description
Support for disk images (.DSK) with the Sedoric file system and
emulation of the Microdisc controller.

## User Stories

### US-E7-01: Microdisc controller
> As a developer, I want to emulate the Microdisc controller to
> access virtual floppy drives.

**Story Points**: 8
**Status**: To do

**Tasks**:
- [ ] T-700: Implement the FDC controller registers (WD1793)
- [ ] T-701: Implement the sector read commands
- [ ] T-702: Implement the sector write commands
- [ ] T-703: Implement seek/step/restore
- [ ] T-704: Implement track and sector handling
- [ ] T-705: FDC controller tests

### US-E7-02: .DSK format
> As a user, I want to load .DSK disk images to
> access programs on floppy disk.

**Story Points**: 5
**Status**: To do

**Tasks**:
- [ ] T-706: Implement .DSK format parsing
- [ ] T-707: Implement sector reading
- [ ] T-708: Implement sector writing
- [ ] T-709: Handle single/double-sided formats
- [ ] T-710: DSK format tests

### US-E7-03: Sedoric file system
> As a user, I want to browse and access files
> on Sedoric floppy disks.

**Story Points**: 8
**Status**: To do

**Tasks**:
- [ ] T-711: Implement Sedoric directory reading
- [ ] T-712: Implement file reading
- [ ] T-713: Implement file writing
- [ ] T-714: Implement free space management (FAT)
- [ ] T-715: Implement disk formatting
- [ ] T-716: Sedoric tests

### Definition of Done - Epic E7
- [ ] Working WD1793 FDC controller
- [ ] .DSK read/write
- [ ] Working Sedoric browsing
- [ ] Compatible with common images
- [ ] Complete tests
- [ ] Sedoric documentation

---

# EPIC E8: Host File System (HostFS)

**Priority**: Medium
**Status**: Partial (40% structure)
**Target version**: 0.8.0-alpha
**Sprint**: S10

## Description
File sharing between the host system (Linux) and the ORIC emulator,
allowing transparent exchange of programs and data.

## User Stories

### US-E8-01: Mounting a host directory
> As a user, I want to mount a directory from my PC in order
> to access it from the ORIC emulator.

**Story Points**: 5
**Status**: Partial

**Tasks**:
- [x] T-800: Implement hostfs_mount() / hostfs_unmount()
- [ ] T-801: Implement directory verification
- [ ] T-802: Handle permissions
- [ ] T-803: Mounting tests

### US-E8-02: File operations
> As a user, I want to read, write, delete and rename
> shared files.

**Story Points**: 8
**Status**: To do

**Tasks**:
- [ ] T-804: Implement hostfs_open()
- [ ] T-805: Implement hostfs_read() / hostfs_write()
- [ ] T-806: Implement hostfs_seek() / hostfs_close()
- [ ] T-807: Implement hostfs_delete() / hostfs_rename()
- [ ] T-808: Implement hostfs_list() (directory listing)
- [ ] T-809: File operation tests

### US-E8-03: Path conversion
> As a developer, I want transparent conversion between
> ORIC file names and host paths.

**Story Points**: 5
**Status**: To do

**Tasks**:
- [ ] T-810: Implement oric_to_host_path()
- [ ] T-811: Implement host_to_oric_name()
- [ ] T-812: Handle special characters and name lengths
- [ ] T-813: Path conversion tests

### US-E8-04: VFS layer (Virtual File System)
> As a developer, I want a VFS abstraction layer to
> unify file access (cassette, disk, host).

**Story Points**: 8
**Status**: To do

**Tasks**:
- [ ] T-814: Define the abstract VFS interface
- [ ] T-815: Implement the HostFS backend
- [ ] T-816: Implement the TAP backend
- [ ] T-817: Implement the Sedoric backend
- [ ] T-818: VFS tests

### Definition of Done - Epic E8
- [ ] Directory mounting/unmounting
- [ ] Working file CRUD
- [ ] Path conversion
- [ ] Unified VFS
- [ ] Complete tests
- [ ] HostFS documentation

---

# EPIC E9: Conversion Tools

**Priority**: Medium
**Status**: CLI done, backends to do
**Target version**: 0.9.0-alpha
**Sprint**: S11

## Description
Command-line tools to convert programs between the
various ORIC formats (BASIC, binary, .TAP, Sedoric).

## User Stories

### US-E9-01: bas2tap - BASIC → TAP converter
> As a user, I want to convert my BASIC programs into
> .TAP files to load them into the emulator.

**Story Points**: 8
**Status**: CLI done, backend to do

**Tasks**:
- [x] T-900: Create the bas2tap CLI (argument parsing)
- [ ] T-901: Implement the ORIC BASIC tokenizer
- [ ] T-902: Implement tap_from_basic()
- [ ] T-903: Handle the auto-run option
- [ ] T-904: bas2tap tests

### US-E9-02: bin2tap - Binary → TAP converter
> As a user, I want to convert machine code into .TAP
> with the load and execution addresses.

**Story Points**: 5
**Status**: CLI done, backend to do

**Tasks**:
- [x] T-905: Create the bin2tap CLI (argument parsing)
- [ ] T-906: Implement tap_from_binary()
- [ ] T-907: Handle start/exec addresses
- [ ] T-908: bin2tap tests

### US-E9-03: tap2sedoric - TAP → Sedoric converter
> As a user, I want to convert .TAP files into a Sedoric disk
> image to use them with the floppy disk drive.

**Story Points**: 5
**Status**: CLI done, backend to do

**Tasks**:
- [x] T-909: Create the tap2sedoric CLI (argument parsing)
- [ ] T-910: Implement the TAP → Sedoric conversion
- [ ] T-911: Create the disk image containing the file
- [ ] T-912: tap2sedoric tests

### US-E9-04: Hybrid BASIC + machine code support
> As a user, I want to create hybrid
> BASIC + machine code programs in a single .TAP.

**Story Points**: 5
**Status**: To do

**Tasks**:
- [ ] T-913: Implement attaching a binary to the BASIC program
- [ ] T-914: Implement multi-block support in a .TAP
- [ ] T-915: Hybrid program tests

### Definition of Done - Epic E9
- [ ] bas2tap correctly converts BASIC programs
- [ ] bin2tap correctly converts binaries
- [ ] tap2sedoric creates valid disk images
- [ ] Working hybrid support
- [ ] Complete tests for each tool
- [ ] Documentation and man pages

---

# EPIC E10: Debugger & Developer Tools

**Priority**: Low
**Status**: To do (0%)
**Target version**: 0.9.5-beta
**Sprint**: S12

## Description
Built-in debugging tools for ORIC program developers:
breakpoints, step, memory view, trace.

## User Stories

### US-E10-01: Breakpoints and Step
> As an ORIC developer, I want to set breakpoints and
> step through the code.

**Story Points**: 8
**Status**: To do

**Tasks**:
- [ ] T-1000: Implement address breakpoints
- [ ] T-1001: Implement step (1 instruction)
- [ ] T-1002: Implement step over (skip JSRs)
- [ ] T-1003: Implement run until (run up to an address)
- [ ] T-1004: Implement conditional breakpoints
- [ ] T-1005: Breakpoint tests

### US-E10-02: Memory view
> As an ORIC developer, I want to view memory in real
> time to understand how programs behave.

**Story Points**: 5
**Status**: To do

**Tasks**:
- [ ] T-1006: Implement the hex dump display
- [ ] T-1007: Implement memory search
- [ ] T-1008: Implement watchpoints (break on read/write)
- [ ] T-1009: Memory view tests

### US-E10-03: Register inspector
> As an ORIC developer, I want to see the state of the CPU
> and VIA registers in real time.

**Story Points**: 3
**Status**: To do

**Tasks**:
- [ ] T-1010: CPU register display (A, X, Y, SP, PC, P)
- [ ] T-1011: Detailed flag display (N V - B D I Z C)
- [ ] T-1012: VIA register display
- [ ] T-1013: PSG register display
- [ ] T-1014: Inspector tests

### US-E10-04: Trace and logging
> As an ORIC developer, I want to trace execution to analyse
> a program's behaviour.

**Story Points**: 5
**Status**: To do

**Tasks**:
- [ ] T-1015: Implement trace logging (every instruction)
- [ ] T-1016: Implement filtering by address range
- [ ] T-1017: Implement export to file
- [ ] T-1018: Implement the cycle counter
- [ ] T-1019: Trace tests

### US-E10-05: Debugger interface
> As an ORIC developer, I want a debugging interface
> reachable via F9 or the command line.

**Story Points**: 8
**Status**: To do

**Tasks**:
- [ ] T-1020: Create the debugger console interface
- [ ] T-1021: Implement the command parser
- [ ] T-1022: Implement the commands (break, step, mem, reg, trace, etc.)
- [ ] T-1023: Integrate the real-time disassembler
- [ ] T-1024: Debugger interface tests

### Definition of Done - Epic E10
- [ ] Working breakpoints (address, condition)
- [ ] Step / Step Over / Run Until
- [ ] Memory and register view
- [ ] Trace logging with filters
- [ ] Usable debugger interface
- [ ] Debugger documentation

---

# EPIC E11: Optimisation & Stabilisation

**Priority**: High
**Status**: To do
**Target version**: 0.9.9-rc
**Sprints**: S13, S14

## Description
Performance optimisation, bug fixing, stabilisation of
the whole emulator before the v1.0.0 release.

## User Stories

### US-E11-01: CPU optimisation
> As a user, I want the emulator to run smoothly and not
> consume too many resources.

**Story Points**: 8
**Status**: To do

**Tasks**:
- [ ] T-1100: Profile CPU execution
- [ ] T-1101: Optimise the fetch-decode-execute loop
- [ ] T-1102: Optimise memory accesses
- [ ] T-1103: Benchmark: target <5% CPU usage

### US-E11-02: Video optimisation
> As a user, I want smooth video rendering without stutter.

**Story Points**: 5
**Status**: To do

**Tasks**:
- [ ] T-1104: Optimise rendering (dirty rectangles)
- [ ] T-1105: Optimise the framebuffer → texture copy
- [ ] T-1106: Benchmark: constant 50 FPS

### US-E11-03: Configuration system
> As a user, I want to be able to configure the emulator
> (ROM paths, resolution, audio, controls).

**Story Points**: 5
**Status**: To do

**Tasks**:
- [ ] T-1107: Implement .ini file parsing
- [ ] T-1108: Implement command-line options
- [ ] T-1109: Save/load the configuration
- [ ] T-1110: Configuration tests

### US-E11-04: Robust error handling
> As a developer, I want complete error handling
> so that the emulator does not crash.

**Story Points**: 5
**Status**: To do

**Tasks**:
- [ ] T-1111: Audit every function for error cases
- [ ] T-1112: Add the missing checks
- [ ] T-1113: Implement graceful recovery
- [ ] T-1114: Robustness tests (fuzzing)

### US-E11-05: Quality audit
> As project manager, I want a complete audit before the release.

**Story Points**: 8
**Status**: To do

**Tasks**:
- [ ] T-1115: Static analysis (cppcheck, clang-tidy)
- [ ] T-1116: Memory leak detection (Valgrind)
- [ ] T-1117: Security audit (buffer overflows, format strings)
- [ ] T-1118: Code coverage > 90%
- [ ] T-1119: Fix all known bugs

### Definition of Done - Epic E11
- [ ] Performance: <5% CPU, stable 50 FPS
- [ ] 0 memory leaks (Valgrind clean)
- [ ] 0 compilation warnings
- [ ] Static analysis clean
- [ ] Coverage > 90%
- [ ] Working configuration
- [ ] All critical bugs fixed

---

# EPIC E12: Release v1.0.0

**Priority**: Critical
**Status**: To do
**Target version**: 1.0.0
**Sprint**: S15

## Description
Preparation and publication of the stable 1.0.0 version of the emulator.

## User Stories

### US-E12-01: User documentation
> As a user, I want a complete guide to using the emulator.

**Story Points**: 8
**Status**: To do

**Tasks**:
- [ ] T-1200: Write the complete user guide
- [ ] T-1201: Create the manual pages (man pages)
- [ ] T-1202: Document the keyboard shortcuts
- [ ] T-1203: Create a FAQ

### US-E12-02: Packaging
> As a user, I want to install the emulator easily on my
> Linux distribution.

**Story Points**: 5
**Status**: To do

**Tasks**:
- [ ] T-1204: Create the .deb package (Debian/Ubuntu)
- [ ] T-1205: Create the .rpm package (Fedora/RHEL)
- [ ] T-1206: Create the .tar.gz archive
- [ ] T-1207: Create the AppImage
- [ ] T-1208: Installation tests on target distributions

### US-E12-03: Example programs
> As a user, I want examples to discover the
> capabilities of the ORIC.

**Story Points**: 3
**Status**: To do

**Tasks**:
- [ ] T-1209: Create a BASIC demonstration program
- [ ] T-1210: Create a HIRES graphics program
- [ ] T-1211: Create a sound program
- [ ] T-1212: Package the examples with the emulator

### US-E12-04: Publication
> As project manager, I want to officially publish v1.0.0.

**Story Points**: 3
**Status**: To do

**Tasks**:
- [ ] T-1213: Create the Git tag v1.0.0
- [ ] T-1214: Write the release notes
- [ ] T-1215: Publish on GitHub
- [ ] T-1216: Announce on the ORIC forums (Defence Force, etc.)

### Definition of Done - Epic E12
- [ ] Complete, proofread documentation
- [ ] Packages tested on 3+ distributions
- [ ] Working examples included
- [ ] Release published on GitHub
- [ ] Community announcement

---

# EPIC E13: Post-Release Extensions

**Priority**: Low
**Status**: Planned
**Target version**: 1.x.x
**Sprints**: S16+

## Description
Additional features planned after v1.0.0, driven by
community feedback.

## Planned features

### Extended compatibility
| Feature | Priority | Story Points |
|---------|----------|-------------|
| ORIC Atmos support | High | 13 |
| Telestrat support | Medium | 21 |
| Pravetz-8D support | Low | 8 |

### Advanced features
| Feature | Priority | Story Points |
|---------|----------|-------------|
| Save States | High | 8 |
| Joystick support | Medium | 3 |
| Printer emulation | Low | 5 |
| Network (TCP/IP overlay) | Low | 13 |

### Developer tools
| Feature | Priority | Story Points |
|---------|----------|-------------|
| Performance profiler | Medium | 8 |
| ROM analysis tools | Low | 5 |
| Sprite editor | Low | 8 |

### Community
| Feature | Priority | Story Points |
|---------|----------|-------------|
| Plugin system | Medium | 13 |
| Game compatibility DB | High | 5 |
| Contribution guide | High | 3 |

---

# Sprint Planning

## Overall timeline

```
2026
├── Feb    S0  ████░░░░░░ Infrastructure (E0) ← WE ARE HERE
├── Mar    S1  ░░░░░░░░░░ CPU Addressing modes + Load/Store/Arith (E1)
│          S2  ░░░░░░░░░░ CPU Logic/Branch/Stack/Interrupts (E1)
├── Apr    S3  ░░░░░░░░░░ Complete memory + VIA registers (E2+E3)
│          S4  ░░░░░░░░░░ VIA timers + keyboard + cassette (E3)
├── May    S5  ░░░░░░░░░░ Text video + attributes (E4)
│          S6  ░░░░░░░░░░ HIRES video + SDL2 backend (E4)
├── Jun    S7  ░░░░░░░░░░ AY-3-8910 audio + SDL2 audio (E5)
├── Jul    S8  ░░░░░░░░░░ .TAP cassette storage (E6)
│          S9  ░░░░░░░░░░ Sedoric disk storage (E7)
├── Aug    S10 ░░░░░░░░░░ Host filesystem (E8)
├── Sep    S11 ░░░░░░░░░░ Conversion tools (E9)
│          S12 ░░░░░░░░░░ Debugger (E10)
├── Oct    S13 ░░░░░░░░░░ Optimisation (E11)
│          S14 ░░░░░░░░░░ Stabilisation (E11)
├── Nov    S15 ░░░░░░░░░░ Release v1.0.0 (E12)
├── Dec+   S16 ░░░░░░░░░░ Extensions (E13)
```

## Estimated velocity

| Sprint | Planned Story Points | Cumulative |
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

# Tracking Metrics

## Project KPIs

| Metric | Target | Current |
|----------|-------|--------|
| Average velocity/sprint | 24 SP | N/A |
| Code coverage | >90% | 0% |
| Open critical bugs | 0 | 0 |
| Total open bugs | <10 | 0 |
| Compilation warnings | 0 | 12 |
| Unit tests | >200 | 5 |
| Integration tests | >20 | 0 |
| Documentation pages | >30 | 5 |

## Priority definitions

| Priority | Meaning |
|----------|---------------|
| **Critical** | Blocking - without this component the emulator does not work |
| **High** | Essential - core feature expected by users |
| **Medium** | Important - significant added value |
| **Low** | Nice to have - improves the experience |

## Risks and mitigations

| Risk | Impact | Probability | Mitigation |
|--------|--------|-------------|------------|
| Incorrect CPU timing | Critical | Medium | Klaus Dormann test suite |
| Program incompatibility | High | High | Broad test base |
| Insufficient performance | Medium | Low | Regular profiling |
| Scope creep | Medium | High | Strict sprint planning |
| SDL2 dependency | Low | Low | Renderer abstraction |

---

# Project Conventions

## Git
- **Branches**: feature/<epic>-<description>, bugfix/<description>
- **Commits**: type: description (feat, fix, refactor, test, docs)
- **Tags**: v<MAJOR>.<MINOR>.<PATCH>-<label>

## Code
- **Language**: C11
- **Style**: snake_case functions, UPPER_CASE constants
- **Headers**: include guards, Doxygen documentation
- **Tests**: 1 test file per module, named test_<module>.c

## Documentation
- **CHANGELOG**: Updated on every commit
- **ROADMAP**: Reviewed at the end of every sprint
- **CIRRUS_OS**: Updated after every build/test
- **VERSION_TRACKING**: Updated on every release
- **AGILE_PLAN**: Updated at every sprint planning

---

**Document generated on**: 2026-02-22
**Next Sprint Planning**: Sprint 1 (S1) - CPU 6502
**Next retrospective**: End of Sprint 0
