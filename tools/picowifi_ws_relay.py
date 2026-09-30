#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
"""picowifi_ws_relay.py — relais WebSocket -> TCP/TLS pour le modem picowifi web.

Un navigateur ne peut pas ouvrir de socket TCP. La build WebAssembly de
Phosphoric ouvre donc chaque connexion du modem picowifi (ATDT, ATGET,
ATRD/ATRT, ATDISKRD…) en WebSocket vers ce relais, qui ouvre le vrai TCP
côté hôte et recopie les octets dans les deux sens :

    ws://127.0.0.1:8766/?host=HOTE&port=PORT&tls=0|1

Avec tls=1, le relais termine le TLS (vérification de certificat par défaut
du système, SNI = HOTE) : le modem web échange du clair, comme le firmware
v0.2.0 qui déchiffre pour l'Oric.

Bibliothèque standard uniquement (asyncio, ssl, hashlib, base64).

Sécurité : c'est un proxy TCP ouvert. Il écoute sur 127.0.0.1 par défaut ;
--allow restreint les destinations (hôte ou hôte:port, répétable) et
--origin restreint les pages autorisées (en-tête Origin).

Usage :
    python3 tools/picowifi_ws_relay.py [--bind 127.0.0.1] [--port 8766]
                                       [--allow HOTE[:PORT]]... [--origin URL]...
"""
import argparse
import asyncio
import base64
import hashlib
import ssl
import struct
import sys
from urllib.parse import parse_qs, urlsplit

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_CONT, OP_TEXT, OP_BIN, OP_CLOSE, OP_PING, OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA
MAX_FRAME = 1 << 20


def log(*a):
    print("[relay]", *a, file=sys.stderr, flush=True)


async def http_error(writer, code, reason):
    writer.write(f"HTTP/1.1 {code} {reason}\r\nContent-Length: 0\r\n"
                 f"Connection: close\r\n\r\n".encode())
    try:
        await writer.drain()
    finally:
        writer.close()


def ws_frame(opcode, payload=b""):
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", 0x80 | opcode, n)
    elif n < 65536:
        head = struct.pack("!BBH", 0x80 | opcode, 126, n)
    else:
        head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
    return head + payload


async def ws_read_frame(reader):
    """Lit une trame client (masquée). Retourne (fin, opcode, payload)."""
    b0, b1 = await reader.readexactly(2)
    fin, opcode = b0 & 0x80, b0 & 0x0F
    masked, n = b1 & 0x80, b1 & 0x7F
    if n == 126:
        (n,) = struct.unpack("!H", await reader.readexactly(2))
    elif n == 127:
        (n,) = struct.unpack("!Q", await reader.readexactly(8))
    if n > MAX_FRAME:
        raise ValueError("trame trop grande")
    if not masked:
        raise ValueError("trame client non masquée")
    mask = await reader.readexactly(4)
    data = bytearray(await reader.readexactly(n))
    for i in range(n):
        data[i] ^= mask[i & 3]
    return fin, opcode, bytes(data)


def allowed(dest_host, dest_port, allow):
    if not allow:
        return True
    return dest_host in allow or f"{dest_host}:{dest_port}" in allow


async def handle(reader, writer, args):
    peer = writer.get_extra_info("peername")
    try:
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
    except Exception:
        writer.close()
        return
    lines = head.decode("latin-1").split("\r\n")
    try:
        method, target, _ = lines[0].split(" ", 2)
    except ValueError:
        return await http_error(writer, 400, "Bad Request")
    hdr = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            hdr[k.strip().lower()] = v.strip()
    if method != "GET" or hdr.get("upgrade", "").lower() != "websocket" \
            or "sec-websocket-key" not in hdr:
        return await http_error(writer, 400, "Bad Request")
    if args.origin and hdr.get("origin") not in args.origin:
        log(peer, "origine refusée :", hdr.get("origin"))
        return await http_error(writer, 403, "Forbidden")

    q = parse_qs(urlsplit(target).query)
    host = (q.get("host") or [""])[0]
    try:
        port = int((q.get("port") or ["0"])[0])
    except ValueError:
        port = 0
    use_tls = (q.get("tls") or ["0"])[0] == "1"
    if not host or not (0 < port < 65536):
        return await http_error(writer, 400, "Bad Request")
    if not allowed(host, port, args.allow):
        log(peer, "destination refusée :", f"{host}:{port}")
        return await http_error(writer, 403, "Forbidden")

    # Connexion TCP (et TLS) AVANT d'accepter le WebSocket : un échec devient
    # une réponse HTTP 502, que le navigateur voit comme un échec d'ouverture
    # (le modem répond alors NO CARRIER).
    try:
        ctx = ssl.create_default_context() if use_tls else None
        treader, twriter = await asyncio.wait_for(
            asyncio.open_connection(host, port, ssl=ctx,
                                    server_hostname=host if use_tls else None), 10)
    except Exception as e:
        log(peer, f"connexion {host}:{port}{' (TLS)' if use_tls else ''} impossible :", e)
        return await http_error(writer, 502, "Bad Gateway")

    accept = base64.b64encode(hashlib.sha1(
        (hdr["sec-websocket-key"] + WS_GUID).encode()).digest()).decode()
    writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  f"Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n").encode())
    await writer.drain()
    log(peer, f"-> {host}:{port}{' (TLS)' if use_tls else ''} ouvert")

    async def tcp_to_ws():
        try:
            while True:
                data = await treader.read(4096)
                if not data:
                    break
                writer.write(ws_frame(OP_BIN, data))
                await writer.drain()
        except Exception:
            pass
        try:
            writer.write(ws_frame(OP_CLOSE, struct.pack("!H", 1000)))
            await writer.drain()
        except Exception:
            pass

    async def ws_to_tcp():
        try:
            while True:
                _fin, op, data = await ws_read_frame(reader)
                if op in (OP_BIN, OP_TEXT, OP_CONT):
                    twriter.write(data)
                    await twriter.drain()
                elif op == OP_PING:
                    writer.write(ws_frame(OP_PONG, data))
                    await writer.drain()
                elif op == OP_CLOSE:
                    break
        except Exception:
            pass

    t1 = asyncio.ensure_future(tcp_to_ws())
    t2 = asyncio.ensure_future(ws_to_tcp())
    await asyncio.wait([t1, t2], return_when=asyncio.FIRST_COMPLETED)
    for t in (t1, t2):
        t.cancel()
    for w in (twriter, writer):
        try:
            w.close()
        except Exception:
            pass
    log(peer, f"-> {host}:{port} fermé")


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8766)
    ap.add_argument("--allow", action="append", default=[],
                    help="destination autorisée HOTE ou HOTE:PORT (répétable ; défaut : toutes)")
    ap.add_argument("--origin", action="append", default=[],
                    help="Origin autorisée, ex. http://localhost:8000 (répétable ; défaut : toutes)")
    args = ap.parse_args()
    srv = await asyncio.start_server(lambda r, w: handle(r, w, args), args.bind, args.port)
    log(f"écoute ws://{args.bind}:{args.port}/  (?host=&port=&tls=0|1)")
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
