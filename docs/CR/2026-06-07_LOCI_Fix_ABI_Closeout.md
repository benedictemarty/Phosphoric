# Report — LOCI closeout / ABI fix 2026-06-07

**Author**: bmarty
**Branch**: `fix/mia-spin-abi` → merged into `main` (commit `26a3977`)
**Version delivered**: v1.16.39-alpha

---

## 1. Context

Following the report of 2026-06-06 (Sprint 34am, PSG R7=$7F pre-seed), which
left the keyboard non-functional despite the R7 fix, the lead engineer
carried out an in-depth review of the LOCI ROM + Pi Pico firmware source code,
and then identified the **real root cause**: the ABI of the spin window
`$03B0-$03B9` was not materialised.

---

## 2. Root cause (lead engineer's analysis)

### The MIA contract is not a register file

The `$03B0-$03B9` window of the LOCI firmware is **self-modifying 6502
code** that the Pi Pico coprocessor rewrites at every operation
transition. The 6502 ABI is:

```asm
STA  $03AF       ; trigger op
JSR  $03B0       ; CALL the spin window
; on return: A = result_lo, X = result_hi
;            $03B8/B9 hold SREG (high 16 bits for AXSREG)
```

### Blocked stub (op queued, BUSY=1)

```
$03B0  B8           CLV
$03B1  50 FE        BVC -2     (loops back forever onto $03B0)
$03B3  A9 --        LDA #--    (operand A to be patched on release)
```

The byte `$03B2 = $FE` **simultaneously** encodes:
- the BVC operand (= -2, backward branch)
- the BUSY flag (bit 7 = 1)

### Released stub (op done, BUSY=0)

```
$03B0  B8           CLV
$03B1  50 00        BVC +0     (fall-through)
$03B3  A9 <A_lo>    LDA #A
$03B5  A2 <X_hi>    LDX #X
$03B7  60           RTS
$03B8  <SREG_lo>
$03B9  <SREG_hi>
```

The byte `$03B2 = $00` encodes:
- the BVC operand (= +0, hence fall-through)
- the BUSY flag (bit 7 = 0)

### The bug

Phosphoric before this fix:
- Wrote `$03B4` (A), `$03B6` (X), `$03B8`/`$03B9` (SREG)
- **Never wrote** `$03B0`/`$03B1`/`$03B2`/`$03B3`/`$03B5`/`$03B7`

Consequence: at `loci_init`, `regs[0x10..0x1F]` was `$00..$00` after
the `memset`. A 6502 `JSR $03B0` fetched `0x00 0x00 0x00 0x00 ...`
= `BRK BRK BRK ...`. The 6502 vectors through `$FFFE/F` (IRQ vector) and
diverges.

The first fastcall of the LOCI boot is `tap_tell()` at `main.c:1159` via
`update_tap_counter()` — BEFORE `InitKeyboard()` (l.1186) and the `while(1)`
(l.1188). Therefore:
1. `tap_tell` never returned
2. `main()` stuck
3. `InitKeyboard` never called → PSG R7 never programmed
4. `while(1)` never reached → no spinner
5. The keyboard seems "not to work" even though it scans correctly

The R7 pre-seed of Sprint 34am masked the R7 symptom but left the
ABI broken. Since the released stub did not exist, `main()` stayed stuck.

### Why the 105 existing tests passed

My tests called `loci_write(..., 0x03AF, op)` and then checked the
registers `regs[0x14]` (A), `regs[0x16]` (X). They **short-circuited
the `JSR $03B0`** by reading the post-dispatch registers directly. The
real 6502, on the other hand, had to go through the spin window — which did not exist.

---

## 3. Patch delivered (Option B)

### New helpers

```c
static void api_install_blocked_stub(loci_t* loci) {
    loci->regs[0x10] = 0xB8;  // CLV
    loci->regs[0x11] = 0x50;  // BVC
    loci->regs[0x12] = 0xFE;  // -2 + BUSY=1
    loci->regs[0x13] = 0xA9;  // LDA #
}

static void api_install_released_stub(loci_t* loci) {
    loci->regs[0x10] = 0xB8;
    loci->regs[0x11] = 0x50;
    loci->regs[0x12] = 0x00;  // +0 + BUSY=0
    loci->regs[0x13] = 0xA9;
}
```

### api_set_ax extended

Byte-for-byte mirror of the firmware's `api.h:183-186`:

```c
static void api_set_ax(loci_t* loci, uint16_t val) {
    loci->regs[0x14] = val & 0xFF;          // A immediate
    loci->regs[0x15] = 0xA2;                 // LDX #
    loci->regs[0x16] = (val >> 8) & 0xFF;    // X immediate
    loci->regs[0x17] = 0x60;                 // RTS
}
```

### $03AF dispatch refactored

```c
if (off == LOCI_REG_API_OP) {
    if (value == 0x00) {
        xstack_zero(loci);
        api_return_ax(loci, 0);    // → released stub
        return;
    }
    if (value == 0xFF) {
        api_install_blocked_stub(loci);  // 6502 spins forever
        return;
    }
    api_install_blocked_stub(loci);
    dispatch_op(loci, value);       // handler → api_return_*
}
```

### Init/Reset

`loci_init` and `loci_reset` now call `seed_initial_stub()`,
which installs a released no-op stub (A=X=SREG=0) — protecting against
pre-op probes.

### Default ENOSYS

The old fallback in `dispatch_op` did `set_errno + set_busy(false)`
— but did not install the released stub. Replaced by `api_return_errno`,
which goes through the full chain.

---

## 4. 6502 differential tests

3 new tests that run a **real `cpu_step()`** on a minimal 6502 program
and exercise the complete ABI:

### `test_6502_initial_jsr_returns_zero`
Before any op, checks that `JSR $03B0` returns cleanly with A=X=0 thanks to the
initial seed.

### `test_6502_jsr_spin_zxstack_op_00`
```asm
LDA #0
STA $03AF       ; trigger zxstack
JSR $03B0       ; spin → released stub
STA $0200       ; record A
STX $0201       ; record X
BRK             ; halt
```
Checks: xstack_ptr goes back to 256, A=0, X=0.

### `test_6502_jsr_spin_returns_via_released_stub`
Op `RNG_LRAND` (0x04). Checks: positive 31-bit return via AXSREG,
BUSY clear, PC at the BRK sentinel ($041A).

### Makefile plumbing

`TEST_LOCI_SRCS` now links cpu6502 + memory + addressing + banking
to allow real 6502 execution.

---

## 5. User validation

Interactive sessions on 2026-06-07 confirmed:

| Test | Before | After |
|------|-------|-------|
| LOCI ROM v0.3.0 boot | reaches $C354 (IRQ) | reaches while(1) |
| TUI spinner visible | ❌ | ✅ |
| Letters a/b/c/d/t/k/m/o | ❌ | ✅ |
| Arrows ↑↓←→ | ❌ | ✅ |
| Widget toggles | ❌ | ✅ |
| ESC → fresh boot | ❌ | ✅ |
| MIA_BOOT op 0xA0 | ❌ | ✅ |
| ROM swap to BASIC 1.0/1.1 | ❌ | ✅ |
| Complete E2E scenario | ❌ | ✅ |

**Phosphoric is officially the first mainstream emulator to
support LOCI end to end**: the LOCI v0.3.0 ROM boots, displays
its TUI, accepts keyboard navigation, runs MIA_BOOT, and swaps
to the selected BASIC ROM (1.0 / 1.1), loading it from the
`--loci-flash DIR` sandbox.

---

## 6. LOCI UI semantics (quirk discovered during testing)

Non-intuitive convention of the LOCI ROM (loci-rom/src/main.c:901-938):

| Key | Action |
|--------|--------|
| **ESC** | Fresh boot according to the current config (`boot(false)`) |
| **Return** on the `BOOT` button | Fresh boot likewise (`boot(false)`) |
| **Return** on the general menu | Resume save state (`boot(true)`) — no-op without a save state |

This is not a Phosphoric bug; it is the design chosen by sodiumlb.

---

## 7. Final metrics

| Indicator | Value |
|------------|--------|
| test-loci tests | 108 (105 unit + 3 6502 differential) |
| Phosphoric global tests | 448 |
| Regressions in other modules | 0 |
| LOCI API ops implemented | 28/36 (78%) |
| Critical bug resolved | MIA spin window ABI |
| User E2E validation | ✅ Confirmed |

---

## 8. Credits

- **bmarty** (Phosphoric): implementation of sprints 34y-34am + patch integration
- **Lead engineer**: ABI root-cause analysis, Option B suggestion,
  firmware contract deciphered
- **sodiumlb**: LOCI ROM v0.3.0 + open-source firmware (BSD-3-Clause)
- **rumbledethumps**: ancestral RP6502 architecture that LOCI inherits from

---

## 9. Reproducibility

```bash
git clone <repo> && cd Oric1
git checkout main
make clean && make SDL2=1
make tests   # 448 pass

# Prepare the sandbox
mkdir -p ~/loci-vfs
cp roms/basic1*.rom roms/microdis.rom ~/loci-vfs/

# Launch the full LOCI ROM
./oric1-emu -r roms/loci/locirom --loci --loci-flash ~/loci-vfs

# In the TUI:
#   ↑↓←→ : navigation
#   b    : toggle BASIC 1.0/1.1
#   t    : toggle TAP
#   f    : toggle FDC
#   ESC  : fresh boot according to config
# → switches into BASIC 1.0/1.1 Atmos!
```

---

**Status**: LOCI E2E scenario **validated**. The series of 14 sprints
(34y → 34an) is officially complete. Remaining optional polish:
WD1793 timing exact to the cycle, TAP bit-streamer $0317, ROM diagnostics via `--loci-diag`.

— End of report
