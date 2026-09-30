# DTL 2000 — Registers extracted by OCR (primary source)

> Extracted by OCR (Tesseract `fra`, 300 dpi) from the 3 Digitelec PDFs on
> the apple2.org.za mirror. **All three are the Apple II / RS232 version**,
> NOT the Oric version. But the chips are identical (PIA EF6821 + ACIA
> EF6850) and the layout of the 6 bytes is offset-for-offset the same as
> the Oric's `#3F8`–`#3FD`. The values below can therefore be **transposed
> directly** to the Oric (only the base address changes).

## OCR sources

| PDF | Pages | Version | Useful content |
|-----|-------|---------|---------------|
| `prog_v23.pdf` | 7 | Apple II | **Exact register values** (POKE/PEEK) |
| `notice.pdf` | 5 | Apple II | Getting started, Minitel keys, COM software |
| `manuel.pdf` | 10 | RS232/V24 | DIP switches + circuits 108/105/106/109 |

## Mapping of the 6 bytes (Apple `$C0n8` → Oric `#3F8`)

| Off | Apple | Oric | Component | Register |
|-----|-------|------|-----------|----------|
| 0 | `$C0n8` | `#3F8` | PIA 6821 | Port A (OR) / DDRA — depending on CRA bit2 |
| 1 | `$C0n9` | `#3F9` | PIA 6821 | Control A (CRA) |
| 2 | `$C0nA` | `#3FA` | PIA 6821 | Port B — **NOT USED** by DTL V23 |
| 3 | `$C0nB` | `#3FB` | PIA 6821 | Control B — **NOT USED** |
| 4 | `$C0nC` | `#3FC` | ACIA 6850 | Control (W) / Status (R) |
| 5 | `$C0nD` | `#3FD` | ACIA 6850 | Tx (W) / Rx (R) data |

(Apple "n" = slot number + 8. Confirms §4.1 of the context document.)

## Initialisation — ASYMMETRIC mode (V23 Call, Minitel/Télétel terminal)

```basic
POKE #3F9, 0      : REM CRA=0  -> access DDRA
POKE #3F8, 244    : REM DDRA = $F4 = %11110100 (inputs: b0,b1,b3 ; outputs: b2,b4-b7)
POKE #3FC, 3      : REM ACIA master reset
POKE #3F9, 4      : REM CRA=4 (CR2=1) -> access Port A (OR)
POKE #3F8, 212    : REM OR = $D4 (line open = disconnected)
POKE #3FC, 73     : REM ACIA control = $49 = 7 bits + even parity + 1 stop, /16
```

## Initialisation — SYMMETRIC mode (V23 Half-Duplex 1200, Oric↔Oric)

```basic
POKE #3F9, 0      : REM CRA=0 -> DDRA
POKE #3F8, 244    : REM DDRA = $F4
POKE #3FC, 3      : REM ACIA master reset   (OCR had swapped 3/4 on lines 30-40)
POKE #3F9, 4      : REM CRA=4 -> OR
POKE #3F8, 196    : REM OR = $C4 (line open, symmetric mode)
POKE #3FC, 85     : REM ACIA control = $55 = 8 bits no parity 1 stop, /16
```

## PIA Port A (OR, `#3F8`) — confirmed bit semantics

- **bit 2 = line connection** (circuit 108 / "closing the line"):
  - `0` → line closed = **connected**
  - `1` → line open = **disconnected**
- **bit 4 = mode select**: asymmetric (`1`, values $Dx) vs symmetric (`0`, values $Cx).
- Concrete values:
  | Action | Asymmetric | Symmetric |
  |--------|-------------|------------|
  | Connect (line closed) | `208` ($D0) | `192` ($C0) |
  | Disconnect (line open) | `212` ($D4) | `196` ($C4) |
- **Pulse dialling**: alternate open(`212`)/closed(`208`).
  One pulse = open ~66 ms + closed ~33 ms; n pulses = digit n
  ("0" = 10 pulses). ~1 s of closed line between two pulse trains.

## ACIA `#3FC` — write (control)

| Value | Hex | Effect |
|--------|-----|-------|
| 3  | $03 | Master reset (mandatory before configuration) |
| 73 | $49 | V23 asym config (7E1, ÷16) **without transmitting** (RTS high, CR6/bit6=1) |
| 9  | $09 | **Start carrier transmission** (RTS low, bit6=0) |
| 85 | $55 | Symmetric config (8N1, ÷16) **without transmitting** |
| 21 | $15 | Start transmission (symmetric mode) |

**Bit 6** carries the carrier-transmit order (= circuit 105 RTS):
`0` = transmit, `1` = silent.

## ACIA `#3FC` — read (status)

| Bit | Meaning | Active |
|-----|---------------|-------|
| 0 | Character received (RDRF) — cleared by reading `#3FD` | `1` = available |
| 1 | Transmitted character actually sent (TDRE) | `1` = ready to write |
| 2 | Remote modem carrier present (DCD / circuit 109) | **`0` = present** (active low) |
| 3 | Modem ready to transmit (CTS / circuit 106) | **`0` = ready** (active low) |

> ⚠️ Carrier detection (bit 2): after reading `#3FC`, do a **dummy
> read of `#3FD`** to reset bit 2.

## ACIA `#3FD` — data

- **Read**: last character received (read when status bit0=1).
- **Write**: character to transmit (write when status bit1=1).
- 7-bit ASCII in V23 Minitel (bit7 always 1); 8 bits in symmetric mode.

## Modes & DIP switches (RS232 manual, switches 3-4-5)

| Mode | sw3 | sw4 | sw5 | Tx/Rx rate |
|------|:----:|:----:|:----:|-------------|
| V23 Call     | 0 | 1 | 1 | 75 / 1200 |
| V23 Answer   | 1 | 1 | 1 | 1200 / 75 (server) |
| V21 Call     | 0 | 0 | 1 | 300 / 300 |
| V21 Answer   | 1 | 0 | 1 | 300 / 300 |
| V23 Half Dpx | 0 | 1 | 0 | 1200 / 1200 |

> **DTL V23** card alone: only **V23 Call** + **V23 Half Duplex**.
> V23/V21 Answer (= server) requires the **DTL PLUS** card.

## V24 circuit ↔ bit correspondence (emulation consistency)

| V24 circuit | Role | Phosphoric bit |
|-------------|------|----------------|
| 108 (DTR) | line connection | PIA OR bit 2 |
| 105 (RTS) | request to send carrier | ACIA ctrl bit 6 |
| 106 (CTS) | clear to send | ACIA status bit 3 (active low) |
| 109 (DCD) | received carrier detect | ACIA status bit 2 (active low) |

## Loopback note (symmetric mode)

In symmetric V23, an **internal loopback** makes the Detection +
Reception LEDs light up during transmission: the transmitter simultaneously receives
what it sends (useful for transmission checking). To be modelled if aiming for fidelity.
