#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
"""test_loci_warm_resume.py — LOCI intégré : bouton Action puis RETURN (2.31.6).

Avant 2.31.6, le bouton à chaud du LOCI intégré faisait un reset direct vers
le menu : la routine de sauvegarde de la ROM du menu (mia_save_state, vecteur
NMI, atteinte sur le matériel par le JMP ($FFFA) du piège) ne tournait
jamais, le tampon de reprise en XRAM n'avait pas sa marque 'L', et le menu
ne proposait pas RETURN — la session était perdue.

Depuis, comme sur le matériel : JMP ($FFFA) dans la ROM du menu (sauvegarde
en XRAM), RETURN → mia_restore_state → MIA_BOOT + RESUME ; la ROM de la
session est remise quand le 6502 tourne dans le spin $03B0, qui sort par
JMP ($FFFE) (gestionnaire IRQ de la session, puis RTI).

Vérifié ici, avec la commande `loci-button` de --control :
  1. le menu propose RETURN : il affiche le mode vidéo sauvegardé (« T50 »…),
     ce qu'il ne fait que si le tampon de reprise porte la marque 'L' ;
  2. au retour au BASIC (vecteur JMP ($FFFE), spin $03B3), la mémoire
     $0000-$BFFF est celle d'avant l'appui, à la trame IRQ près ($01xx) ;
  3. les registres A/X/Y sont ceux d'avant l'appui ;
  4. le programme BASIC interrompu continue (son compteur avance) ;
  5. un nouvel appui puis ESC (Boot) redémarre le BASIC à froid.

Usage : test_loci_warm_resume.py EMU ROM
"""
import os
import select
import subprocess
import sys
import tempfile
import time

EMU, ROM = sys.argv[1], sys.argv[2]
MENU = os.path.join(os.path.dirname(ROM) or ".", "loci", "locirom")
checks = fails = 0


def check(cond, msg):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
    print(("  [OK]   " if cond else "  [FAIL] ") + msg)


print("test_loci_warm_resume — LOCI intégré : bouton Action puis RETURN (2.31.6)")
if not os.path.exists(MENU):
    print(f"  SKIP : ROM du menu LOCI absente ({MENU})")
    sys.exit(0)

tmpdir = tempfile.TemporaryDirectory()
tmp = tmpdir.name
p = subprocess.Popen([EMU, "-n", "--no-config", "-r", ROM, "--loci", "--control"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
OUT_FD = p.stdout.fileno()
pending = b""


def send(t):
    p.stdin.write(t + "\n")
    p.stdin.flush()


def until(prefix, timeout=15.0):
    """Première ligne commençant par PREFIX, ou None (délai, fin du processus).
    Lecture brute + select : une attente bornée même si l'émulateur se tait."""
    global pending
    t0 = time.time()
    while True:
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            line = raw.decode(errors="replace").strip()
            if line.startswith(prefix):
                return line
        left = timeout - (time.time() - t0)
        if left <= 0 or not select.select([OUT_FD], [], [], left)[0]:
            return None
        chunk = os.read(OUT_FD, 65536)
        if not chunk:
            return None
        pending += chunk


def run(seconds):
    send("continue")
    until("OK")
    time.sleep(seconds)
    send("pause")
    until("EVT stopped")


def regs():
    send("regs")
    line = until("OK A=") or ""
    return dict(t.split("=", 1) for t in line.split()[1:] if "=" in t)


def screen():
    send("read bb80 460")
    line = until("OK") or "OK"
    try:
        b = bytes.fromhex(line[3:].replace(" ", ""))
    except ValueError:
        return ""
    return bytes(c & 0x7F if 32 <= (c & 0x7F) < 127 else 32 for c in b).decode()


def counter(scr):
    """Valeur affichée par PLOT 2,10 (ligne 11 de l’écran : la ligne 0 est la ligne d’état)."""
    try:
        return int(scr[440:480].split()[0])
    except (ValueError, IndexError):
        return -1


def save_ram(name):
    path = os.path.join(tmp, name)
    send(f"save-mem {path} 0000 c000")
    until("OK")
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return b""


try:
    until("EVT stopped")
    run(0.3)
    send(r"keys 10 I=I+1:PLOT 2,10,STR$(I):GOTO 10\nRUN\n")
    until("OK")
    run(1.0)
    before_ram = save_ram("before.bin")
    before_regs = regs()
    before_count = counter(screen())

    send("loci-button")
    until("OK")
    run(1.5)
    menu = screen()
    # Le menu n'écrit le mode vidéo sauvegardé (T/H + 50/60, colonne 34 de
    # la ligne 0) que si mia_restore_buffer_ok() voit la marque 'L'.
    vmode = menu[34:37]
    check(vmode[:1] in ("T", "H") and vmode[1:] in ("50", "60"),
          f"le menu propose RETURN (tampon de reprise en XRAM, mode « {vmode} »)")

    send("break 03B3")
    until("OK")
    send(r"keys \n")
    until("OK")
    send("continue")
    until("OK")
    stop = until("EVT stopped", 20) or ""
    check("pc=03B3" in stop, f"RETURN : le spin sort par JMP ($FFFE) ({stop or 'pas d’arrêt'})")
    after_regs = regs()
    after_ram = save_ram("after.bin")
    n = min(len(before_ram), len(after_ram))
    diff = [i for i in range(n) if before_ram[i] != after_ram[i]]
    outside_stack = [i for i in diff if not 0x0100 <= i <= 0x01FF]
    check(len(before_ram) == len(after_ram) == 0xC000 and not outside_stack,
          f"mémoire $0000-$BFFF identique hors pile ({len(diff)} octet(s) de pile, "
          f"{len(outside_stack)} ailleurs)")
    same = all(before_regs.get(k) == after_regs.get(k) for k in ("A", "X", "Y"))
    check(same, f"registres A/X/Y repris ({before_regs.get('A')}/{before_regs.get('X')}/"
                f"{before_regs.get('Y')} → {after_regs.get('A')}/{after_regs.get('X')}/"
                f"{after_regs.get('Y')})")
    send("unbreak 0")
    until("OK")
    run(1.0)
    after_count = counter(screen())
    check(after_count > before_count > 0,
          f"le programme interrompu continue (compteur {before_count} → {after_count})")

    send("loci-button")
    until("OK")
    run(1.5)
    send(r"keys \e")
    until("OK")
    run(2.0)
    cold = screen()
    check("BYTES FREE" in cold and counter(cold) == -1,
          "nouvel appui puis ESC : BASIC redémarré à froid")
finally:
    try:
        send("quit")
        p.wait(timeout=5)
    except Exception:
        p.kill()
    tmpdir.cleanup()

print(f"{checks - fails}/{checks} vérifications OK")
sys.exit(1 if fails else 0)
