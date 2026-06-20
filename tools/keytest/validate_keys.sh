#!/usr/bin/env bash
# =====================================================================
# validate_keys.sh — validation automatisée de la détection clavier
# (ROM KEY$) en mode ORIC-1 (BASIC 1.0) et Atmos (BASIC 1.1).
#
# Principe : keytest.bas tourne dans l'émulateur et POKE le code ASCII
# de chaque touche détectée (KEY$) dans un buffer à #2000. On injecte
# une séquence de touches/combos via --type-keys, on dump la RAM, et on
# compare le buffer au vecteur attendu, octet par octet.
#
# CTRL+C est EXCLU du jeu BASIC : c'est la touche BREAK, qui interrompt
# le programme (validé séparément = 0x03 via le LOCI File Manager).
# =====================================================================
set -u
cd "$(dirname "$0")/../.."   # racine du dépôt

EMU=./oric1-emu
TAP=tools/keytest/keytest.tap
DUMP=/tmp/keyval_dump.bin
PASS=0; FAIL=0

# Construit le tap si besoin
[ -f "$TAP" ] || ./bas2tap tools/keytest/keytest.bas -o "$TAP" --auto-run >/dev/null 2>&1

# --- Jeu de touches : INJECT (séquence type-keys) / EXPECT (hex attendu) ---
# Chaque touche/combo produit exactement 1 octet capturé.
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
# CTRL+lettre (sans C = BREAK)
CTRL_INJ='\Ca\Cb\Cd\Ce\Cf\Cg\Ch\Ci\Cj\Ck\Cl\Cm\Cn\Co\Cp\Cq\Cr\Cs\Ct\Cu\Cv\Cw\Cx\Cy\Cz'
CTRL_EXP='01 02 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 11 12 13 14 15 16 17 18 19 1a'
# FUNCT ignoré par le ROM BASIC -> touche de base
FUNCT_INJ='\F1\Fa\Fz'
FUNCT_EXP='31 41 5a'
# SHIFT gauche/droit (touches de base distinctes pour éviter l'artefact
# combos même-touche consécutifs). Le ROM ne distingue pas L/R : shift+1='!',
# shift+lettre=majuscule. Distinction physique L/R validée séparément sur LFMV2.
SHIFT_INJ='\L1\Rq\La\Rz'
SHIFT_EXP='21 51 41 5a'

INJECT="${LETTERS_INJ}${DIGITS_INJ}${SYMS_INJ}${SPACE_INJ}${ARROW_INJ}${ESC_INJ}${CTRL_INJ}${FUNCT_INJ}${SHIFT_INJ}"
EXPECT="${LETTERS_EXP} ${DIGITS_EXP} ${SYMS_EXP} ${SPACE_EXP} ${ARROW_EXP} ${ESC_EXP} ${CTRL_EXP} ${FUNCT_EXP} ${SHIFT_EXP}"

# Nombre d'octets attendus
NBYTES=$(echo $EXPECT | wc -w)

run_rom() {
    local rom="$1" label="$2"
    echo "═══════════════════════════════════════════════════════════"
    echo "  $label  ($rom)"
    echo "═══════════════════════════════════════════════════════════"
    timeout 90 "$EMU" -r "$rom" -t "$TAP" -f --headless \
        --type-keys "4000000:RUN\\n\\p2${INJECT}" \
        --dump-ram-at 50000000:"$DUMP" >/dev/null 2>&1
    # Extrait NBYTES octets à partir de 0x2000
    local got
    got=$(xxd -s 0x2000 -l "$NBYTES" -p "$DUMP" | tr -d '\n' | sed 's/../& /g' | tr 'A-F' 'a-f')
    # Compare octet par octet
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
