# Request to the Phosphoric team — capture triggered by a memory STATE

**Date**: 2026-07-18
**Requester**: SCUMM-Oric project (headless test bench)
**Type**: feature request
**Priority**: high for the SCUMM CI (unblocks a blocked refactor)

---

## 1. The need, measured

The SCUMM-Oric test bench uses Phosphoric in headless mode to validate the
rendering and state of the game. **All captures are triggered by a fixed number
of cycles**:

- `--screenshot-at C:FILE`
- `--dump-ram-at C:FILE`
- `--screenshot-text-at C:FILE`

Yet the cycle at which a given game state appears **depends on the size and
layout of the binary under test**. As soon as the code is modified (even a
neutral refactor that moves variables around in memory), the timeline shifts: the
capture at cycle N shows **another frame** → the test fails even though the
behaviour is correct. This is a documented fragility on the SCUMM side ("§S306").

Concrete, current consequence: a clean refactor (unifying the actor state
into a single source) is **blocked**, because moving tables breaks
byte-exact/screenshot tests — not through a change of behaviour, but through a
timing shift. The possible workarounds on the SCUMM side are all bad:
- leave BSS "holes" to freeze addresses (debt that accumulates);
- pick fixed addresses by hand (= guessing the memory map).

## 2. What is missing in Phosphoric

**No capture triggered by a STATE** (a memory condition), only by
time (cycles). There is no equivalent of "capture when `RAM[$XXXX] == V`".

## 3. The request

Add **state-triggered** variants of the existing captures:

```
--screenshot-when   ADDR:VAL:FILE   # capture image when RAM[ADDR] becomes == VAL
--dump-ram-when     ADDR:VAL:FILE   # dump 64K when RAM[ADDR] becomes == VAL
--screenshot-text-when ADDR:VAL:FILE
```

- `ADDR` in hex (e.g. `9C55`), `VAL` a byte (e.g. `07` or `0x07`).
- **Rising edge**: triggers the first time the condition becomes true
  (just as `screenshot_at_done` prevents re-triggering).
- **Safety net**: `--cycles N` remains the upper bound; if the condition
  never becomes true before N cycles, the behaviour is up to the team
  (ideally: non-zero exit + a "condition never reached" message, so that the
  test fails loudly rather than silently).
- Useful option later: a 16-bit value (`ADDR:VAL16`) and comparators
  (`>=`), but `== byte` covers the essentials.

## 4. Why this is the right remedy (and not a workaround)

The capture becomes deterministic on the **game state** ("when room 7 is
loaded", `RAM[current_room] == 7`) and **independent of timing/layout**. The
SCUMM refactor can then move whatever it wants in memory without breaking a
single test — no more need for BSS holes or guessed addresses. This is aligned with
the SCUMM rule "measure, don't invent": we capture on a **measured
value**, not on an assumed address or cycle.

## 5. Anchor points in the code (for estimation)

The existing `screenshot-at` mechanism is a good template — it is enough to duplicate
it, replacing "cycle reached" with "memory condition true":

- Option declaration: `include/cli/cli_options.h:63`
  (`{"screenshot-at", required_argument, 0, OPT_SCREENSHOT_AT}`).
- Help: `src/main.c:323`.
- Field initialisation: `src/main.c:1463-1464`
  (`emu->screenshot_at_cycles = -1; emu->screenshot_at_file = NULL;`).
- Cycle-based triggering: `src/main.c:2861-2866`
  (`if (!screenshot_at_done && emu->screenshot_at_cycles >= 0 && total_executed >= ...) { emu_export_image(...); }`).

The `-when` variant would add `screenshot_when_addr/val/file` fields and,
in the same loop, a `bus_read(ADDR) == VAL` test (through the memory access already
used by `--dump-ram-at`) instead of the cycle comparison.

## 6. Expected impact on the SCUMM side

- Migration of the ~13 layout-sensitive tests to `-when` (trigger = stable state
  value), single VERIFIED regeneration of their references.
- Removal of the BSS "holes"; actor-state refactor unblocked.
- CI robust to layout changes in the future.

Thanks — available to clarify the test cases or provide a SCUMM reproduction
binary (a simple `load_room #7` then a capture on `current_room == 7`).
