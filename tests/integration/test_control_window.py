#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
"""test_control_window.py — --control avec fenêtre (2.31.2).

Avant 2.31.2, --control imposait le mode headless : pas de fenêtre et une
machine non cadencée (≈ 40 M cycles par seconde). Depuis, sans -n, la fenêtre
est affichée et cadencée à 50 Hz, et l'attente des commandes rafraîchit la
fenêtre toutes les ~20 ms (select sur stdin, stdin non tamponné).

Vérifié ici avec le pilote vidéo SDL « dummy » (pas d'écran requis) :
  1. avec fenêtre : ~1 s de `continue` ≈ 1 M cycles (cadence 50 Hz) ;
  2. avec -n : toujours sans cadence (comportement inchangé pour les clients) ;
  3. avec fenêtre, deux commandes envoyées d'un seul write pendant la pause :
     les deux reçoivent leur réponse (sans stdin non tamponné, la 2e restait
     dans le tampon de stdio, invisible pour select).

Usage : test_control_window.py EMU ROM
"""
import os
import subprocess
import sys
import time

EMU, ROM = sys.argv[1], sys.argv[2]
checks = fails = 0


def check(cond, msg):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
    print(("  [OK]   " if cond else "  [FAIL] ") + msg)


class Emu:
    def __init__(self, extra):
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                   PHOSPHORIC_NO_CONFIG="1")
        self.p = subprocess.Popen([EMU, "-r", ROM, "--control"] + extra,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1, env=env)

    def send(self, txt):
        self.p.stdin.write(txt)
        self.p.stdin.flush()

    def until(self, prefix, timeout=10.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            line = self.p.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if line.startswith(prefix):
                return line
        return None

    def cycles(self, line):
        for tok in (line or "").split():
            if tok.startswith("cycles="):
                return int(tok[7:])
        return -1

    def run_for(self, seconds):
        """continue, attendre, pause : cycles exécutés."""
        self.send("regs\n")
        c0 = self.cycles(self.until("OK A="))
        self.send("continue\n")
        self.until("OK")
        time.sleep(seconds)
        self.send("pause\n")
        self.until("OK pc=")
        c1 = self.cycles(self.until("EVT stopped"))
        return c1 - c0

    def close(self):
        try:
            self.send("quit\n")
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


print("test_control_window — --control avec fenêtre (2.31.2)")

# Build sans SDL (SDL2=0) : pas de fenêtre possible, la partie fenêtrée est sautée.
has_sdl = b"libSDL2" in subprocess.run(["ldd", EMU], capture_output=True).stdout
if not has_sdl:
    print("  SKIP fenêtre : binaire sans SDL2")

if has_sdl:
    e = Emu([])
    first = e.p.stdout.readline().strip()
    check(first.startswith("EVT ready"),
          f"avec fenêtre : stdout commence par EVT ready (pas de bannière ; lu : `{first}`)")
    check(e.until("EVT stopped") is not None, "avec fenêtre : session ouverte, machine arrêtée")
    ran = e.run_for(1.0)
    check(0 < ran < 2500000, f"avec fenêtre : ~1 s de continue = {ran} cycles (< 2,5 M : cadencée à 50 Hz)")
    e.send("regs\nregs\n")
    r1, r2 = e.until("OK A=", 3), e.until("OK A=", 3)
    check(r1 is not None and r2 is not None, "avec fenêtre : deux commandes d'un seul write, deux réponses")
    e.close()
    check(e.p.returncode == 0, "avec fenêtre : quit, sortie propre")

h = Emu(["-n"])
h.until("EVT stopped")
ran = h.run_for(1.0)
check(ran > 5000000, f"avec -n : ~1 s de continue = {ran} cycles (> 5 M : non cadencée, comme avant)")
h.close()

print(f"{checks - fails}/{checks} vérifications OK")
sys.exit(1 if fails else 0)
