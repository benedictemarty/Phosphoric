#!/usr/bin/env bash
# tests/integration/test_cli_parsing.sh
#
# Characterization tests for the CLI argument parser (main.c + cli_options.h).
# These lock the CURRENT, MEASURED behaviour of option parsing so a future
# refactor (Epic 7 / US3 : declarative parser) can be proven iso-behaviour.
#
# NB : every expectation below was OBSERVED against the built binary, not
# assumed. Some are deliberately non-obvious and are captured as-is (they
# document current behaviour, not necessarily the ideal one) :
#   - an unknown option prints usage and exits 0 (non-fatal)
#   - --joystick with a bogus mode logs an error but exits 0 (non-fatal)
#   - --serial with a bogus backend exits 1 (fatal)
# A future parser must reproduce these exactly, or consciously change them.

set -u
cd "$(dirname "$0")/../.." || exit 1

EMU=./oric1-emu
ROM=roms/basic11b.rom
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
note_pass() { echo "  PASS: $1"; pass=$((pass + 1)); }
note_fail() { echo "  FAIL: $1"; fail=$((fail + 1)); }

if [ ! -x "$EMU" ]; then
    echo "  oric1-emu not built — skipping (run: make)"
    exit 0
fi
if [ ! -f "$ROM" ]; then
    echo "  ROM missing ($ROM) — skipping"
    exit 0
fi

# Run the emulator, capture combined output + exit code into globals OUT / RC.
run() { OUT=$("$@" 2>&1); RC=$?; }

# Assert an exit code and (optionally) an output substring.
expect() {  # <desc> <expected_rc> [substr]
    local desc="$1" want_rc="$2" substr="${3:-}"
    if [ "$RC" -ne "$want_rc" ]; then
        note_fail "$desc (rc=$RC, expected $want_rc)"
        echo "$OUT" | tail -2 | sed 's/^/        /'
        return
    fi
    if [ -n "$substr" ] && ! echo "$OUT" | grep -qF "$substr"; then
        note_fail "$desc (rc ok but missing: '$substr')"
        echo "$OUT" | tail -2 | sed 's/^/        /'
        return
    fi
    note_pass "$desc"
}

echo "CLI parser characterization tests:"

# ── --help : usage, exit 0 ─────────────────────────────────────────────
run "$EMU" --help
expect "--help prints usage, exit 0" 0 "Usage:"

# ── unknown option : NON-fatal, prints usage, exit 0 (measured) ────────
run "$EMU" -r "$ROM" -n --bogus-xyz -c 1000
expect "unknown option is non-fatal (usage, exit 0)" 0 "Usage:"

# ── malformed CYCLES:VALUE options : fatal, specific message ───────────
run "$EMU" -r "$ROM" -n --dump-ram-at NOCOLON -c 1000
expect "--dump-ram-at without ':' → exit 1" 1 "Invalid --dump-ram-at format"

run "$EMU" -r "$ROM" -n --screenshot-at NOCOLON -c 1000
expect "--screenshot-at without ':' → exit 1" 1 "Invalid --screenshot-at format"

run "$EMU" -r "$ROM" -n --screenshot-text-at NOCOLON -c 1000
expect "--screenshot-text-at without ':' → exit 1" 1 "Invalid --screenshot-text-at format"

run "$EMU" -r "$ROM" -n --screenshot-ansi-at NOCOLON -c 1000
expect "--screenshot-ansi-at without ':' → exit 1" 1 "Invalid --screenshot-ansi-at format"

run "$EMU" -r "$ROM" -n --type-keys NOCOLON -c 1000
expect "--type-keys without ':' → exit 1" 1 "Invalid --type-keys format"

# ── enum-valued options : bogus values ─────────────────────────────────
run "$EMU" -r "$ROM" -n --joystick BOGUS -c 1000
expect "--joystick bogus is non-fatal (exit 0)" 0 "Unknown joystick mode"

run "$EMU" -r "$ROM" -n --serial BOGUS -c 1000
expect "--serial bogus is fatal (exit 1)" 1 "Unknown serial backend"

# ── enum / range / value validation (all measured) ────────────────────
run "$EMU" -r "$ROM" -n -m BOGUS -c 1000
expect "--model bogus is fatal (exit 1)" 1 "Unknown model"

run "$EMU" -r "$ROM" -n -m atmos -c 1000
expect "--model atmos parses (exit 0)" 0

run "$EMU" -r "$ROM" -n --scale 99 -c 1000
expect "--scale out of range (99) is fatal (exit 1)" 1 "must be 1-4"

run "$EMU" -r "$ROM" -n --dtl2000 BOGUS -c 1000
expect "--dtl2000 bogus transport is fatal (exit 1)" 1 "Unknown DTL 2000 transport"

# Non-fatal-by-design cases (captured as-is : a bogus value is IGNORED, not
# rejected — a future parser must keep this or change it consciously).
run "$EMU" -r "$ROM" -n -k BOGUS -c 1000
expect "--keyboard bogus is non-fatal (exit 0)" 0

run "$EMU" -r "$ROM" -n --acia-addr ZZZZ -c 1000
expect "--acia-addr invalid hex is non-fatal (exit 0)" 0

run "$EMU" -r "$ROM" -n --load-state /nonexistent.ost -c 1000
expect "--load-state missing file is non-fatal (exit 0)" 0

# ── positive control : a well-formed CYCLES:FILE parses and acts ───────
run "$EMU" -r "$ROM" -n --dump-ram-at 500000:"$TMP/ram.bin" -c 600000
if [ "$RC" -eq 0 ] && [ -s "$TMP/ram.bin" ] \
        && [ "$(stat -c%s "$TMP/ram.bin")" -eq 65536 ]; then
    note_pass "well-formed --dump-ram-at 500000:FILE parses and writes 64K"
else
    note_fail "well-formed --dump-ram-at (rc=$RC, size=$(stat -c%s "$TMP/ram.bin" 2>/dev/null || echo 0))"
fi

# well-formed --screenshot-text-at : dump du texte $BB80 non vide au cycle donné
run "$EMU" -r "$ROM" -n --screenshot-text-at 3000000:"$TMP/at.txt" -c 3200000
if [ "$RC" -eq 0 ] && [ -s "$TMP/at.txt" ]; then
    note_pass "well-formed --screenshot-text-at 3000000:FILE parses and writes text"
else
    note_fail "well-formed --screenshot-text-at (rc=$RC, size=$(stat -c%s "$TMP/at.txt" 2>/dev/null || echo 0))"
fi

# well-formed --screenshot-ansi-at : image ANSI non vide au cycle donné
run "$EMU" -r "$ROM" -n --screenshot-ansi-at 3000000:"$TMP/at.ansi" -c 3200000
if [ "$RC" -eq 0 ] && [ -s "$TMP/at.ansi" ]; then
    note_pass "well-formed --screenshot-ansi-at 3000000:FILE parses and writes ANSI"
else
    note_fail "well-formed --screenshot-ansi-at (rc=$RC, size=$(stat -c%s "$TMP/at.ansi" 2>/dev/null || echo 0))"
fi

# ── side-effect options (fopen / load at parse time) : the delicate part of
#    US3 — a future parser must reproduce these exactly. All measured. ──────

# Success : the artifact is actually produced.
run "$EMU" -r "$ROM" -n --trace "$TMP/tr.log" -c 200000
if [ "$RC" -eq 0 ] && [ -s "$TMP/tr.log" ]; then
    note_pass "--trace FILE parses and writes the trace file"
else
    note_fail "--trace FILE (rc=$RC, file empty/missing)"
fi

run "$EMU" -r "$ROM" -n --profile "$TMP/pr.txt" -c 200000
if [ "$RC" -eq 0 ] && [ -s "$TMP/pr.txt" ]; then
    note_pass "--profile FILE parses and writes the profile on exit"
else
    note_fail "--profile FILE (rc=$RC, file empty/missing)"
fi

# Missing/invalid resource : note the NON-obvious fatality divergences.
run "$EMU" -r "$ROM" -n --tape "$TMP/nope.tap" -c 1000
expect "--tape missing file is non-fatal (warning, exit 0)" 0 "Failed to open tape"

run "$EMU" -r "$ROM" -n --disk "$TMP/nope.dsk" -c 1000
expect "--disk missing file is FATAL (exit 1)" 1 "Failed to load disk image"

run "$EMU" -r "$ROM" -n --symbols "$TMP/nope.sym" -c 1000
expect "--symbols missing file is FATAL (exit 1)" 1 "cannot open"

run "$EMU" -r "$ROM" -n --breakpoint ZZZZ -c 1000
expect "--breakpoint bad hex is non-fatal (exit 0)" 0

# fopen failure on an unwritable path : logged as error but NON-fatal (exit 0).
run "$EMU" -r "$ROM" -n --trace /proc/nonexistent-dir/x.log -c 1000
expect "--trace unwritable path is non-fatal (error logged, exit 0)" 0 "Cannot open trace file"

# ... whereas these three sibling file options are FATAL on open failure (rc 1),
# with the same 'Cannot open --NAME file' message. This non-obvious divergence
# (--trace non-fatal vs --trace-irq fatal) is exactly what the parser must keep.
run "$EMU" -r "$ROM" -n --trace-irq /proc/nonexistent-dir/x.log -c 1000
expect "--trace-irq unwritable path is FATAL (exit 1)" 1 "Cannot open --trace-irq file"

run "$EMU" -r "$ROM" -n --psg-trace /proc/nonexistent-dir/x.psg -c 1000
expect "--psg-trace unwritable path is FATAL (exit 1)" 1 "Cannot open --psg-trace file"

run "$EMU" -r "$ROM" -n --audio-wav /proc/nonexistent-dir/x.wav -c 1000
expect "--audio-wav unwritable path is FATAL (exit 1)" 1 "Cannot open --audio-wav file"

echo ""
echo "  Results: $pass passed, $fail failed (total: $((pass + fail)))"
[ "$fail" -eq 0 ]
