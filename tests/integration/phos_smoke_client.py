#!/usr/bin/env python3
"""
phos_smoke_client.py — minimal Python client for the Phosphoric --control
IPC protocol (sprint 35a/b/c).

Built as the 6th E2E scenario in tests/integration/. Mirrors the public
shape of OricForge's phos_client.py (handshake, async EVT thread,
synchronous REP queueing) on a leaner code budget so it can live in the
emulator's own regression suite without a third-party dependency.

Usage as a standalone script:
    ./tests/integration/phos_smoke_client.py path/to/oric1-emu \\
        -r roms/basic11b.rom

Usage as a module (for assertions in shell tests):
    from phos_smoke_client import PhosClient
    with PhosClient.spawn(["./oric1-emu", "-r", "roms/basic11b.rom"]) as c:
        c.wait_ready()
        regs = c.regs()
        assert regs["PC"] == "F88F"

Python 3.8+, stdlib only.
"""

from __future__ import annotations

import os
import queue
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import contextmanager
from typing import Callable, Dict, Iterator, List, Optional


class ProtocolError(RuntimeError):
    pass


class PhosClient:
    """One synchronous request channel + one async event channel.

    The reader thread tags every incoming line as REP or EVT and pushes
    them on separate queues; the main thread blocks on the REP queue for
    request/response, and either polls or registers callbacks for EVT.
    """

    REQ_TIMEOUT = 5.0   # seconds to wait for a REP after sending a CMD

    def __init__(self, proc: subprocess.Popen, stderr_file=None):
        self.proc = proc
        # Emulator stderr, kept to explain a crash (see diagnostic()).
        self._stderr_file = stderr_file
        # A `quit` already went out (cmd("quit")): close() must not send a
        # second one into a pipe the exiting emulator has already closed.
        self._quit_sent = False
        self._reps: "queue.Queue[str]" = queue.Queue()
        self._evts: "queue.Queue[str]" = queue.Queue()
        self._evt_handlers: List[Callable[[str], None]] = []
        self._reader_stop = threading.Event()
        # Sprint 35c — `bread` flips this to True so the reader exits
        # after delivering exactly the next REP line, releasing the pipe
        # so the main thread can siphon the binary payload.
        self._reader_yield_after_next = threading.Event()
        self._start_reader()

    def _start_reader(self):
        self._reader_stop.clear()
        self._reader_yield_after_next.clear()
        self._reader = threading.Thread(
            target=self._reader_loop, name="phos-reader", daemon=True
        )
        self._reader.start()

    # ── lifecycle ──────────────────────────────────────────────────────
    @classmethod
    def spawn(cls, argv: List[str], extra_args: Optional[List[str]] = None) -> "PhosClient":
        """Spawn the emulator with --control appended and return a connected client."""
        full_argv = list(argv)
        if "--control" not in full_argv:
            full_argv.append("--control")
        if extra_args:
            full_argv.extend(extra_args)
        err = tempfile.TemporaryFile()
        proc = subprocess.Popen(
            full_argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=err,
            bufsize=0,                 # raw, we framing per line
            text=False,                # bytes so bread (binary) is clean
        )
        return cls(proc, err)

    def diagnostic(self, lines: int = 15) -> str:
        """Exit code and last stderr lines of the emulator (crash report)."""
        rc = self.proc.poll()
        tail = ""
        if self._stderr_file is not None:
            try:
                self._stderr_file.seek(0)
                tail = "\n".join(self._stderr_file.read().decode("utf-8", "replace")
                                 .splitlines()[-lines:])
            except (OSError, ValueError):
                pass
        return f"emulator exit code: {rc}\n{tail}"


    def close(self):
        try:
            if self.proc.poll() is None:
                if not self._quit_sent:
                    try:
                        self._send_raw(b"quit\n")
                        self._quit_sent = True
                    except (BrokenPipeError, OSError, ProtocolError):
                        pass   # died meanwhile: keep the original error visible
                try:
                    self.proc.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    # Still alive: stop it and reap it, so no emulator outlives
                    # the test (the next one would race with it).
                    self.proc.terminate()
                    try:
                        self.proc.wait(timeout=5.0)
                    except subprocess.TimeoutExpired:
                        self.proc.kill()
                        self.proc.wait()
        finally:
            self._reader_stop.set()
            if self.proc.stdin and not self.proc.stdin.closed:
                self.proc.stdin.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        # A failure while the emulator is dead: say why it died.
        if exc_type is not None and self.proc.poll() is not None:
            print("--- emulator died ---\n" + self.diagnostic(), file=sys.stderr)
        if self._stderr_file is not None:
            self._stderr_file.close()

    # ── reader thread ──────────────────────────────────────────────────
    def _reader_loop(self):
        out = self.proc.stdout
        if not out:
            return
        while not self._reader_stop.is_set():
            try:
                line = out.readline()
            except (ValueError, OSError):
                return
            if not line:
                return
            decoded = line.decode("utf-8", errors="replace").rstrip("\r\n")
            if decoded.startswith("EVT "):
                self._evts.put(decoded)
                for h in list(self._evt_handlers):
                    try:
                        h(decoded)
                    except Exception:
                        pass
            else:
                self._reps.put(decoded)
                if self._reader_yield_after_next.is_set():
                    # bread is in progress and just got its OK — give the
                    # pipe back to the main thread.
                    return

    def on_event(self, handler: Callable[[str], None]):
        self._evt_handlers.append(handler)

    # ── event helpers ──────────────────────────────────────────────────
    def wait_event(self, kind: str, timeout: float = 5.0) -> Optional[str]:
        """Block until an EVT line whose first token after `EVT ` matches `kind`."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                evt = self._evts.get(timeout=deadline - time.time())
            except queue.Empty:
                return None
            if evt.startswith(f"EVT {kind}"):
                return evt
        return None

    def wait_ready(self, timeout: float = 5.0) -> str:
        evt = self.wait_event("ready", timeout=timeout)
        if evt is None:
            raise ProtocolError("no EVT ready within timeout")
        # The server emits EVT ready THEN EVT stopped — consume the
        # initial stopped so the caller starts on a clean slate.
        self.wait_event("stopped", timeout=timeout)
        return evt

    def wait_stopped(self, timeout: float = 5.0) -> Optional[str]:
        return self.wait_event("stopped", timeout=timeout)

    # ── request/reply ──────────────────────────────────────────────────
    def _send_raw(self, data: bytes):
        if not self.proc.stdin or self.proc.stdin.closed:
            raise ProtocolError("stdin closed")
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def cmd(self, line: str, timeout: Optional[float] = None) -> str:
        """Send a command and return the next REP line (OK or ERR)."""
        timeout = timeout if timeout is not None else self.REQ_TIMEOUT
        self._send_raw((line + "\n").encode("utf-8"))
        if line.strip() == "quit":
            self._quit_sent = True
        try:
            return self._reps.get(timeout=timeout)
        except queue.Empty:
            raise ProtocolError(f"no REP within {timeout}s for `{line}`")

    def ok(self, line: str, timeout: Optional[float] = None) -> str:
        rep = self.cmd(line, timeout=timeout)
        if not rep.startswith("OK"):
            raise ProtocolError(f"expected OK, got `{rep}` for `{line}`")
        return rep[3:].strip() if len(rep) > 2 else ""

    # ── high-level wrappers ────────────────────────────────────────────
    def hello(self) -> Dict[str, str]:
        body = self.ok("hello client=phos_smoke_client/0.1 proto=1")
        return self._parse_kv(body)

    def regs(self) -> Dict[str, str]:
        return self._parse_kv(self.ok("regs"))

    def read(self, addr: int, length: int) -> List[int]:
        # Lengths are parsed as hex by the server, so format explicitly
        # with the `$` prefix to avoid the decimal-vs-hex ambiguity.
        body = self.ok(f"read ${addr:04X} ${length:X}")
        return [int(b, 16) for b in body.split()]

    def write(self, addr: int, data: bytes) -> int:
        body = self.ok(f"write ${addr:04X} " + " ".join(f"{b:02X}" for b in data))
        return int(self._parse_kv(body)["count"])

    def set_break(self, addr: int) -> int:
        body = self.ok(f"break ${addr:04X}")
        return int(self._parse_kv(body)["id"])

    def step(self) -> str:
        self.ok("step")
        evt = self.wait_stopped()
        if evt is None:
            raise ProtocolError("no EVT stopped after step")
        return evt

    def cont(self):
        self.ok("continue")

    def pause(self) -> str:
        self.ok("pause")
        evt = self.wait_stopped()
        if evt is None:
            raise ProtocolError("no EVT stopped after pause")
        return evt

    def peek(self, subsystem: str) -> Dict[str, str]:
        return self._parse_kv(self.ok(f"peek {subsystem}"))

    def bread(self, addr: int, length: int) -> bytes:
        """Length-prefixed binary read (sprint 35c).

        Wire :
            > bread $XXXX $LEN\n
            OK bread len=$LEN\n          (consumed by reader, then yields)
            <LEN raw bytes>              (consumed by main, raw)
            \n                            (consumed by main, framing)
        """
        # Tell the reader to exit after the next REP it delivers.
        self._reader_yield_after_next.set()
        self._send_raw(f"bread ${addr:04X} ${length:X}\n".encode("utf-8"))
        try:
            rep = self._reps.get(timeout=self.REQ_TIMEOUT)
        except queue.Empty:
            raise ProtocolError("no REP for bread within timeout")
        if not rep.startswith("OK"):
            raise ProtocolError(f"bread failed: `{rep}`")
        meta = self._parse_kv(rep[3:].strip())
        n = int(meta["len"])
        if n != length:
            raise ProtocolError(f"bread len mismatch: got {n}, expected {length}")

        # Reader exits after delivering OK; wait for it then own the pipe.
        self._reader.join(timeout=2.0)
        if self._reader.is_alive():
            raise ProtocolError("reader did not yield the pipe for bread")

        out = self.proc.stdout
        if not out:
            raise ProtocolError("stdout closed mid-bread")
        chunks: List[bytes] = []
        remaining = n
        while remaining > 0:
            c = out.read(remaining)
            if not c:
                raise ProtocolError("EOF during bread payload")
            chunks.append(c)
            remaining -= len(c)
        nl = out.read(1)
        if nl != b"\n":
            raise ProtocolError(f"bread missing trailing newline (got {nl!r})")

        # Resume background reader.
        self._start_reader()
        return b"".join(chunks)

    # ── parsing helper ────────────────────────────────────────────────
    @staticmethod
    def _parse_kv(body: str) -> Dict[str, str]:
        out: Dict[str, str] = {}
        for token in body.split():
            if "=" in token:
                k, v = token.split("=", 1)
                out[k] = v
        return out


# ─── standalone demo / smoke test ─────────────────────────────────────
def main(argv: List[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 1
    emu_argv = argv[1:]
    with PhosClient.spawn(emu_argv) as c:
        c.wait_ready()
        print("ready ✓")

        hello = c.hello()
        print(f"hello → server={hello.get('server')} "
              f"proto={hello.get('proto')} caps={hello.get('caps')}")

        regs = c.regs()
        assert regs["PC"] == "F88F", f"unexpected boot PC: {regs}"
        print(f"regs ✓ PC={regs['PC']} A={regs['A']} cycles={regs['cycles']}")

        mem = c.read(0xF88F, 4)
        assert mem == [0xA2, 0xFF, 0x9A, 0x58], f"unexpected mem: {mem}"
        print(f"read ✓ {[f'{b:02X}' for b in mem]}")

        bp = c.set_break(0xF891)
        print(f"break ✓ id={bp}")

        c.cont()
        evt = c.wait_stopped()
        if not evt or "reason=break" not in evt:
            print(f"continue/stopped FAIL: {evt}")
            return 1
        print(f"continue ✓ {evt}")

        via = c.peek("via")
        assert "ifr" in via, f"missing 'ifr' in peek via: {via}"
        print(f"peek via ✓ ifr={via.get('ifr')}")

        # Step + verify reason=step
        evt2 = c.step()
        if "reason=step" not in evt2:
            print(f"step FAIL: {evt2}")
            return 1
        print(f"step ✓ {evt2}")

        # bread cross-check : same window via hex read and binary bread.
        hex_buf = bytes(c.read(0xF88F, 64))
        bin_buf = c.bread(0xF88F, 64)
        if hex_buf != bin_buf:
            print(f"bread mismatch:\n  hex={hex_buf.hex()}\n  bin={bin_buf.hex()}")
            return 1
        print(f"bread ✓ 64 bytes match hex read")

    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
