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
       | "$EMU" -r "$ROM" -n --control 2>/dev/null | grep -E 'A=[0-9A-F].*cycles=' | tail -1)"
ref_cycles="$(echo "$ref" | grep -oE 'cycles=[0-9]+' | cut -d= -f2)"
ref_pc="$(echo "$ref" | grep -oE 'PC=[0-9A-F]+' | cut -d= -f2)"

if [ -z "${ref_cycles:-}" ] || [ "$ref_cycles" = "0" ]; then
    echo "  [SKIP] could not produce a non-zero reference state (cycles='$ref_cycles')"
    exit 0
fi
ok "produced reference state PC=$ref_pc cycles=$ref_cycles"

# 2. Relaunch with --load-state and read regs BEFORE any step.
got="$(printf 'regs\nquit\n' \
       | "$EMU" -r "$ROM" --load-state "$OST" -n --control 2>/dev/null | grep -E 'A=[0-9A-F].*cycles=' | head -1)"
got_cycles="$(echo "$got" | grep -oE 'cycles=[0-9]+' | cut -d= -f2)"
got_pc="$(echo "$got" | grep -oE 'PC=[0-9A-F]+' | cut -d= -f2)"

[ "$got_cycles" = "$ref_cycles" ] \
    && ok "restored cycles match ($got_cycles)" \
    || ko "cycles not restored: expected $ref_cycles, got '${got_cycles:-<none>}' (reset bug?)"
[ "$got_pc" = "$ref_pc" ] \
    && ok "restored PC matches ($got_pc)" \
    || ko "PC not restored: expected $ref_pc, got '${got_pc:-<none>}'"

# 3. Picture after a load (2.31.3): savestate_load pointed video.charset at a
#    test-only buffer, so every frame after a load was blank (white) while RAM
#    and CPU were fine. Boot to Ready, save at exit with a screenshot, reload,
#    screenshot again: both pictures must match (cursor blink aside).
OST2="$(mktemp --suffix=.ost)"; P1="$(mktemp --suffix=.ppm)"; P2="$(mktemp --suffix=.ppm)"
"$EMU" -n --no-config -r "$ROM" -c 3000000 --screenshot-at 2990000:"$P1" --save-state "$OST2" >/dev/null 2>&1
"$EMU" -n --no-config -r "$ROM" --load-state "$OST2" -c 100000 --screenshot-at 60000:"$P2" >/dev/null 2>&1
diff_pct="$(python3 -I - "$P1" "$P2" <<'PY'
import sys
def ppm(p):
    d = open(p, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:                      # P6, width, height, maxval
        while d[i:i+1].isspace(): i += 1
        j = i
        while not d[j:j+1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    return d[i+1:]
a, b = ppm(sys.argv[1]), ppm(sys.argv[2])
n = min(len(a), len(b)) // 3
px = lambda d, k: d[3*k:3*k+3]
diff = sum(1 for k in range(n) if px(a, k) != px(b, k))
# "Ink" pixels of the saved screen: those not of the majority colour (the
# paper). The difference is measured against them: a white screen after the
# load wipes all the text, a difference of about 30 % of the ink or more,
# whereas the cursor blink alone stays under 5 %.
from collections import Counter
paper = Counter(px(a, k) for k in range(n)).most_common(1)[0][0]
ink = sum(1 for k in range(n) if px(a, k) != paper)
print(f"{100.0 * diff / max(ink, 1):.2f}")
PY
)"
rm -f "$OST2" "$P1" "$P2"
if [ -n "$diff_pct" ] && python3 -I -c "import sys; sys.exit(0 if float('$diff_pct') < 10.0 else 1)"; then
    ok "picture after --load-state matches the saved screen (difference = ${diff_pct}% of the ink pixels)"
else
    ko "picture after --load-state differs from the saved screen (difference = ${diff_pct:-?}% of the ink pixels; blank frame bug?)"
fi

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
