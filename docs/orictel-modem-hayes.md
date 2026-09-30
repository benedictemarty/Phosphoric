# Phosphoric — Hayes AT modem support for OricTel

Hello,

Phosphoric now supports a complete Hayes modem with AT commands, usable from BASIC or from ORIC assembly code.

## Launching

```bash
# Pure command mode — the ORIC program chooses the destination via ATD
./oric1-emu -r roms/basic10.rom --serial modem

# Predefined host — a bare ATD connects to this host
./oric1-emu -r roms/basic10.rom --serial modem:bbs.host:23

# BBS server mode — waits for incoming connections
./oric1-emu -r roms/basic10.rom --serial modem:listen:2323
```

## Supported AT commands

| Command | Action | Response |
|----------|--------|---------|
| `AT` | Modem test | `OK` |
| `ATZ` | Modem reset | `OK` |
| `ATE0` / `ATE1` | Echo off / on | `OK` |
| `ATH` | Hang up | `OK` |
| `ATD host:port` | Connect via TCP | `CONNECT` or `NO CARRIER` |
| `ATDT host:port` | Same (T ignored) | `CONNECT` or `NO CARRIER` |
| `ATA` | Accept incoming connection (listen mode) | `CONNECT` or `ERROR` |
| `ATS0=N` | Auto-answer after N rings | `OK` |
| `+++` | Escape data → command mode (guard time) | `OK` |

## BASIC example

```basic
10 DA=796:ST=797:CM=798:CT=799
20 POKE ST,0:POKE CT,31:POKE CM,3
30 REM == DIAL ==
40 A$="ATD pavi.3617.fr:3617"+CHR$(13)
50 FOR I=1 TO LEN(A$)
60 POKE DA,ASC(MID$(A$,I,1))
70 FOR W=1 TO 100:NEXT W
80 NEXT I
90 REM == WAIT FOR CONNECT ==
100 R$=""
110 S=PEEK(ST):IF (S AND 8)=0 THEN 110
120 C=PEEK(DA)
130 IF C=10 THEN 110
140 IF C=13 THEN 160
150 R$=R$+CHR$(C):GOTO 110
160 IF R$="CONNECT" THEN PRINT"CONNECTE!":GOTO 200
170 PRINT R$:END
200 REM == TERMINAL ==
210 S=PEEK(ST)
220 IF (S AND 8)=8 THEN C=PEEK(DA):IF C>31 AND C<127 THEN PRINT CHR$(C);
230 IF (S AND 8)=8 THEN IF C=13 THEN PRINT
240 K$=KEY$:IF K$="" THEN 210
250 IF ASC(K$)=27 THEN 300
260 POKE DA,ASC(K$):GOTO 210
300 REM == HANGUP ==
310 FOR W=1 TO 500:NEXT W
320 A$="+++":FOR I=1 TO 3:POKE DA,43:FOR W=1 TO 200:NEXT W:NEXT I
330 FOR W=1 TO 500:NEXT W
340 A$="ATH"+CHR$(13)
350 FOR I=1 TO LEN(A$):POKE DA,ASC(MID$(A$,I,1)):FOR W=1 TO 100:NEXT W:NEXT I
360 PRINT"DECONNECTE":END
```

(The program prints `CONNECTE!` ("connected!") and `DECONNECTE` ("disconnected").)

## Other available options

```bash
--serial-buffer 256      RX FIFO (avoids overrun during clear screen)
--serial-irq-on-rdrf     WDC 65C51 mode (IRQ re-triggers while RDRF is set)
--serial-trace FILE      TX/RX debug trace with CPU timestamps
--serial-v23             V23 1200/75 baud mode (Minitel)
```

## AY-3-8912 register 7 fix (included in this version)

The keyboard bug after modifying register 7 is fixed. Phosphoric now blocks the keyboard scan when the PSG's Port A is set as output (reg7 bit 6 = 0), like Oricutron and the real hardware. The `ay_write(7, $7F)` workaround is still needed on the program side.

---

bmarty — Phosphoric v1.15.1-alpha, 303 tests
