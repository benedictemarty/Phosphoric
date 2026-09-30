#!/bin/sh
# cli_golden.sh — differential comparison of two binaries on a corpus of
# command lines (safety net for refactors of main()/the parser).
#
# Usage: tools/cli_golden.sh OLD NEW [CORPUS]
#   CORPUS (default tests/cli_golden/cases.txt): one line of arguments per case,
#   read by the shell (quotes allowed); "@OUT@" = output directory specific to
#   the case and the binary. Blank lines and "#" lines are ignored.
# For each case both binaries run from the repository root; exit code, stdout,
# stderr (timestamps and @OUT@ normalised) and every file produced under @OUT@
# are compared. Exit code 1 if any difference is reported (all cases are still
# played). GOLDEN_KEEP=1 keeps the outputs.
set -u
OLD=${1:?usage: cli_golden.sh ANCIEN NOUVEAU [CORPUS]}
NEW=${2:?usage: cli_golden.sh ANCIEN NOUVEAU [CORPUS]}
CASES=${3:-tests/cli_golden/cases.txt}
T=$(mktemp -d) || exit 1
[ "${GOLDEN_KEEP:-0}" = 1 ] && echo "sorties conservées : $T" || trap 'rm -rf "$T"' EXIT

norm() {  # norm <file> <@OUT@ directory>
    sed -e "s|$2|@OUT@|g" \
        -e 's/\[[0-9]\{4\}-[0-9][0-9]-[0-9][0-9] [0-9][0-9]:[0-9][0-9]:[0-9][0-9]\]/[TS]/g' "$1" |
    sed -E '/^BENCH /s/(wall_ms|mhz_eq|speed_ratio|frame_us)=[0-9.x]*/\1=N/g'
}

# `timeout` (coreutils) is missing on macOS by default: without it, no limit.
TO=""
command -v timeout >/dev/null 2>&1 && TO="timeout ${GOLDEN_TIMEOUT:-60}"

# Both binaries run under the SAME path (symbolic link): argv[0] appears in
# the help and in some messages.
BIN="$T/oric1-emu"
abs() { case "$1" in /*) echo "$1" ;; *) echo "$(pwd)/$1" ;; esac; }
OLD=$(abs "$OLD"); NEW=$(abs "$NEW")

run_one() {  # run_one <binary> <directory> <arguments>
    mkdir -p "$2/out"
    ln -sf "$1" "$BIN"
    args=$(printf '%s' "$3" | sed "s|@OUT@|$2/out|g")
    eval "$TO \"\$BIN\" $args" </dev/null >"$2/stdout" 2>"$2/stderr"
    echo $? > "$2/rc"
}

n=0; bad=0
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in ''|'#'*) continue ;; esac
    n=$((n+1))
    a="$T/$n/a"; b="$T/$n/b"
    run_one "$OLD" "$a" "$line"
    run_one "$NEW" "$b" "$line"
    diffs=""
    cmp -s "$a/rc" "$b/rc" || diffs="$diffs rc($(cat "$a/rc")→$(cat "$b/rc"))"
    for s in stdout stderr; do
        norm "$a/$s" "$a/out" > "$a/$s.n"; norm "$b/$s" "$b/out" > "$b/$s.n"
        cmp -s "$a/$s.n" "$b/$s.n" || diffs="$diffs $s"
    done
    (cd "$a/out" && find . -type f | sort) > "$a/files"
    (cd "$b/out" && find . -type f | sort) > "$b/files"
    if cmp -s "$a/files" "$b/files"; then
        while IFS= read -r f; do
            cmp -s "$a/out/$f" "$b/out/$f" || diffs="$diffs fichier:$f"
        done < "$a/files"
    else
        diffs="$diffs liste-fichiers"
    fi
    if [ -n "$diffs" ]; then
        bad=$((bad+1)); echo "DIFF [$n] $line"; echo "      →$diffs"
    fi
done < "$CASES"
echo "--- $n cas, $bad écart(s)"
[ "$bad" -eq 0 ]
