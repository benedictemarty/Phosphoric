# Pushing the Oric's limits: five experimental extensions for the LOCI board

*How a bus-cycle-accurate emulator serves as a hardware test bench — with the ABI, the
registers and the diagrams.*

---

> **About the photos.** The pictures of the LOCI board below belong to
> their authors (RAXISS / sodiumlb). Links are provided for illustration and
> attribution — **ideally to be replaced by your own photos** (you own a
> board) or by images you have permission to republish.
>
> - LOCI board, product views: <https://www.raxiss.com/article/id/38-LOCI>
>   (e.g. `https://www.raxiss.com/images/resized/800x600-loci02g.jpg`)
> - Reseller page / photos: <https://www.tindie.com/products/8bitclub/loci-oric-bus-expansion-port-and-floppy-emulator/>
> - Development thread (many in-situ photos):
>   <https://forum.defence-force.org/viewtopic.php?t=2593>

---

## 1. The hardware context

The **Oric-1** and the **Atmos** (1983) are built around a **MOS 6502** at 1 MHz, with 64 KB
of address space whose top (`$C000-$FFFF`) is taken by the **BASIC ROM**.
No hardware multiplication, no memory banks, no modern storage:
everything goes through the **expansion bus** at the back of the machine.

The **LOCI** board (*Lovely Oric Computer Interface*) by **sodiumlb** plugs into
this bus. Technically, it is a derivative of the *Picocomputer 6502* (RP6502): an
**RP2040** microcontroller (a Raspberry Pi Pico) plays the role of a **MIA** (*Media
Interface Adapter*) that presents itself to the Oric as an input/output peripheral. It
emulates floppy and cassette drives, manages an SD card and acts as a **USB
host** — this USB port is where a **Wi-Fi modem** (a separate dongle, the
*PicoWiFiModemUSB*) or an HID device (mouse, gamepad) is plugged in; Wi-Fi
is *not* built into the LOCI board itself. Two address ranges
matter here:

```
         Oric address space (64 KB)
   $0000 ┌──────────────────────────────┐
         │ RAM                          │
   $0300 ├──────────────────────────────┤
         │ VIA 6522        $0300-$030F  │
   $0310 │ Microdisc WD1793 $0310-$031F │
         ├──────────────────────────────┤
   $0380 │ ACIA 6551 (LOCI) $0380-$0383 │ ← serial console / modem
         ├──────────────────────────────┤
   $03A0 │ MIA (LOCI)      $03A0-$03BF  │ ← 32-byte API window
         ├──────────────────────────────┤
   $C000 │ BASIC ROM       $C000-$FFFF  │ ← target of the bank overlay
   $FFFF └──────────────────────────────┘
```

The **Phosphoric** emulator (bus-cycle-accurate, C11) faithfully reproduces this board, which
allows an unusual approach: **write the spec of an extension, code it, and
validate it with deterministic tests — before touching the soldering iron**. A detail
that matters: the author owns a LOCI board, but no Oric. The software bench
is not a luxury, it is the only test lab.

## 2. The *fastcall* ABI — how the 6502 calls the board

Everything relies on a **window of 32 registers** at `$03A0-$03BF` (the MIA). Here is the
actual map of the registers used by the ABI (names as in the firmware):

```
 Offset  Address  Register          Role
 ------  -------  ----------------  ------------------------------------------
  $00    $03A0    CONS_FLAGS        bit7 = TX free, bit6 = RX ready
  $01    $03A1    CONS_TX           console write (UART)
  $02    $03A2    CONS_CHAR         console read (consumes the byte)
  $0C    $03AC    API_STACK         xstack pointer (argument stack)
  $0D    $03AD    API_ERRNO_LO      errno low
  $0E    $03AE    API_ERRNO_HI      errno high
  $0F    $03AF    API_OP            ← WRITING HERE triggers the operation
  $12    $03B2    BUSY              bit7 = board busy
  $14    $03B4    API_A             return value A
  $16    $03B6    API_X             return value X
  $18    $03B8    API_SREG          16-bit return (SREG)
```

The calling protocol (*fastcall*) comes down to four steps:

```
   6502 (Oric)                         MIA (LOCI, µC)
   ───────────                         ──────────────
   1. push args ──► xstack ($03AC)
   2. set A/X (direct parameters)
   3. write op ──► API_OP ($03AF) ───► triggers the handler
                                       ├─ BUSY=1
      poll BUSY ($03B2) ◄──────────────┤  executes
                                       └─ BUSY=0, fills API_A/X/SREG, ERRNO
   4. read API_A/API_X ($03B4/$03B6) ◄─ result
```

Each opcode value still free is an **entry point** for a new
function. The standard operations range from `$01` to `$98` (clock, `open`/`read`/
`lseek`, directories, image mounting, TAP…). Our five extensions live in the
**unused** opcodes:

```
  $A7  SET_BANK        16 KB switchable bank        (--loci-bank)
  $A8  STREAM_BANK     asset streamer               (--loci-bank)
  $A9  MATH            arithmetic coprocessor       (--loci-coproc)
  $AA  ACIA_RELIABLE   reliable ACIA mode (seqlock) (opt-in mode)
       + acia_stat_checked : lossless RX handshake  (--loci-acia-rx-nag)
```

> **Safeguard by design.** Without the corresponding enabling flag, the opcode
> returns `ENOSYS` (errno 13) — exactly like an unpatched firmware. Oric software
> can therefore **detect** whether the extension is present and fall back to its own
> routines. The LOCI's default behaviour remains **strictly** that of the
> original hardware.

## 3. Arithmetic coprocessor — `$A9`

**The problem.** The 6502 has no hardware multiplication, division or floating point.
Every operation is a software routine: slow, bulky, expensive in
cycles. On a 1 MHz machine, a 16×16 multiplication costs hundreds of
cycles.

**The idea.** Delegate the computation to the board's much faster microcontroller, through
the existing *fastcall* ABI — a single opcode `$A9`, the operation sub-code in
`API_A`, the operands on the xstack, the result in `API_A`/`SREG`.

```
   ; conceptual example: A×B through the coprocessor
   LDA #<op_mul  : STA API_A     ; operation sub-code
   ... push A, B onto the xstack ($03AC)
   LDA #$A9      : STA API_OF     ; triggers MATH
   ; poll BUSY, then read the 32-bit result in SREG
```

**The implementation.** An **isolated** file, `src/io/loci_math.c` (`op_math`), plugged
into the dispatch. Integers, floats, vector operations. Gated by
`--loci-coproc`. Coverage: **23 deterministic tests** (integer vectors, floats,
edge cases). Zero randomness — same inputs, same outputs, the prerequisite for a
reproducible bench.

## 4. Reliable ACIA mode — `$AA` (seqlock + ACK)

**The problem.** The real **ACIA 6551** has a known flaw: if the 6502 does not read the
data register in time, the received byte is **overwritten** by the next one. At high
speed — a Wi-Fi modem, for instance — bytes are lost, and the link becomes
unusable. It is faithful to the silicon, but crippling.

**The solution: a seqlock.** A receive sequence counter and an acknowledgement
from the 6502. The byte is only **consumed** once it is **acknowledged** — never lost, even
if the read is late or missed.

```
     Classic 6551 reception (destructive)
     ─────────────────────────────────────
     RX byte1 ──► RDR   (the 6502 has not read it…)
     RX byte2 ──► RDR   ✗ byte1 OVERWRITTEN, lost

     Reliable mode $AA (seqlock + ACK)
     ───────────────────────────────
        RXSEQ $0384  (counter, incremented for each byte presented)
        RXACK $0385  (acknowledgement written by the 6502)

     RX byte1 ─► presented, RXSEQ++          consumed := (RXACK == RXSEQ)
     6502 reads byte1, writes RXACK = RXSEQ ─► byte1 acknowledged → advances
     RX byte2 ─► presented only once acknowledged  ✓ no byte lost
```

The **transmit** channel is unchanged (always reliable). State carried by `loci_t`
(`acia_reliable`, `acia_rx_seq`, `acia_rx_presented`), opcode `$AA`, enabled via
`API_A` bit0. Coverage: **+7 tests** (`test-loci-acia-miss`, 13 → 20) — non-destructive
DATA, *ACK-gated* consumption, multi-byte seqlock **in order**, and
above all **surviving a missed read**.

### 4a. *Lossless* RX handshake — `acia_stat_checked` (`--loci-acia-rx-nag`)

An adjacent refinement, modelled on the real firmware (`feature/acia-rx-lossless`). On
the real LOCI, the ACIA's `/IRQ` is a **level** signal, not a pulse. We
model that level with a "nag": as long as the byte has not been read (`stat_checked`
false), the interrupt is **re-asserted** periodically (default: every 1000
cycles), then **goes quiet** as soon as the 6502 has read the status register.

```
   RDRF=1 (byte available) ──┐
                          │  nag: deassert+assert /IRQ every 1000 cycles
   /IRQ  ▁▔▁▔▁▔▁▔▁▔▁▔▁▔▁▔ │  while  RDRF && !stat_checked && RX-IRQ enabled
                          │
   6502 reads STATUS ─────┘  stat_checked = true  ──►  /IRQ silent
```

Without `--loci-acia-rx-nag`, the ACIA remains **strictly** a 6551 (no nag).
Coverage in `test-loci-acia-miss`: nag **observed before** acknowledgement, **silence
after**, empty buffer ⇒ no IRQ.

## 5. 16 KB switchable bank — `$A7` (`--loci-bank`)

**The problem.** How do you give more memory to a machine whose address space is
saturated by the ROM?

**The solution.** Temporarily overlay 16 KB of the board's RAM (*xram*) onto
the `$C000-$FFFF` window, where the ROM sits.

```
        $A7 disabled                  $A7 EN | SEL=n
   $C000 ┌───────────┐          $C000 ┌───────────────┐
         │ BASIC ROM │   ─────►        │ xram[n*0x4000]│  overlay (read)
   $FFFF └───────────┘          $FFFF  └───────────────┘
                                       └─ ROM intact UNDERNEATH (not overwritten)
   xram base = SEL * 0x4000 ; SEL clamped to 0..3 (like mia_set_bank)
```

**The key point: a non-destructive overlay.** The bank takes priority for reads (and
for memory inspection) **without ever overwriting** the ROM array. Disabling it
restores the machine **byte for byte**. The earlier prototype approach using
`memcpy` + backup was dropped in favour of this clean overlay
(`memory_set_loci_bank()` in `memory.c`).

**Two modes via `reset`.** Enabling through `$A7 EN` triggers a **reset**: the
6502 re-reads its `$FFFC` vector **from the bank** (so you can *boot* code from the
bank). The hot swap (used by the `$A8` streamer) on the contrary switches **without
a CPU reset** — an absolute prerequisite for double buffering. Coverage: `test-loci`
**170/170** (+4: enable/state, disable, gated OFF → `ENOSYS`, SEL clamp 15→3) and an
end-to-end `test-loci-bank-e2e`.

## 6. Asset streamer — `$A8`

**The idea.** Once the bank is in place, **pour data into it from a
file** (flash or SD card) in **a single fastcall**: `lseek(SEEK_SET)` + `read`
→ 16 KB bank, with optional mapping at `$C000-$FFFF`. Oric software can then
**go beyond the 48 usable KB**: overlays, backgrounds, levels on demand.

```
   Double buffering (beyond 48 KB without a reset)
   ─────────────────────────────────────────────
   $A8 MAP=0 SEL=1 ─► preloads bank 1 (invisible)          ┐ in the
   (the 6502 keeps executing/displaying bank 0)            ┘ meantime
   $A8 MAP=1 SEL=1 ─► switches bank 1 in at $C000 (hot swap, PC intact)
```

Details: `API_A` bit7 = `MAP`, bits3:0 = `SEL` (**0..3 valid; >3 = `EINVAL`,
no clamping**, unlike `$A7`). Arguments on the LIFO xstack (`len, dst, off,
fd`), writes bounded to the bank size, return `AX` = bytes read. Two read
paths: host file **and** SD image. Reuses the `--loci-bank` opt-in.

## 7. The *tearing* model — the two-core question

The subtlest extension, and the most intellectually honest.

**The question.** When a bank is switched **while** a bus cycle is in
progress, what does the 6502 latch? In the single-threaded emulator, the swap is **atomic**:
invisible, the question does not arise. But the real hardware has **two cores**; a
swap concurrent with a memory access can produce *tearing*.

**The answer: explicitly model the worst case.** With `--loci-bank-tearing`,
an `$A8 MAP` hot swap concurrent with a **lost PHI2 bus race** makes the 6502 latch
**open-bus** on the **first read** of the window (one-shot
behaviour), after which the bank takes over.

```
   PHI2 bus cycle  ─┬─ race won  ─► bank served cleanly (atomic)
                    │
                    └─ race LOST ─► 1st read = OPEN-BUS (latched value)
                                        then  ─► bank   (one-shot consumed)
   (reuses loci_mia_serve_lost_sampled + seeded jitter, deterministic)
```

The associated test is **self-diagnosing**: it first checks the **precondition**
(the race is indeed lost), then that the model **arms** the *tearing* flag,
then that the one-shot is **consumed**. An incomplete build now fails on a
clear assertion rather than a cryptic `torn != pat[0]`. Deterministic (jitter 0):
three clean builds, identical results. `test-loci-bank-e2e` **8/8**.

## 8. What the exercise teaches

Five extensions, five **minimal** additions to an existing ABI, **zero
regressions** in the default behaviour. Each one is reversible (behind a flag),
gated with `ENOSYS` without its opt-in, and backed by **deterministic** tests.

Perhaps the real lesson is this: on a 1983 machine, the difficulty
is not imagining modern features, but **grafting them on without betraying** the
original behaviour — and **proving** it before getting the soldering iron out.

The bus-cycle-accurate emulator stops being a mere playable museum: it becomes a **hardware
prototyping bench**. You write the spec there, code the extension there, run the
tests there… and the silicon only comes last, with the acceptance checklist already filled in.

---

*The five extensions live on Phosphoric's `experiment/loci-coproc-acia-reliable`
branch — opt-in, reversible, outside the stable release. Test them, criticise them,
improve them.*

### Sources & links

- LOCI firmware / hardware (sodiumlb): <https://github.com/sodiumlb/loci-firmware>,
  <https://github.com/sodiumlb/loci-hardware>, <https://github.com/sodiumlb/loci-rom>
- User manual (FR): <https://github.com/sodiumlb/loci-hardware/wiki/LOCI-Mode-d'emploi>
  and <https://ceo.oric.org/loci-mode-demploi/>
- Reseller & photos: <https://www.raxiss.com/article/id/38-LOCI>,
  <https://www.tindie.com/products/8bitclub/loci-oric-bus-expansion-port-and-floppy-emulator/>
- Development thread: <https://forum.defence-force.org/viewtopic.php?t=2593>
- Phosphoric emulator: <https://github.com/benedictemarty/Phosphoric>
