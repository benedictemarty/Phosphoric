# Phosphoric v1.15.1 — ACIA fixes for OricTel

Hello,

Following your feedback on the ACIA 6551 timing problems encountered with OricTel, we have implemented several fixes in Phosphoric. Here is the summary.

---

## New: Digitelec DTL 2000 backend (recommended)

The biggest change: a modem backend that emulates the behaviour of the Digitelec DTL 2000. The ACIA 6551 stays 100% faithful to the MOS datasheet, and it is the modem that handles buffering and flow control — as on the real hardware.

```bash
./oric1-emu -r roms/basic10.rom -t orictel.tap -f \
  --serial digitelec:minitel.3614.fr:516
```

What the backend does:
- **512-byte internal buffer** (the real DTL 2000 had its own RAM) → no more overruns during clear screen or Vidéotex REP commands
- **Automatic CTS flow control**: the modem drops CTS when its buffer exceeds 400 bytes and raises it again below 256 → the remote server is naturally throttled
- **DCD driven by the TCP connection**: PEEK($031D) bit 5 = carrier state
- **DTR to connect/disconnect**: POKE $031E,3 = go off-hook (TCP connect), POKE $031E,0 = hang up
- **V23 enabled automatically** (1200 RX / 75 TX)
- **No AT commands** (faithful to the Digitelec, which was not Hayes-compatible)

On the ORIC program side, connecting is simple:
```basic
10 POKE 798,3:REM DTR ON = DECROCHER
20 S=PEEK(797):IF (S AND 32)=32 THEN 20:REM ATTENDRE DCD
30 REM ... CONNECTE, LIRE/ECRIRE SUR 796 ...
40 POKE 798,0:REM DTR OFF = RACCROCHER
```
(The BASIC REM comments mean: DTR ON = go off-hook; wait for DCD; connected, read/write at 796; DTR OFF = hang up.)

---

## Alternative: ACIA patches (if you prefer to keep your current architecture)

If you do not want to change backend, the 3 problems you had identified are also fixed through CLI options:

### 1. Overrun (1-byte buffer)

```bash
--serial-buffer 256
```

Adds a transparent 256-byte RX FIFO inside the ACIA. When the CPU reads RDR ($031C), the next byte is automatically loaded from the queue. RDRF stays at 1 as long as the queue is not empty. Not faithful to the MOS 6551 but functional.

### 2. IRQ lost with simultaneous TX+RX

```bash
--serial-irq-on-rdrf
```

WDC 65C51 mode: the IRQ re-fires as long as RDRF is set, even after the Status Register has been read. Solves the problem where reading $031D to check TDRE clears the IRQ of a pending RX byte.

### 3. First-byte timing

Fixed by default (no option needed). `tx_cycles` and `rx_cycles` are now initialised to `tx_reload`/`rx_reload` instead of 0. Changing the baud rate also resynchronises the counters.

---

## Full command line (with ACIA patches)

```bash
# Option A: Digitelec backend (recommended, solves everything naturally)
./oric1-emu -r roms/basic10.rom -t orictel.tap -f \
  --serial digitelec:minitel.3614.fr:516

# Option B: direct TCP + ACIA patches (your current architecture)
./oric1-emu -r roms/basic10.rom -t orictel.tap -f \
  --serial tcp:minitel.3614.fr:516 \
  --serial-v23 \
  --serial-buffer 256 \
  --serial-irq-on-rdrf
```

Option A is recommended because it solves the timing problems architecturally (the buffer is in the modem, not in the ACIA).

---

## Other ACIA improvements in this release

- ACIA clock fixed: 1.8432 MHz crystal / 16 = 115200 Hz (was approximated to 1 MHz)
- Variable frame format: 5-8 data bits + parity + 1-2 stop bits (was fixed at 10)
- Bitmask applied on TX/RX (7-bit mode masks bit 7)
- DCD transition generates an IRQ (as per the datasheet)
- Savestate: ACIA state saved in .ost files (SER section)
- Configurable ACIA offset: --acia-addr (default $031C)
- bas2tap fixed: BASIC tokenizer with a 120-token table extracted from the ROM

## Build

```bash
git clone https://git.nagominosato.fr:6775/chipinette/Phosphoric.git
cd Phosphoric
make SDL2=1
```

303 unit tests, 100% pass.

---

Do not hesitate to get in touch if you have questions or run into other timing problems.

bmarty
