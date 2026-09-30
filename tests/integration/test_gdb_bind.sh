#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
#
# test_gdb_bind.sh — bind address of the GDB stub (--gdb, --gdb-bind).
#
# The stub grants full access to the machine (memory, registers, execution)
# without authentication: by default it listens on 127.0.0.1 only. Checked
# with real connections:
#   1. default: 127.0.0.1 accepted, the host's network address refused;
#   2. --gdb-bind 0.0.0.0: the host's network address accepted;
#   3. invalid --gdb-bind: explicit refusal, no listening.
#
# Author: bmarty <bmarty@mailo.com>

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
EMU=${EMU:-./oric1-emu}

echo "=== GDB stub: adresse d'écoute ==="
exec python3 - "$EMU" <<'PY'
import os, socket, subprocess, sys, time

emu = sys.argv[1]
passed = failed = 0
def check(ok, msg):
    global passed, failed
    print(("  PASS: " if ok else "  FAIL: ") + msg)
    if ok: passed += 1
    else:  failed += 1

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close()
    return p

def host_ip():
    """Adresse IPv4 non locale de l'hôte (route par défaut), sans envoyer de paquet."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))          # TEST-NET-1: nothing is sent (UDP)
        ip = s.getsockname()[0]
    except OSError:
        ip = None
    finally:
        s.close()
    return None if not ip or ip.startswith("127.") or ip == "0.0.0.0" else ip

def start(extra):
    port = free_port()
    args = [emu, "--headless", "--no-config", "-c", "1000000", "--gdb=%d" % port] + extra
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p, port

def can_connect(ip, port, wait=5.0):
    """Vrai dès qu'une connexion aboutit ; faux si refusée jusqu'à l'échéance."""
    end = time.time() + wait
    while time.time() < end:
        try:
            c = socket.create_connection((ip, port), timeout=1.0)
            c.close()
            return True
        except OSError:
            time.sleep(0.1)
    return False

def stop(p):
    p.kill()
    try: out = p.communicate(timeout=5)[0].decode("utf-8", "replace")
    except subprocess.TimeoutExpired: out = ""
    return out

ip = host_ip()

# 1. default: local listening only.
p, port = start([])
if ip:
    # Network address first (the stub closes after the first client).
    check(not can_connect(ip, port, wait=2.0), "défaut : %s refusée" % ip)
check(can_connect("127.0.0.1", port), "défaut : 127.0.0.1 acceptée")
out = stop(p)
check("127.0.0.1:%d" % port in out, "défaut : journal « waiting for connection on 127.0.0.1 »")

# 2. --gdb-bind 0.0.0.0: every interface.
if ip:
    p, port = start(["--gdb-bind", "0.0.0.0"])
    check(can_connect(ip, port), "--gdb-bind 0.0.0.0 : %s acceptée" % ip)
    stop(p)
else:
    print("  SKIP: pas d'adresse IPv4 non locale sur cet hôte (cas 0.0.0.0 non vérifiable)")

# 3. invalid address: no listening, explicit message.
p, port = start(["--gdb-bind", "pas.une.adresse"])
check(not can_connect("127.0.0.1", port, wait=1.5), "adresse invalide : pas d'écoute")
out = stop(p)
check("invalid bind address" in out, "adresse invalide : message explicite")

print("  Results: %d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
PY
