# Phosphoric — Serial link technical guide for OricTel

## Architecture

```
ORIC program (BASIC/ASM)
        │
        │ POKE/PEEK $031C-$031F
        ▼
┌─────────────────────┐
│   ACIA 6551         │  Faithful MOS 6551 emulation
│   4 I/O registers   │  1.8432 MHz crystal / 16
│   $031C = DATA      │  Variable frame format (5-8 bits + parity + stop)
│   $031D = STATUS    │  Level-triggered TX/RX IRQ
│   $031E = COMMAND   │
│   $031F = CONTROL   │
└────────┬────────────┘
         │ send()/recv()/poll()
         ▼
┌─────────────────────┐
│   Serial backend    │  vtable abstraction (6 implementations)
│   (interchangeable) │
└────────┬────────────┘
         │
         ▼
    TCP / PTY / COM / etc.
```

## The 4 ACIA registers

### $031C — DATA (POKE 796 / PEEK 796)

- **Write**: puts a byte into the Transmitter Data Register (TDR)
  - The TDRE bit (status bit 4) goes to 0
  - The byte is transmitted after `tx_reload` CPU cycles
  - The `bitmask` is applied (7-bit mode → bit 7 forced to 0)

- **Read**: reads the Receiver Data Register (RDR)
  - Clears the RDRF, OVRN, PE, FE flags
  - If the FIFO is enabled, automatically loads the next byte from the queue

### $031D — STATUS (PEEK 797) / RESET (POKE 797)

- **Read**: returns the current state

  ```
  Bit 7 : IRQ (1=IRQ active) — cleared by the read
  Bit 6 : DSR (1=DSR inactive, 0=DSR active)
  Bit 5 : DCD (1=no carrier, 0=carrier detected)
  Bit 4 : TDRE (1=TX register empty, ready to send)
  Bit 3 : RDRF (1=received data available)
  Bit 2 : OVRN (1=overrun, previous data not read)
  Bit 1 : FE (framing error)
  Bit 0 : PE (parity error)
  ```

  **WARNING**: reading the status clears the IRQ bit! This is the real MOS 6551 behaviour. If you use IRQs with simultaneous TX+RX, enable `--serial-irq-on-rdrf` (WDC 65C51 mode) so that the IRQ re-fires as long as RDRF is set.

- **Write**: programmed reset (any value)
  - Sets TDRE=1, clears overrun
  - Clears bits 0-4 of the Command Register (DTR off, IRQ off)
  - Preserves bits 5-7 of the Command Register (parity)
  - Does NOT touch the Control Register (baud rate kept)

### $031E — COMMAND (POKE 798 / PEEK 798)

```
Bit 0 : DTR (1=Data Terminal Ready — enables the modem)
Bit 1 : IRD (1=receive IRQ DISABLED, 0=RX IRQ enabled)
Bits 3-2 : TIC (transmit IRQ control)
  00 = RTS high, TX IRQ off
  01 = RTS low, TX IRQ on
  10 = RTS low, TX IRQ off
  11 = RTS low, break on line
Bit 4 : ECHO (1=echo mode, sends back the received bytes)
Bit 5 : PME (1=parity enabled)
Bits 7-6 : PMC (parity type)
```

**Recommended value: `POKE 798,3`** (DTR on + RX IRQ disabled)

If you use IRQs, `POKE 798,1` enables DTR + RX IRQ. But WARNING: every received byte generates an IRQ. Without an ASM handler, the CPU enters an infinite IRQ loop because the ROM does not handle the ACIA.

### $031F — CONTROL (POKE 799 / PEEK 799)

```
Bits 3-0 : Baud Rate
  0=$EXT  1=50   2=75    3=110   4=135  5=150
  6=300   7=600  8=1200  9=1800  A=2400 B=3600
  C=4800  D=7200 E=9600  F=19200
Bit 4 : RX Clock Source (0=external, 1=internal BRG)
Bits 6-5 : Word Length (00=8bit, 01=7bit, 10=6bit, 11=5bit)
Bit 7 : Stop Bits (0=1 stop, 1=2 stop)
```

**Recommended value: `POKE 799,31`** ($1F = 19200 baud, 8-N-1, internal clock)

For 1200 baud 8-N-1: `POKE 799,24` ($18)

## Minimal initialisation sequence

```basic
10 DA=796:ST=797:CM=798:CT=799
20 POKE ST,0          : REM Reset ACIA
30 POKE CT,31         : REM 19200 baud, 8-N-1
40 POKE CM,3          : REM DTR on, IRQ RX off
```

## Sending a byte

```basic
100 REM Wait until TX is free
110 IF (PEEK(ST) AND 16)=0 THEN 110
120 POKE DA, octet
```

## Receiving a byte

```basic
200 REM Check whether a byte is available
210 IF (PEEK(ST) AND 8)=0 THEN GOTO 200
220 C=PEEK(DA)
```

## Checking the carrier (DCD)

```basic
300 S=PEEK(ST)
310 IF (S AND 32)=32 THEN PRINT"PAS DE PORTEUSE"
320 IF (S AND 32)=0 THEN PRINT"CONNECTE"
```

(The program prints "PAS DE PORTEUSE" = "no carrier" and "CONNECTE" = "connected".)

## Available backends

### 1. Loopback (test)

```bash
./oric1-emu -r roms/basic10.rom --serial loopback
```

TX comes straight back as RX. 256-byte circular buffer. Useful to check that the ACIA works without a network.

### 2. Raw TCP

```bash
./oric1-emu -r roms/basic10.rom --serial tcp:bbs.host:23
```

Direct TCP connection. No AT commands, no modem flow control. Data are sent/received byte by byte on the socket. Connection established at startup.

### 3. Hayes modem (AT commands)

```bash
# Pure command mode — the ORIC program dials via ATD
./oric1-emu -r roms/basic10.rom --serial modem

# Predefined host — a bare ATD connects here
./oric1-emu -r roms/basic10.rom --serial modem:bbs.host:23

# Server mode — accepts incoming connections
./oric1-emu -r roms/basic10.rom --serial modem:listen:2323
```

Starts in **command mode**. The ACIA sees the AT responses as received data.

Supported commands:
- `AT` → `OK`
- `ATZ` → reset → `OK`
- `ATE0` / `ATE1` → echo off/on → `OK`
- `ATH` → hang up → `OK`
- `ATD host:port` → connect via TCP → `CONNECT` or `NO CARRIER`
- `ATDT host:port` → same (T ignored)
- `ATA` → accept connection (listen mode) → `CONNECT` or `ERROR`
- `ATS0=N` → auto-answer after N rings → `OK`
- `+++` → back to command mode (guard time) → `OK`

64 KB internal RX/TX buffers.

Complete BASIC example:
```basic
10 DA=796:ST=797:CM=798:CT=799
20 POKE ST,0:POKE CT,31:POKE CM,3
30 REM SEND ATD
40 A$="ATD pavi.3617.fr:3617"+CHR$(13)
50 FOR I=1 TO LEN(A$)
60 POKE DA,ASC(MID$(A$,I,1))
70 FOR W=1 TO 100:NEXT W
80 NEXT I
90 REM WAIT FOR RESPONSE
100 R$=""
110 S=PEEK(ST):IF (S AND 8)=0 THEN 110
120 C=PEEK(DA)
130 IF C=10 THEN 110
140 IF C=13 THEN PRINT R$:GOTO 160
150 R$=R$+CHR$(C):GOTO 110
160 IF R$="CONNECT" THEN PRINT"OK!":GOTO 200
170 PRINT"ECHEC":END
200 REM TERMINAL
210 S=PEEK(ST)
220 IF (S AND 8)=8 THEN C=PEEK(DA):IF C>31 AND C<127 THEN PRINT CHR$(C);
230 K$=KEY$:IF K$="" THEN 210
240 IF ASC(K$)=27 THEN END
250 POKE DA,ASC(K$):GOTO 210
```

(`ECHEC` = "failure".)

### 4. Digitelec DTL 2000

```bash
./oric1-emu -r roms/basic10.rom --serial digitelec:minitel.host:516
```

Emulates the Digitelec DTL 2000 external modem (1984). No AT commands. Controlled through the RS232 lines:
- DTR on (POKE CM,3) → the modem connects via TCP
- DTR off (POKE CM,0) → the modem hangs up
- DCD in the status register → connection state
- Automatic CTS flow control (512-byte internal buffer)
- V23 1200/75 enabled automatically

### 5. PTY (pseudo-terminal)

```bash
./oric1-emu -r roms/basic10.rom --serial pty
```

Creates a pseudo-terminal. The log shows the device path:
```
Serial PTY: opened /dev/pts/3 (master fd=5)
```
Connect minicom/screen/picocom to this device from another terminal.

### 6. COM (real serial port)

```bash
./oric1-emu -r roms/basic10.rom --serial com:9600,8,N,1,/dev/ttyUSB0
```

Real serial port via termios (Linux). Format: `baud,databits,parity,stopbits,device`.

### 7. PicoWiFiModemUSB (LOCI WiFi modem)

```bash
# Under --loci, the ACIA is mapped at $0380 (LOCI firmware default)
./oric1-emu -r roms/basic10.rom --loci --serial picowifi:MonWiFi:motdepasse
```

Emulates sodiumlb's WiFi modem (a Pico W exposed by LOCI as an ACIA). The
ORIC program dials with AT commands. Full v0.1.0 command set: `AT$SSID=`,
`AT$PASS=`, `ATC1` (WiFi connection), `ATDT host:port` (TCP), `ATNET`,
speed-dial numbers `AT&Z`, S registers, etc. WiFi simulated, data = real TCP.

**⚠ Minitel / Videotex: force NET0.** Real telnet mode (NET1, the default)
translates `CR`→`CR+NUL` and doubles `IAC` (0xFF) — which **corrupts** a
Videotex stream (where these bytes are data). Dial in NET0 for a
transparent stream:

```basic
A$="ATDT-mon.serveur.minitel:516"+CHR$(13) : REM the '-' forces NET0
```

The prefix `-` (NET0), `=` (real NET1), `+` (fake NET2) sets the telnet mode
of the session. For pure Videotex, the `digitelec` backend + `--serial-v23`
remains the most idiomatic option (native V23 1200/75, no AT commands).

**Ready-to-use example**: `examples/picowifi_test.bas` talks to the
modem (ATI, AT$SSID, ATC1…) and displays the responses:

```bash
./oric1-emu -r roms/basic11b.rom \
  --acia-addr 0380 --serial picowifi:HomeNet --serial-buffer 512 \
  -t examples/picowifi_test.tap -f
```

⚠ `--serial-buffer N` matters: without an RX FIFO, BASIC display is
slower than the modem's data rate → OVERRUN and lost bytes (the ACIA has
only one RX register). The FIFO buffers the responses.

**Real network bridge.** Data connections (`ATDT`/`ATGET`/`ATRD`)
already go out as real TCP through the host's active interface (WiFi card
included). To make the *reported* WiFi state (IP, connectivity, SSID)
real as well — instead of simulated — export:

```bash
PHOSPHORIC_PICOWIFI_REALNET=1 ./oric1-emu ... --serial picowifi ...
```

`ATI` then shows the real local IP (`IP: 192.168.1.19 (host)`), `ATC?`
reflects the real internet connectivity, and the host's SSID is adopted at
boot. It is **read-only**: the emulator never drives the WiFi
card (no scan/association). Disabled by default (simulated mode).

## Improvement options

### RX FIFO (anti-overrun)

```bash
--serial-buffer 256
```

The real ACIA has a single RX byte. If the CPU has not read it before the next one arrives → overrun. The FIFO adds a transparent queue. When DATA ($031C) is read, the next byte is loaded automatically. RDRF stays at 1 as long as the queue is not empty.

**When to use it**: when the ORIC program performs long operations between reads (clear screen, scroll, Videotex display).

### WDC 65C51 IRQ

```bash
--serial-irq-on-rdrf
```

The MOS 6551 has a bug: reading the status ($031D) clears the IRQ bit. If the status is read to check TDRE (TX), the IRQ for a pending RX byte is lost. WDC 65C51 mode re-fires the IRQ as long as RDRF is set.

**When to use it**: when the program uses IRQs with simultaneous TX+RX.

### I2C transport cost of LOCI IRQs

```bash
--loci-irq-latency 10000
```

On the **real LOCI**, the ACIA's `/IRQ` line is not wired directly to the 6502: it goes through the **I2C** bus, an "extremely slow" transport (SodiumLB, defence-force forum p34982). Consequence: receiving data **by interrupt** caps out at ~100 bytes/s, whereas **polling** registers $0380–$0383 is "an order of magnitude faster". This is an artefact of the physical transport of the IRQ, not of the 6551.

By default the emulator asserts `/IRQ` with no transport cost (like a bare 6551), so IRQ there is as fast as polling. `--loci-irq-latency US` delays each **physical assertion** of `/IRQ` by `US` microseconds (at 1 MHz, 1 µs = 1 cycle); the logical decision of the status register is unchanged, so **polling is not penalised**. At ~10,000 µs/IRQ (10 ms), the handler runs only ~100×/s → RX capped at ~100 bytes/s with IRQs.

**When to use it**: to faithfully reproduce the IRQ ceiling of the real LOCI, to compare objectively **polling** reception (Command $0B, IRQ off) with an IRQ approach, or to test Oric software that uses the LOCI's ACIA. Option specific to the LOCI context (a "bare" 6551 does not have this cost; a warning reminds you of it if used without `--loci`).

### V23 Minitel

```bash
--serial-v23
```

Asymmetric baud rate: 1200 RX / 75 TX (Minitel/Prestel standard). Enabled automatically with the digitelec backend.

### Debug trace

```bash
--serial-trace serial.log
```

Logs every TX/RX, register write and signal change, with CPU timestamps:
```
CYCLE       DIR  HEX  CHR  STATUS    FIFO  SIGNALS
0003689369  TX   42   B    ........    0   DTR=1 DCD=1 CTS=1 DSR=1
0003689832  RX   42   B    ...T....    0   DTR=1 DCD=1 CTS=1 DSR=1
```

### ACIA offset

```bash
--acia-addr 0320
```

Changes the base address (default $031C). For non-standard interfaces.

## Timing

- **Clock**: 1.8432 MHz crystal / 16 = 115200 Hz internal
- **CPU cycles per byte**: `(1000000 × framebits) / baud`
  - 19200 baud 8-N-1: 520 cycles
  - 1200 baud 8-N-1: 8333 cycles
  - 75 baud 8-N-1: 133333 cycles
- **Frame limiter**: 50 Hz PAL (20 ms/frame) — same speed as a real ORIC
- **tx_cycles/rx_cycles**: initialised to tx_reload/rx_reload (not 0), resynchronised when the baud rate changes

## Typical command line for OricTel

```bash
# Minitel via Hayes modem (the ORIC program does ATD)
./oric1-emu -r roms/basic10.rom -t orictel.tap -f \
  --serial modem \
  --serial-buffer 256 \
  --serial-trace serial.log

# Or via Digitelec (DTR to connect, no AT commands)
./oric1-emu -r roms/basic10.rom -t orictel.tap -f \
  --serial digitelec:pavi.3617.fr:3617 \
  --serial-trace serial.log

# Or direct TCP (immediate connection, no modem)
./oric1-emu -r roms/basic10.rom -t orictel.tap -f \
  --serial tcp:pavi.3617.fr:3617 \
  --serial-v23 \
  --serial-buffer 256 \
  --serial-trace serial.log
```

---

bmarty — Phosphoric v1.15.1-alpha, 303 tests
