#!/usr/bin/env python3
"""
test_savestate_determinism.py — un savestate est un point de reprise EXACT (V2-E7).

Propriété testée : « sauver à l'instant T puis reprendre » produit la même
machine que « continuer sans s'arrêter ». Le point de sauvegarde est pris en
PLEINE trame (après quelques `step`), donc le test couvre ce qui manquait aux
.ost antérieurs :

  - la position du balayage (section CLK) — un point d'arrêt raster doit
    tomber au MÊME cycle CPU après reprise ;
  - l'état au cycle du VIA (t1_active/t1_reload…) et l'échantillon
    d'interruption du cycle pénultième — les IRQ du Timer 1 doivent tomber au
    même cycle, donc PC/cycles identiques à chaque arrêt ;
  - la RAM, en fin de parcours.

Le run de référence et le run repris passent par les mêmes arrêts raster
(ligne 100, 4 trames de suite) et sont comparés arrêt par arrêt.
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phos_smoke_client import PhosClient  # noqa: E402

EMU = "./oric1-emu"
ROM = "roms/basic11b.rom" if os.path.exists("roms/basic11b.rom") else "roms/basic10.rom"
STEPS_BEFORE_SAVE = 37       # arbitraire, pas aligné sur une trame ni une ligne
RASTER_LINE = 100
STOPS = 4
RAM_END = 0xC000

passed = 0
failed = 0


def ok(msg):
    global passed
    print(f"  [OK]   {msg}")
    passed += 1


def ko(msg):
    global failed
    print(f"  [FAIL] {msg}")
    failed += 1


def stop_info(line):
    """'EVT stopped pc=XXXX cycles=N reason=raster' → (pc, cycles)."""
    kv = dict(tok.split("=", 1) for tok in line.split()[2:] if "=" in tok)
    return kv.get("pc"), kv.get("cycles"), kv.get("reason")


def run_stops(c, ram_path):
    """Pose le point d'arrêt raster, enchaîne STOPS arrêts, dump la RAM."""
    c.ok(f"raster {RASTER_LINE}")
    stops = []
    for _ in range(STOPS):
        c.cont()
        ev = c.wait_stopped(timeout=10.0)
        if ev is None:
            raise RuntimeError("no stop event")
        stops.append(stop_info(ev))
    via = c.peek("via")
    c.ok(f"save-mem {ram_path} 0 {RAM_END:X}")
    return stops, via


# Scénarios : (nom, arguments en plus, trames avant la sauvegarde, médias requis).
# Les cartes à section .ost propre (sprint D) sont reprises dans la même machine :
# un champ oublié au chargement ferait diverger les arrêts, le VIA ou la RAM.
SCENARIOS = [
    ("machine de base", [], 0, []),
    # FTDOS (disque Jasmin TDOS, média local) : à 100 trames le boot vient de
    # commencer, à 250 un secteur est EN COURS de lecture (reprise en plein
    # transfert : cur_sector_data + drq_age, défaut corrigé en 2.4.0).
    ("Jasmin en début de boot TDOS",
     ["--jasmin-rom", "roms/jasmin.rom", "-d", "disks/FTDOS.dsk"], 100,
     ["roms/jasmin.rom", "disks/FTDOS.dsk"]),
    ("Jasmin en plein transfert disque (TDOS)",
     ["--jasmin-rom", "roms/jasmin.rom", "-d", "disks/FTDOS.dsk"], 250,
     ["roms/jasmin.rom", "disks/FTDOS.dsk"]),
    # Microdisc + Sedoric (Citadelle, média local) : même défaut de reprise en
    # plein transfert ; la 2.3.0 divergeait à 100 et 200 trames.
    ("Microdisc en plein boot Sedoric (100 trames)",
     ["--disk-rom", "roms/microdis.rom", "-d", "disks/Citadelle.dsk"], 100,
     ["roms/microdis.rom", "disks/Citadelle.dsk"]),
    ("Microdisc en plein boot Sedoric (200 trames)",
     ["--disk-rom", "roms/microdis.rom", "-d", "disks/Citadelle.dsk"], 200,
     ["roms/microdis.rom", "disks/Citadelle.dsk"]),
    ("synthèse MEA8000 présente", ["--mea8000"], 60, []),
]


def main():
    if not os.path.exists(EMU):
        print(f"  [SKIP] {EMU} non construit")
        return 0
    if not os.path.exists(ROM):
        print("  [SKIP] aucune ROM BASIC")
        return 0
    for name, extra, warm, media in SCENARIOS:
        missing = [m for m in media if not os.path.exists(m)]
        if missing:
            print(f"  [SKIP] {name} : {', '.join(missing)} absent")
            continue
        run_scenario(name, extra, warm)
    print(f"=== result: {passed} passed, {failed} failed ===")
    return 1 if failed else 0


def run_scenario(name, extra, warm):
    print(f"=== Savestate = point de reprise exact (V2-E7, US7.2) : {name} ===")
    with tempfile.TemporaryDirectory() as tmp:
        ost = os.path.join(tmp, "mid.ost")
        ram_ref = os.path.join(tmp, "ref.bin")
        ram_res = os.path.join(tmp, "res.bin")

        # 1. Référence : step×N, sauvegarde en pleine trame, puis on CONTINUE.
        with PhosClient.spawn([EMU, "-r", ROM, "-n"] + extra) as c:
            c.wait_ready()
            if warm:                                   # machine en activité
                c.ok(f"raster {RASTER_LINE}")
                for _ in range(warm):
                    c.cont()
                    if c.wait_stopped(timeout=10.0) is None:
                        raise RuntimeError("no stop event (warm-up)")
            for _ in range(STEPS_BEFORE_SAVE):
                c.step()
            at_save = c.regs()
            c.ok(f"state-save {ost}")
            ref_stops, ref_via = run_stops(c, ram_ref)
            c.cmd("quit")

        cyc = int(at_save["cycles"])
        ok(f"sauvegarde en pleine trame (cycle {cyc}, ligne {cyc % 19968 // 64})")
        if not os.path.exists(ost):
            ko("state-save n'a rien écrit")
            return

        # 2. Reprise : --load-state, puis le MÊME parcours.
        with PhosClient.spawn([EMU, "-r", ROM, "-n", "--load-state", ost] + extra) as c:
            c.wait_ready()
            at_load = c.regs()
            res_stops, res_via = run_stops(c, ram_res)
            c.cmd("quit")

        # 3. Comparaisons.
        if at_load["PC"] == at_save["PC"] and at_load["cycles"] == at_save["cycles"]:
            ok(f"reprise au même point : PC={at_load['PC']} cycles={at_load['cycles']}")
        else:
            ko(f"reprise ailleurs : sauvé {at_save}, chargé {at_load}")

        for i, (r, g) in enumerate(zip(ref_stops, res_stops)):
            if r == g:
                ok(f"arrêt raster {i + 1}/{STOPS} identique : pc={r[0]} cycles={r[1]}")
            else:
                ko(f"arrêt raster {i + 1}/{STOPS} diverge : référence {r}, reprise {g}")

        if ref_via == res_via:
            ok("VIA identique après reprise (timers, IFR, ports)")
        else:
            diff = {k: (ref_via.get(k), res_via.get(k))
                    for k in set(ref_via) | set(res_via) if ref_via.get(k) != res_via.get(k)}
            ko(f"VIA diverge : {diff}")

        with open(ram_ref, "rb") as f:
            a = f.read()
        with open(ram_res, "rb") as f:
            b = f.read()
        if a == b and len(a) == RAM_END:
            ok(f"RAM $0000-${RAM_END - 1:04X} identique après reprise")
        else:
            first = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), None)
            ko(f"RAM diverge (tailles {len(a)}/{len(b)}, premier écart à ${first:04X})"
               if first is not None else f"RAM diverge (tailles {len(a)}/{len(b)})")



if __name__ == "__main__":
    sys.exit(main())
