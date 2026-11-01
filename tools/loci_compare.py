#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
"""loci_compare.py — même scénario sur la LOCI intégrée et sur une vraie LOCI (2.31.14).

Usage :
  loci_compare.py [--a ARGS] [--b ARGS] [--only NOM] [--out DOSSIER]

Lance chaque scénario deux fois sous --control, avec deux jeux d'options :
  A (défaut) : --loci                 (modèle intégré de Phosphoric)
  B (défaut) : --loci-hw /dev/ttyACM0 (Feather avec le vrai firmware LOCI)
puis compare, étape par étape, l'écran texte ($BB80) et les valeurs lues par
BASIC. Les différences sont la liste de travail pour rapprocher l'émulation du
firmware réel. Hors `make tests` : il faut la Feather.

Étapes d'un scénario (liste de tuples) :
  ("run", s)        fait tourner la machine s secondes (temps réel)
  ("until", txt, s) fait tourner jusqu'à voir txt à l'écran (au plus s secondes)
  ("keys", txt)     frappe (syntaxe de la commande keys)
  ("button",)       bouton Action (loci-button)
  ("snap", nom)     capture : écran texte + ligne 0 + mode vidéo
  ("settle", s)      fait tourner jusqu'à ce que l'écran ait changé depuis la dernière
                    frappe et finisse par « Ready » (au plus s secondes) : indépendant
                    de la vitesse de l'émulation
  ("peeks", nom)    capture la ligne affichée juste avant le dernier « Ready » (valeurs PRINT)

Author: bmarty <bmarty@mailo.com>
"""
import argparse
import os
import select
import shlex
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SCENARIOS = {
    # Identification et registres de la MIA lus par BASIC.
    "registres": [
        ("until", "Ready", 15),
        ("keys", r"CLS:PRINT PEEK(#319);PEEK(#3A0) AND 128;PEEK(#3B2)\n"),
        ("settle", 30), ("peeks", "id-console-busy"),
    ],
    # XRAM : écriture puis relecture par la fenêtre RW0 (pas +1).
    "xram": [
        ("until", "Ready", 15),
        ("keys", r"CLS:POKE #3A5,1:POKE #3A6,0:POKE #3A7,16:FOR I=0 TO 7:POKE #3A4,I*3:NEXT\n"),
        ("settle", 30),
        ("keys", r"POKE #3A6,0:POKE #3A7,16:FOR I=0 TO 7:PRINT PEEK(#3A4);:NEXT\n"),
        ("settle", 30), ("peeks", "xram-rw0"),
    ],
    # xstack : empilement par une routine en STA $03AC (adressage absolu, sans
    # lecture préalable), puis dépilement par PEEK : 40 30 20 10 attendu (LIFO).
    "xstack": [
        ("until", "Ready", 15),
        ("keys", r"100 DATA 169,10,141,172,3,169,20,141,172,3\n"
                 r"110 DATA 169,30,141,172,3,169,40,141,172,3,96\n"
                 r"CLS:FOR I=0 TO 20:READ B:POKE #400+I,B:NEXT:CALL #400\n"),
        ("settle", 30),
        ("keys", r"FOR I=1 TO 4:PRINT PEEK(#3AC);:NEXT\n"),
        ("settle", 30), ("peeks", "xstack-lifo"),
    ],
    # xstack par POKE : la ROM écrit par STA ($33),Y, qui LIT l'adresse avant
    # d'écrire (NMOS 6502, 5e cycle) ; chaque lecture de $03AC dépile. Attendu sur
    # un vrai Oric : 40 0 0 0 (seul le dernier octet reste).
    "xstack-poke": [
        ("until", "Ready", 15),
        ("keys", r"CLS:FOR I=1 TO 4:POKE #3AC,I*10:NEXT:FOR I=1 TO 4:PRINT PEEK(#3AC);:NEXT\n"),
        ("settle", 30), ("peeks", "xstack-poke"),
    ],
    # Bouton Action : comme sur un vrai Oric, on passe d'abord par le menu LOCI et
    # « Boot » (ESC) : le firmware ne fait l'appui À CHAUD (piège IRQ + sauvegarde
    # en XRAM) que s'il a lui-même démarré la machine (main_active() dans ext.c) ;
    # sinon l'appui est À FROID (nRESET, menu sans RETURN).
    "bouton": [
        ("until", "Ready", 15),
        ("button",), ("until", "ESC = boot", 15),
        ("keys", r"\e"), ("until", "Ready", 20),
        ("keys", r"10 I=I+1:PLOT 2,10,STR$(I):GOTO 10\nRUN\n"),
        ("run", 3),
        ("button",), ("until", "ESC = boot", 20), ("run", 2), ("snap", "menu"),
        ("keys", r"\n"), ("run", 6), ("snap", "reprise"),
    ],
}


class Emu:
    def __init__(self, args):
        cmd = [os.path.join(ROOT, "oric1-emu"), "-n", "--no-config",
               "-r", os.path.join(ROOT, "roms", "basic11b.rom"), "--control"] + args
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, bufsize=0)
        self.buf = b""

    def send(self, t):
        self.p.stdin.write((t + "\n").encode())
        self.p.stdin.flush()

    def until(self, prefix, timeout=15.0):
        t0 = time.time()
        while True:
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip()
                if line.startswith(prefix):
                    return line
            left = timeout - (time.time() - t0)
            if left <= 0 or not select.select([self.p.stdout], [], [], left)[0]:
                return None
            chunk = os.read(self.p.stdout.fileno(), 65536)
            if not chunk:
                return None
            self.buf += chunk

    def run(self, sec):
        self.send("continue")
        self.until("OK")
        time.sleep(sec)
        self.send("pause")
        self.until("EVT stopped")

    def screen(self):
        self.send("read bb80 460")
        line = self.until("OK") or "OK"
        try:
            b = bytes.fromhex(line[3:].replace(" ", ""))
        except ValueError:
            return [""] * 28
        t = bytes(c & 0x7F if 32 <= (c & 0x7F) < 127 else 32 for c in b).decode()
        return [t[i:i + 40].rstrip() for i in range(0, len(t), 40)]

    def video(self):
        self.send("peek video")
        line = self.until("OK") or ""
        return " ".join(t for t in line.split() if not t.startswith("frame="))

    def close(self):
        try:
            self.send("quit")
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def play(args, steps):
    """Joue STEPS ; renvoie {nom de capture: valeur}."""
    e = Emu(args)
    out = {}
    ready_before = []
    try:
        e.until("EVT stopped")
        for st in steps:
            op = st[0]
            if op == "run":
                e.run(st[1])
            elif op == "until":
                t0 = time.time()
                while time.time() - t0 < st[2]:
                    e.run(0.3)
                    if any(st[1] in r for r in e.screen()):
                        break
            elif op == "keys":
                ready_before = e.screen()            # écran au moment de la frappe
                e.send("keys " + st[1])
                e.until("OK")
            elif op == "settle":
                t0 = time.time()
                while time.time() - t0 < st[1]:
                    e.run(0.4)
                    full = e.screen()
                    scr = [r.strip() for r in full if r.strip()]
                    if scr and scr[-1] == "Ready" and full != ready_before:
                        break
            elif op == "button":
                e.send("loci-button")
                out["button-reply"] = e.until("OK") or e.until("ERR") or "(pas de réponse)"
            elif op == "snap":
                scr = e.screen()
                out[st[1] + ":ecran"] = "\n".join(scr)
                out[st[1] + ":ligne0"] = scr[0]
                out[st[1] + ":video"] = e.video()
            elif op == "peeks":
                # la ligne affichée juste avant le dernier « Ready »
                rows = [r.strip() for r in e.screen()[1:] if r.strip()]
                last = max((i for i, r in enumerate(rows) if r == "Ready"), default=-1)
                out[st[1]] = rows[last - 1] if last > 0 else ""
    finally:
        e.close()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--a", default="--loci", help="options du côté A (défaut : --loci)")
    ap.add_argument("--b", default="--loci-hw /dev/ttyACM0",
                    help="options du côté B (défaut : --loci-hw /dev/ttyACM0)")
    ap.add_argument("--only", help="un seul scénario")
    ap.add_argument("--out", help="dossier où écrire les captures complètes")
    o = ap.parse_args()
    names = [o.only] if o.only else list(SCENARIOS)
    diffs = screens = 0
    for name in names:
        ra = play(shlex.split(o.a), SCENARIOS[name])
        rb = play(shlex.split(o.b), SCENARIOS[name])
        print(f"== {name}")
        for k in sorted(set(ra) | set(rb)):
            va, vb = ra.get(k, "(absent)"), rb.get(k, "(absent)")
            same = va == vb
            if k.endswith(":ecran"):
                screens += 0 if same else 1   # écran complet : animations possibles
                print(f"  {'=' if same else '≠'} {k}" + ("" if same else " (voir --out)"))
            else:
                diffs += 0 if same else 1
                print(f"  {'=' if same else '≠'} {k}: A={va!r}" + ("" if same else f"  B={vb!r}"))
        if o.out:
            os.makedirs(o.out, exist_ok=True)
            for side, r in (("A", ra), ("B", rb)):
                with open(os.path.join(o.out, f"{name}.{side}.txt"), "w") as f:
                    for k in sorted(r):
                        f.write(f"--- {k}\n{r[k]}\n")
    print(f"{diffs} différence(s) ; {screens} écran(s) complet(s) différent(s) "
          "(peut venir d'une animation, à regarder avec --out)")
    sys.exit(1 if diffs else 0)


if __name__ == "__main__":
    main()
