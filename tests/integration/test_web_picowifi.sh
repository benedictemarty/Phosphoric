#!/usr/bin/env bash
#
# test_web_picowifi.sh — e2e du modem picowifi dans la build WebAssembly
#
# Chrome headless (Playwright) sur la page servie localement, trois scénarios
# (détail dans web_picowifi_e2e.js) :
#   1. relais WebSocket : BASIC -> ACIA $031C -> picowifi -> WebSocket ->
#      tools/picowifi_ws_relay.py -> serveur TCP local, et retour ;
#   2. sans relais (?relay=none) : ATGET rejoué par fetch() (serveur CORS),
#      ATRD = heure du navigateur ;
#   3. LOCI + cassette + ?httpsame= (cas ProphetOric) : ACIA $0380, ATD- puis
#      HTTP brut réécrit vers l'origine de la page, ResponseFormat transmis.
# Les programmes BASIC sont tokenisés en .tap auto-run (bas2tap) et chargés par
# ?media= : pas de frappe clavier simulée.
#
# SKIP (exit 0) si emcc, node, Playwright ou Chrome sont absents.
set -u
cd "$(dirname "$0")/../.."

skip() { echo "SKIP test-web-picowifi: $*"; exit 0; }

if ! command -v emcc >/dev/null 2>&1 && [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
    # shellcheck disable=SC1091
    source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
fi
command -v emcc >/dev/null 2>&1 || skip "emcc introuvable (emsdk)"
command -v node >/dev/null 2>&1 || skip "node introuvable"
command -v python3 >/dev/null 2>&1 || skip "python3 introuvable"

echo "test-web-picowifi: make wasm bas2tap…"
make -s bas2tap >/dev/null 2>&1 || { echo "FAIL: make bas2tap"; exit 1; }
make -s wasm >/dev/null 2>&1 || { echo "FAIL: make wasm"; exit 1; }

freeport() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
HTTP=$(freeport); RELAY=$(freeport); TCP=$(freeport); FETCH=$(freeport)
TMP=$(mktemp -d)
WWW="$TMP/www"; mkdir -p "$WWW"
RXLOG="$TMP/rx.log"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; rm -rf "$TMP"; }
trap cleanup EXIT

for f in web/*; do ln -s "$PWD/$f" "$WWW/"; done

# Tronc commun : ACIA à l'adresse AC (control $1E, command 3), envoi de S$ par
# GOSUB 100 (en rangeant l'écho), réception par GOSUB 200 jusqu'à ~1500 tours
# de silence. Tout octet reçu va en #4000+N.
common() { cat <<'BAS'
100 FOR I=1 TO LEN(S$)
110 IF (PEEK(A+1) AND 16)=0 THEN 110
120 POKE A,ASC(MID$(S$,I,1))
130 IF PEEK(A+1) AND 8 THEN POKE 16384+N,PEEK(A):N=N+1
140 NEXT:RETURN
200 T=0
210 IF PEEK(A+1) AND 8 THEN POKE 16384+N,PEEK(A):N=N+1:T=0
220 T=T+1:IF T<1500 THEN 210
230 RETURN
BAS
}
mktap() {   # $1 = nom, stdin = lignes propres au scénario
    { cat; common; } > "$TMP/$1.bas"
    ./bas2tap "$TMP/$1.bas" -o "$WWW/$1.tap" --auto-run >/dev/null || { echo "FAIL: bas2tap $1"; exit 1; }
}
mktap relay <<BAS
10 A=796:POKE A+1,0:POKE A+3,30:POKE A+2,3:N=0
20 S\$="ATDT127.0.0.1:$TCP"+CHR\$(13):GOSUB 100
30 GOSUB 200
40 S\$="PING-ORIC"+CHR\$(13):GOSUB 100:GOTO 30
BAS
mktap fetch <<BAS
10 A=796:POKE A+1,0:POKE A+3,30:POKE A+2,3:N=0
20 S\$="ATGET http://127.0.0.1:$FETCH/x"+CHR\$(13):GOSUB 100
30 GOSUB 200
40 S\$="ATRD"+CHR\$(13):GOSUB 100:GOSUB 200:END
BAS
mktap same <<'BAS'
10 A=896:POKE A+1,0:POKE A+3,30:POKE A+2,3:N=0
20 S$="ATD-prophet.example:8998"+CHR$(13):GOSUB 100
30 GOSUB 200:L$=CHR$(13)+CHR$(10)
40 S$="GET /echo HTTP/1.1"+L$+"Host: prophet.example"+L$:GOSUB 100
50 S$="ResponseFormat: cli"+L$+"Connection: close"+L$+L$:GOSUB 100
60 GOSUB 200:END
BAS

# Serveur de la page : fichiers de $WWW + /echo (renvoie l'en-tête ResponseFormat).
python3 - "$HTTP" "$WWW" <<'PY' & PIDS+=($!)
import functools, http.server, sys
class H(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/echo"):
            body = ("FORMAT=%s\r\n" % self.headers.get("ResponseFormat", "(absent)")).encode()
            self.send_response(200); self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
        else:
            super().do_GET()
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])),
    functools.partial(H, directory=sys.argv[2])).serve_forever()
PY
# Relais WebSocket (scénario 1), restreint au serveur TCP de test.
python3 tools/picowifi_ws_relay.py --port "$RELAY" --allow "127.0.0.1:$TCP" 2>"$TMP/relay.log" & PIDS+=($!)
# Serveur TCP : bannière à la connexion, trace de tout ce qu'il reçoit.
python3 - "$TCP" "$RXLOG" <<'PY' & PIDS+=($!)
import socket, sys
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", int(sys.argv[1]))); srv.listen(4)
while True:
    c, _ = srv.accept()
    c.sendall(b"HELLO-RELAY\r\n")
    with open(sys.argv[2], "ab") as f:
        while True:
            d = c.recv(4096)
            if not d: break
            f.write(d); f.flush()
    c.close()
PY
# Serveur HTTP autre origine avec CORS (scénario 2, fetch() direct).
python3 - "$FETCH" <<'PY' & PIDS+=($!)
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"HELLO-FETCH\r\n"
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
for _ in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$HTTP/phosphoric.html" && break; sleep 0.1; done
sleep 0.3

node tests/integration/web_picowifi_e2e.js "http://127.0.0.1:$HTTP" "ws://127.0.0.1:$RELAY/" "$RXLOG"
rc=$?
[ $rc -eq 77 ] && skip "Playwright/Chrome indisponible"
if [ $rc -eq 0 ]; then echo "test-web-picowifi: OK"; else echo "test-web-picowifi: ÉCHEC"; sed 's/^/  relay: /' "$TMP/relay.log"; fi
exit $rc
