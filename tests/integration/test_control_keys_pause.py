#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
"""test_control_keys_pause.py — `keys` de --control, caractères avec SHIFT (2.31.8).

Avant 2.31.8, la commande `keys` perdait parfois SHIFT : « ( » devenait « 9 »,
« : » devenait « ; », « $ » devenait « 4 ». Deux causes :
  1. l'injecteur comptait la tenue d'une touche en trames ; une `pause` coupe
     une trame et la raccourcissait (3 lignes fausses sur 10 avec une pause
     toutes les 0,3 s) ;
  2. la touche et SHIFT apparaissaient au même instant ; la ROM lit les rangées
     de 7 à 0 et SHIFT est sur la rangée 4 : une touche d'une rangée plus basse
     apparue entre les deux lectures était vue sans SHIFT (1 sur 30 sans pause).
Depuis : tenue et intervalle comptés en cycles + passes du scanner, et SHIFT
pressé seul d'abord.

Vérifié ici : une ligne riche en caractères avec SHIFT, tapée par `keys`
pendant que le client alterne `continue` / `pause` toutes les 0,1 s, puis
relue par LIST — sur plusieurs sessions dont le départ tombe à des cycles
différents.

Usage : test_control_keys_pause.py EMU ROM
"""
import subprocess
import sys
import time

EMU, ROM = sys.argv[1], sys.argv[2]
LINE = '10 A$="(X):Y";B=1+2:PRINT A$;STR$(B)'
SESSIONS = 4
checks = fails = 0


def check(cond, msg):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
    print(("  [OK]   " if cond else "  [FAIL] ") + msg)


def session(warmup):
    p = subprocess.Popen([EMU, "-n", "--no-config", "-r", ROM, "--control"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def send(t):
        p.stdin.write(t + "\n")
        p.stdin.flush()

    def until(prefix):
        while True:
            line = p.stdout.readline()
            if not line:
                return ""
            line = line.strip()
            if line.startswith(prefix):
                return line

    def run(sec):
        send("continue")
        until("OK")
        time.sleep(sec)
        send("pause")
        until("EVT stopped")

    try:
        until("EVT stopped")
        run(warmup)
        send("keys " + LINE + r"\nLIST\n")
        until("OK")
        t0 = time.time()
        rows = []
        while time.time() - t0 < 30:  # pause toutes les 0,1 s jusqu'à la sortie de LIST
            run(0.1)
            send("read bb80 460")
            raw = until("OK")
            b = bytes.fromhex(raw[3:].replace(" ", ""))
            text = bytes(c & 0x7F if 32 <= (c & 0x7F) < 127 else 32 for c in b).decode()
            rows = [text[i:i + 40].strip() for i in range(0, len(text), 40)]
            # LIST fini : la ligne listée est suivie d'un « Ready »
            listed = [i for i, r in enumerate(rows) if r.startswith("10 ")]
            if len(listed) >= 2 and "Ready" in rows[listed[1] + 1:]:
                break
        return rows
    finally:
        try:
            send("quit")
            p.wait(timeout=5)
        except Exception:
            p.kill()


print("test_control_keys_pause — `keys` + SHIFT sous pause/continue (2.31.8)")
for k in range(SESSIONS):
    rows = session(0.3 + 0.07 * k)
    listed = [r for r in rows if r.startswith("10 ")]
    ok = len(listed) >= 2 and all(r == LINE for r in listed[:2])
    check(ok, f"session {k + 1} : ligne tapée et LIST identiques à « {LINE} » "
              f"(lu : {listed[:2]})")

print(f"{checks - fails}/{checks} vérifications OK")
sys.exit(1 if fails else 0)
