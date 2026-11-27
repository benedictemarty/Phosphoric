#!/usr/bin/env python3
"""LOCI ACIA flow control and pacing with the picowifi modem (end to end, no system ROM).

A tiny ROM built here sends "AT?" to the modem through the ACIA at $0380, then
reads the answer about five times slower than the 9600-baud line and stores it
at $4000. On the real LOCI the firmware only hands over the next byte once the
6502 has read the previous one, so nothing is lost under --loci. The same ROM on
a plain 6551 (no --loci) must still lose bytes: the check discriminates.

A second ROM reads as fast as it can. Early in the run, the whole answer is
already there under --loci (the firmware does not time bytes on the baud rate),
but not with --loci-acia-pacing, which paces the ACIA at 9600 baud again.

On Linux, a pseudo-terminal then plays a real modem (--serial com:): it answers
at once, and the slow ROM must receive everything under --loci only.

Usage:  test_loci_acia_flow.py ./phosphoric
Author: bmarty <bmarty@mailo.com>
"""
import os
import select
import subprocess
import sys
import tempfile
import threading

BUF = 0x4000
HELP = b"PicoWiFiModemUSB AT set: $SSID $PASS $CV $CA C D NET S0 S2 E Q V X &Z &V I"


def build_rom(slow=True):
    """16 KB ROM at $C000: send "AT?\\r", then polled reception (slow or not)."""
    code = bytearray()

    def op(*b):
        code.extend(b)

    def rel(opcode, target):
        op(opcode, (target - (len(code) + 2)) & 0xFF)

    op(0x78, 0xD8, 0xA2, 0xFF, 0x9A)        # SEI : CLD : LDX #$FF : TXS
    op(0xA9, 0x00, 0x8D, 0x81, 0x03)        # programmed reset
    op(0xA9, 0x1E, 0x8D, 0x83, 0x03)        # 9600 8N1
    op(0xA9, 0x0B, 0x8D, 0x82, 0x03)        # DTR on, no IRQ
    op(0xA0, 0x00)                          # LDY #0
    send = len(code)
    op(0xAD, 0x81, 0x03, 0x29, 0x10)        # wait TDRE
    rel(0xF0, send)
    cmd_operand = len(code) + 1
    op(0xB9, 0x00, 0x00, 0x8D, 0x80, 0x03)  # LDA cmd,Y : STA $0380
    op(0xC8, 0xC0, 0x04)                    # INY : CPY #4
    rel(0xD0, send)
    op(0xA9, BUF & 0xFF, 0x85, 0x10, 0xA9, BUF >> 8, 0x85, 0x11, 0xA0, 0x00)
    read = len(code)
    if slow:
        op(0xA2, 0x04, 0x86, 0x12, 0xA2, 0x00)  # ~5100 cycles between two reads
        delay = len(code)
        op(0xCA)                            # DEX
        rel(0xD0, delay)
        op(0xC6, 0x12)                      # DEC $12
        rel(0xD0, delay)
    op(0xAD, 0x81, 0x03, 0x29, 0x08)        # RDRF ?
    rel(0xF0, read)
    op(0xAD, 0x80, 0x03, 0x91, 0x10)        # LDA $0380 : STA ($10),Y
    op(0xE6, 0x10)                          # INC $10
    rel(0xD0, read)
    op(0xE6, 0x11)                          # INC $11
    op(0x4C, read & 0xFF, 0xC0 + (read >> 8))

    cmd = 0xC000 + len(code)
    code[cmd_operand] = cmd & 0xFF
    code[cmd_operand + 1] = cmd >> 8
    code.extend(b"AT?\r")

    rom = bytearray([0xEA]) * 0x4000
    rom[:len(code)] = code
    for vector in (0x3FFA, 0x3FFC, 0x3FFE):
        rom[vector:vector + 2] = bytes((0x00, 0xC0))
    return bytes(rom)


def play_modem(master, stop, answer):
    """Answer once on the pseudo-terminal, as soon as the command is complete."""
    got = b""
    while not stop.is_set():
        if select.select([master], [], [], 0.02)[0]:
            got += os.read(master, 64)
            if got.endswith(b"\r"):
                os.write(master, answer)
                return


def received_com(emu, tmp, extra, answer):
    """Same as received(), the modem being a real port (a pseudo-terminal)."""
    master, slave = os.openpty()
    stop = threading.Event()
    modem = threading.Thread(target=play_modem, args=(master, stop, answer))
    modem.start()
    try:
        port = "com:115200,8,N,1," + os.ttyname(slave)
        return received(emu, tmp, [*extra, "--realtime"], serial=port)
    finally:
        stop.set()
        modem.join()
        os.close(master)
        os.close(slave)


def received(emu, tmp, extra, rom="acia.rom", cycles=3000000, serial="picowifi"):
    rom = os.path.join(tmp, rom)
    ram = os.path.join(tmp, "ram.bin")
    if os.path.exists(ram):
        os.unlink(ram)
    env = dict(os.environ, PHOSPHORIC_PICOWIFI_NVRAM=os.path.join(tmp, "nvram"))
    subprocess.run([emu, "-r", rom, "--no-config-cards", *extra,
                    "--serial", serial, "-n", "-c", str(cycles),
                    "--dump-ram-at", "%d:%s" % (cycles - 5000, ram)],
                   env=env, check=True, timeout=60,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(ram, "rb") as f:
        data = f.read()
    return data[BUF:BUF + 0x400].split(b"\0")[0]


def main():
    emu = os.path.abspath(sys.argv[1])
    if not os.access(emu, os.X_OK):
        print("SKIP : émulateur non construit")
        return 0
    print("=== Contrôle de flux et cadence de l'ACIA LOCI avec le picowifi ===")
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, slow in (("acia.rom", True), ("fast.rom", False)):
            with open(os.path.join(tmp, name), "wb") as f:
                f.write(build_rom(slow))
        opts = ["--loci", "--loci-flash", tmp]
        loci = received(emu, tmp, opts)
        plain = received(emu, tmp, ["--acia-addr", "0380"])
        slow_paced = received(emu, tmp, opts + ["--loci-acia-pacing"])
        fast = received(emu, tmp, opts, "fast.rom", 30000)
        fast_paced = received(emu, tmp, opts + ["--loci-acia-pacing"], "fast.rom", 30000)
        real = None
        if sys.platform.startswith("linux"):
            answer = HELP + b"\r\nOK\r\n"
            real = (answer, received_com(emu, tmp, opts, answer),
                    received_com(emu, tmp, ["--acia-addr", "0380"], answer))

    checks = [
        ("--loci : réponse complète (aide puis OK)",
         HELP in loci and loci.endswith(b"OK\r\n")),
        ("6551 seule : des octets sont perdus",
         0 < len(plain) < len(loci)),
        ("--loci-acia-pacing, lecteur lent : réponse complète", slow_paced == loci),
        ("--loci : sans cadence, tout est reçu en 25 000 cycles", fast == loci),
        ("--loci-acia-pacing : cadencé à 9600 bauds, réponse partielle",
         0 < len(fast_paced) < len(loci) // 2 and loci.startswith(fast_paced)),
    ]
    if real:
        answer, com_loci, com_plain = real
        checks += [
            ("--loci, modem réel (com:) : réponse complète", com_loci == answer),
            ("6551 seule, com: : des octets sont perdus", 0 < len(com_plain) < len(answer)),
        ]
    else:
        print("  (modem réel par pseudo-terminal : vérifié sous Linux seulement)")
    for name, good in checks:
        print("  [%s] %s" % ("OK" if good else "FAIL", name))
        fails += not good
    if fails:
        print("  --loci  : %d octets %r" % (len(loci), loci))
        print("  sans    : %d octets %r" % (len(plain), plain))
        for label, data in (("lent cadencé", slow_paced), ("rapide", fast),
                            ("rapide cadencé", fast_paced)):
            print("  %s : %d octets %r" % (label, len(data), data))
        for data in (real or ())[1:]:
            print("  com: : %d octets %r" % (len(data), data))
    print("=== %d réussi(s), %d échec(s) ===" % (len(checks) - fails, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
