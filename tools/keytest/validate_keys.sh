#!/usr/bin/env bash
# =====================================================================
# validate_keys.sh — automated validation of keyboard detection
# (ROM KEY$) in ORIC-1 (BASIC 1.0) and Atmos (BASIC 1.1) modes.
#
# Principle: keytest.bas runs in the emulator and POKEs the ASCII code
# of each detected key (KEY$) into a buffer at #2000. We inject
# a sequence of keys/combos via --type-keys, dump the RAM, and
# compare the buffer against the expected vector, byte by byte.
#
# CTRL+C is EXCLUDED from the BASIC set: it is the BREAK key, which interrupts
# the program (validated separately = 0x03 via the LOCI File Manager).
# =====================================================================
set -u
cd "$(dirname "$0")/../.."   # repository root

EMU=./oric1-emu
TAP=tools/keytest/keytest.tap
DUMP=/tmp/keyval_dump.bin
PASS=0; FAIL=0

# Build the tap if needed
[ -f "$TAP" ] || ./bas2tap tools/keytest/keytest.bas -o "$TAP" --auto-run >/dev/null 2>&1

# --- Key set: INJECT (type-keys sequence) / EXPECT (expected hex) ---
# Each key/combo produces exactly 1 captured byte.
LETTERS_INJ='ABCDEFGHIJKLMNOPQRSTUVWXYZ'
LETTERS_EXP='41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f 50 51 52 53 54 55 56 57 58 59 5a'
DIGITS_INJ='0123456789'
DIGITS_EXP='30 31 32 33 34 35 36 37 38 39'
SYMS_INJ='!"#$%&+-=.,/'
SYMS_EXP='21 22 23 24 25 26 2b 2d 3d 2e 2c 2f'
SPACE_INJ=' '
SPACE_EXP='20'
ARROW_INJ='\u\d\l\r'
ARROW_EXP='0b 0a 08 09'
ESC_INJ='\e'
ESC_EXP='1b'
# CTRL+letter (without C = BREAK)
CTRL_INJ='\Ca\Cb\Cd\Ce\Cf\Cg\Ch\Ci\Cj\Ck\Cl\Cm\Cn\Co\Cp\Cq\Cr\Cs\Ct\Cu\Cv\Cw\Cx\Cy\Cz'
CTRL_EXP='01 02 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 11 12 13 14 15 16 17 18 19 1a'
# FUNCT ignored by the BASIC ROM -> base key
FUNCT_INJ='\F1\Fa\Fz'
FUNCT_EXP='31 41 5a'
# Left/right SHIFT (distinct base keys to avoid the consecutive same-key
# combo artefact). The ROM does not distinguish L/R: shift+1='!',
# shift+letter=uppercase. Physical L/R distinction validated separately on LFMV2.
SHIFT_INJ='\L1\Rq\La\Rz'
SHIFT_EXP='21 51 41 5a'

INJECT="${LETTERS_INJ}${DIGITS_INJ}${SYMS_INJ}${SPACE_INJ}${ARROW_INJ}${ESC_INJ}${CTRL_INJ}${FUNCT_INJ}${SHIFT_INJ}"
EXPECT="${LETTERS_EXP} ${DIGITS_EXP} ${SYMS_EXP} ${SPACE_EXP} ${ARROW_EXP} ${ESC_EXP} ${CTRL_EXP} ${FUNCT_EXP} ${SHIFT_EXP}"

# Number of expected bytes
NBYTES=$(echo $EXPECT | wc -w)

run_rom() {
    local rom="$1" label="$2"
    echo "═══════════════════════════════════════════════════════════"
    echo "  $label  ($rom)"
    echo "═══════════════════════════════════════════════════════════"
    timeout 90 "$EMU" -r "$rom" -t "$TAP" -f --headless \
        --type-keys "4000000:RUN\\n\\p2${INJECT}" \
        --dump-ram-at 50000000:"$DUMP" >/dev/null 2>&1
    # Extract NBYTES bytes starting at 0x2000
    local got
    got=$(xxd -s 0x2000 -l "$NBYTES" -p "$DUMP" | tr -d '\n' | sed 's/../& /g' | tr 'A-F' 'a-f')
    # Compare byte by byte
    local exp_arr=($EXPECT) got_arr=($got)
    local i lp=0 lf=0
    for ((i=0; i<NBYTES; i++)); do
        if [ "${got_arr[i]:-XX}" = "${exp_arr[i]}" ]; then
            lp=$((lp+1))
        else
            lf=$((lf+1))
            printf "    MISMATCH @%-3d attendu=%s obtenu=%s\n" "$i" "${exp_arr[i]}" "${got_arr[i]:-(rien)}"
        fi
    done
    printf "  -> %d/%d octets corrects\n\n" "$lp" "$NBYTES"
    PASS=$((PASS+lp)); FAIL=$((FAIL+lf))
}

echo "Validation clavier : $NBYTES touches/combos par ROM"
echo "(lettres A-Z, chiffres 0-9, symboles, espace, fleches, ESC,"
echo " CTRL+lettre[sauf C=BREAK], FUNCT+x, SHIFT L/R + chiffre)"
echo
run_rom roms/basic10.rom  "ORIC-1  (BASIC 1.0)"
run_rom roms/basic11b.rom "ATMOS   (BASIC 1.1)"

echo "═══════════════════════════════════════════════════════════"
echo "  TOTAL : $PASS corrects, $FAIL erreurs (sur $((NBYTES*2)) vérifs)"
echo "═══════════════════════════════════════════════════════════"
[ "$FAIL" -eq 0 ]
