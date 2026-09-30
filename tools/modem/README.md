# LOCI WiFi terminal — real PicoWiFiModemUSB through Phosphoric

Drives the **real** PicoWiFiModemUSB (plugged into the host over USB) from the
emulated Oric, exactly as on a real LOCI: modem exposed as an **ACIA 6551 at $0380**,
`--loci` enabled. The `com:` backend connects the emulated ACIA to the real device.

## Prerequisites
- PicoWiFiModemUSB connected → `/dev/ttyACM0` (check: `lsusb | grep -i pico`).
- User in the `dialout` group (port access).

## Starting the terminal (interactive, SDL2 window)
```bash
make SDL2=1
make tools                                   # bas2tap
./bas2tap tools/modem/modem_term.bas -o tools/modem/modem_term.tap --auto-run

./oric1-emu -r roms/basic11b.rom --loci \
    --serial com:9600,8,N,1,/dev/ttyACM0 --acia-addr 0380 \
    --serial-buffer 1024 \
    -t tools/modem/modem_term.tap -f
```
The program runs automatically. Type AT commands on the keyboard:

| Command | Effect |
|---|---|
| `ATI` ⏎ | modem info (WiFi, IP, RSSI…) |
| `AT` ⏎ | test → `OK` |
| `ATDT host:port` ⏎ | outgoing telnet call (BBS, service…) |
| `ATH` ⏎ | hang up |
| `ATE0` / `ATE1` | modem echo off/on (the terminal has no local echo) |
| `CTRL-C` | quit the terminal (BASIC BREAK) |

## Confirmed test target — TELEHACK

`telehack.com:23` is a public telnet service, reliable and 100 % ASCII (ideal
for the Oric's 40-column screen). Once the terminal is running:

```
ATDT telehack.com:23
```

Actual response obtained (real modem, real WiFi):

```
DIALLING telehack.com:23
CONNECT 9600
Connected to TELEHACK port 112
It is ... in Mountain View, California, USA.
There are N local users. There are M hosts on the network.
May the command line live forever.
Command, one of the following:
  2048  advent  eliza  figlet  starwars  rfc  usenet  ...
Type HELP for a detailed command list.
```

Fun commands to try once connected: `starwars`, `eliza`,
`figlet hello`, `advent`, `2048`, `today`, `rfc 1`. `CTRL-C` interrupts a
BBS command (on the TELEHACK side), not the terminal.

Other telnet BBSes verified as reachable: `particlesbbs.dyndns.org:6400`,
`bbs.fozztexx.com:23`, `blackflag.acid.org:23`.

> Note: if an `ATDT` immediately returns `NO CARRIER (00:00:00)`, it is a
> failing DNS resolution (non-existent host name) — not a problem in the
> emulation chain.

## Usenet / NNTP (Eternal-September) — CRLF terminal

NNTP requires **CRLF** line endings, whereas the telehack terminal sends
CR only. Use the `modem_nntp.bas` variant (sends CR+LF on RETURN):

```bash
./oric1-emu -r roms/basic11b.rom --loci \
    --serial com:9600,8,N,1,/dev/ttyACM0 --acia-addr 0380 --serial-buffer 8192 \
    -t tools/modem/modem_nntp.tap -f
```

Then (the `-` = transparent/raw mode, required for NNTP):
```
ATDT-news.eternal-september.org:119      (free account required)
AUTHINFO USER <login>
AUTHINFO PASS <password>
GROUP comp.sys.oric
HEAD <n> / BODY <n> / ARTICLE <n>
POST                                     (post; greeting "posting ok")
```
Free account: https://www.eternal-september.org/ . Reading AND posting OK.
NB: keyboard typing is paced by hand (no loss); a programmatic send of a
long string may lose bytes (ACIA in instant transfer).

## Files
- `modem_term.bas` — interactive full-duplex terminal (keyboard ↔ modem, CR).
- `modem_nntp.bas` — interactive **CRLF** terminal (for NNTP/Usenet).
- `modem_probe.bas` — minimal probe: sends `ATI` and displays the response
  (auto-run, no interaction). Quick test.

## ACIA driver (modem_term.bas)
ACIA 6551 at `$0380`: data `$0380`, status `$0381` (TDRE=$10, RDRF=$08),
command `$0382` (`$0B` = DTR on, RX-IRQ off, RTS low), control `$0383` (`$1E`).
Full-duplex loop: drains RX to the screen, then `KEY$` → TX.

## Empirically validated
`TX: ATI\r` → `RX: Pico WiFi modem … WiFi status: CONNECTED … OK` displayed on
the Oric's screen, via the real modem over real WiFi (live RSSI).
