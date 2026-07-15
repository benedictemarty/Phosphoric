#!/usr/bin/env bash
#
# test_load_state_control.sh — regression test for --load-state under --control.
#
# Bug: emulator_run() ran an unconditional power-on cpu_reset AFTER --load-state,
# wiping the restored PC/cycles back to the reset vector (cycles=0). Most visible
# under --control, where the client inspects registers before any step.
#
# This test: step a few instructions under --control, save the state, then
# relaunch with --load-state and verify `regs` reports the SAME cycles/PC (not 0
# / reset). Skips gracefully when no ROM or binary is available.
#
# Author: bmarty <bmarty@mailo.com>

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EMU="./oric1-emu"
ROM="roms/basic11b.rom"
[ -f "$ROM" ] || ROM="roms/basic10.rom"
OST="$(mktemp --suffix=.ost)"

pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$*"; pass=$((pass+1)); }
ko() { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }
cleanup() { rm -f "$OST"; }
trap cleanup EXIT

echo "=== --load-state under --control regression test ==="
[ -x "$EMU" ] || { echo "  [SKIP] $EMU not built"; exit 0; }
[ -f "$ROM" ] || { echo "  [SKIP] no BASIC ROM"; exit 0; }

# 1. Advance a few instructions, capture the reference regs, save the state.
ref="$(printf 'step\nstep\nstep\nstep\nstep\nregs\nstate-save %s\nquit\n' "$OST" \
       | "$EMU" -r "$ROM" --control 2>/dev/null | grep -E 'A=[0-9A-F].*cycles=' | tail -1)"
ref_cycles="$(echo "$ref" | grep -oE 'cycles=[0-9]+' | cut -d= -f2)"
ref_pc="$(echo "$ref" | grep -oE 'PC=[0-9A-F]+' | cut -d= -f2)"

if [ -z "${ref_cycles:-}" ] || [ "$ref_cycles" = "0" ]; then
    echo "  [SKIP] could not produce a non-zero reference state (cycles='$ref_cycles')"
    exit 0
fi
ok "produced reference state PC=$ref_pc cycles=$ref_cycles"

# 2. Relaunch with --load-state and read regs BEFORE any step.
got="$(printf 'regs\nquit\n' \
       | "$EMU" -r "$ROM" --load-state "$OST" --control 2>/dev/null | grep -E 'A=[0-9A-F].*cycles=' | head -1)"
got_cycles="$(echo "$got" | grep -oE 'cycles=[0-9]+' | cut -d= -f2)"
got_pc="$(echo "$got" | grep -oE 'PC=[0-9A-F]+' | cut -d= -f2)"

[ "$got_cycles" = "$ref_cycles" ] \
    && ok "restored cycles match ($got_cycles)" \
    || ko "cycles not restored: expected $ref_cycles, got '${got_cycles:-<none>}' (reset bug?)"
[ "$got_pc" = "$ref_pc" ] \
    && ok "restored PC matches ($got_pc)" \
    || ko "PC not restored: expected $ref_pc, got '${got_pc:-<none>}'"

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
