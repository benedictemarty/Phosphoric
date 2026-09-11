#!/usr/bin/env bash
#
# test_serial_file_backend.sh — deterministic file: replay/capture transport
#
# The file: backend is a transparent byte pipe: bytes read from an input file
# are delivered to the Oric as RX; bytes the Oric transmits are appended to an
# output file. Being transparent it is shared by --serial and --dtl2000.
#
# Three checks, all hermetic (no network, no peer):
#   1. Capture     — dtl2000-test.bas transmits 'A'..'J'; assert the capture
#                    file holds exactly "ABCDEFGHIJ".
#   2. Round-trip  — dtl2000-echo.bas reads 5 replayed RX bytes and echoes them
#                    back on TX; assert input file == output file (proves BOTH
#                    replay and capture in one run).
#   3. Plumbing    — the same file: spec is accepted behind --serial too.
#
# Usage:  tests/integration/test_serial_file_backend.sh
# Exit:   0 = pass, 1 = fail
#
# Author: bmarty <bmarty@mailo.com>

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EMU="./oric1-emu"
ROM="roms/basic11b.rom"
BAS2TAP="./bas2tap"
TMP="$(mktemp -d /tmp/serial_file.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok() { printf '  [OK]   %s\n' "$*"; pass=$((pass+1)); }
ko() { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }

echo "=== serial file: backend (replay/capture) test ==="

# --- preconditions (skip gracefully) ---------------------------------------
[ -x "$EMU" ]     || { echo "  [SKIP] $EMU not built (make SDL2=1)"; exit 0; }
[ -f "$ROM" ]     || { echo "  [SKIP] $ROM absent"; exit 0; }
[ -x "$BAS2TAP" ] || { echo "  [SKIP] $BAS2TAP not built (make tools)"; exit 0; }
[ -x ./bin2tap ]  || { echo "  [SKIP] ./bin2tap not built (make tools)"; exit 0; }

# --- 1) capture: DTL transmits A..J -> capture file ------------------------
"$BAS2TAP" examples/dtl2000-test.bas -o "$TMP/test.tap" --auto-run >/dev/null 2>&1
cap="$TMP/cap.bin"
: > "$cap"
"$EMU" -r "$ROM" --dtl2000 "file::$cap" -t "$TMP/test.tap" -f \
    -n -c 18000000 >/dev/null 2>&1
got="$(tr -d '\0' < "$cap")"
[ "$got" = "ABCDEFGHIJ" ] \
    && ok "capture file holds the 10 transmitted bytes (ABCDEFGHIJ)" \
    || ko "capture mismatch: got '$got' (expected ABCDEFGHIJ)"

# --- 2) round-trip: replay RX -> echo -> capture TX ------------------------
#
# L'écho est en ASSEMBLEUR, pas en BASIC, et c'est une contrainte matérielle, pas
# un confort : sur 6502 NMOS, un POKE passe par un mode indexé qui fait une
# LECTURE FACTICE de l'adresse avant d'écrire. Sur le registre de données d'un
# ACIA, cette lecture **consomme** l'octet reçu — le programme BASIC
# examples/dtl2000-echo.bas perd donc des octets, sur émulateur comme sur machine
# réelle (il garde une valeur pédagogique, avec son avertissement). Un pilote
# série s'écrit avec des STA/STY ABSOLUS, qui n'ont pas de cycle factice.
#
#   0500  A9 00 8D F9 03   LDA #$00 / STA $03F9   ; CRA: sélection DDRA
#   0505  A9 F4 8D F8 03   LDA #$F4 / STA $03F8   ; DDRA = $F4
#   050A  A9 04 8D F9 03   LDA #$04 / STA $03F9   ; CRA: sélection OR
#   050F  A9 C0 8D F8 03   LDA #$C0 / STA $03F8   ; OR  = $C0 (ligne fermée)
#   0514  A9 03 8D FC 03   LDA #$03 / STA $03FC   ; ACIA master reset
#   0519  A9 15 8D FC 03   LDA #$15 / STA $03FC   ; 8N1, div16, RTS bas
#   051E  A2 05            LDX #$05               ; 5 octets
#   0520  AD FC 03 29 01 F0 F9   attendre RDRF
#   0527  AD FD 03 A8            LDA $03FD / TAY  ; octet reçu
#   052B  AD FC 03 29 02 F0 F9   attendre TDRE
#   0532  8C FD 03               STY $03FD        ; émettre, SANS cycle factice
#   0535  CA D0 E8 60            DEX / BNE / RTS
printf '\251\000\215\371\003\251\364\215\370\003\251\004\215\371\003\251\300\215\370\003\251\003\215\374\003\251\025\215\374\003\242\005\255\374\003\051\001\360\371\255\375\003\250\255\374\003\051\002\360\371\214\375\003\312\320\350\140' \
    > "$TMP/echo.bin"
./bin2tap "$TMP/echo.bin" --start 0x0500 --exec 0x0500 -o "$TMP/echo.tap" \
    --name DTLECHO >/dev/null 2>&1
inp="$TMP/in.bin"; out="$TMP/out.bin"
printf 'ORIC!' > "$inp"; : > "$out"
"$EMU" -r "$ROM" --dtl2000 "file:$inp:$out" -t "$TMP/echo.tap" -f \
    -n -c 12000000 >/dev/null 2>&1
if cmp -s "$inp" "$out"; then
    ok "round-trip replay->echo->capture: output == input (ORIC!)"
else
    ko "round-trip mismatch: $(xxd -p "$out" 2>/dev/null) != $(xxd -p "$inp")"
fi

# --- 3) plumbing: file: is accepted behind --serial as well ----------------
plog="$TMP/serial.log"
"$EMU" -r "$ROM" --serial "file::$TMP/s.bin" -n -c 200000 >"$plog" 2>&1
grep -q "Serial FILE:" "$plog" && grep -q "Serial interface enabled" "$plog" \
    && ok "--serial accepts the file: transport (shared helper)" \
    || ko "--serial did not accept the file: transport"

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
