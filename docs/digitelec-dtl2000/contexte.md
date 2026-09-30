# Digitelec DTL 2000 modem — Technical reference

> Context document for the development agent.
> Target: emulation (Phosphoric), driver, and the Oric / PAVI telematics ecosystem.
> Status of the information: **sourced from the press + period manuals**. Bit-level
> details of the **Oric** variant are to be confirmed on the scans (see §10), since the
> programming PDF is a non-extractable image scan.

---

## 1. TL;DR

- French telematics modem by **Digitelec**, ~1985, ~1,490 FF (base configuration).
- Range: **DTL 2000**, **DTL 2000+**, **DTL 2100**, **DTL 3000**.
- **Modular card-based** architecture: backplane (power supply + line access) +
  computer link card + modem card.
- On the **Oric**: **memory-mapped** interface from `#3F8` to `#3FD`, via a ribbon cable to the
  **34-pin** connector. (Seen by the machine as a range of bytes, not a
  conventional serial link.)
- Key chips: **Motorola EF6821 PIA**, **Motorola EF6850 ACIA**, modem chip
  **Thomson EFCIS EFB 7510** (V23 card).
- Modes: **V23** (1200/75 baud, Minitel/Télétel emulation), **V21** (300 baud
  full duplex, Transpac), **V23 half-duplex 1200** (Oric↔Oric), **Bell 103** (depending on the
  variant).
- The `+` (DTL 2000+) adds **V23 answer full duplex** → allows use as a server.

---

## 2. Identity and variants

| Model       | V21 300 | V23 1200/75 | V23 answer | Bell 103 | Auto-answer | Switch | PTT-approved |
|-------------|:-------:|:-----------:|:-----------:|:--------:|:------------:|:------------:|:---------:|
| DTL 2000    | option  | yes         | no          | no       | no           | no           | yes       |
| DTL 2000+   | option  | yes         | **yes**     | no       | no           | no           | yes       |
| DTL 2100    | yes     | yes         | yes         | yes      | yes          | yes          | yes       |
| DTL 3000    | yes     | yes         | yes (univ.) | yes      | yes          | yes          | yes       |

> Table reconstructed from the *Microstrad* comparison (see Sources). To be cross-checked
> against the manuals for the exact values per revision.

Major practical difference: only the **2000+** (and above) can do **V23
answer full duplex**, the condition for an Oric to behave as a Minitel *server*
rather than a mere terminal.

---

## 3. Hardware architecture

Three functions, three components:

1. **Modulation/demodulation** → **modem** chip (V23 card built around the
   **Thomson EFCIS EFB 7510**).
2. **Parallel I/O + modem control** → **EF6821 PIA** (Peripheral Interface
   Adapter, 2 ports A/B with direction registers DDR, data registers OR, control registers CR).
3. **Serial/parallel data conversion** → **EF6850 ACIA** (Asynchronous
   Communications Interface Adapter, control/status register + data register).

Logical chain:

```
CPU (Oric)  <->  PIA 6821  <->  [control]  MODEM (EFB 7510)  <->  telephone line
            <->  ACIA 6850 <->  [serial data]  ^
```

The modem is programmed **through the PIA**; the payload data goes
**through the ACIA**.

---

## 4. Memory map

### 4.1 Oric (main target)

**Memory-mapped** interface, range **`#3F8`–`#3FD`** — **confirmed** by the
review in *L'Ordinateur Individuel n°69* (« l'Oric voit le modem comme une série
d'octets de #3F8 à #3FD » — "the Oric sees the modem as a series of bytes from #3F8 to #3FD").
The 6 bytes correspond exactly to a **6821 PIA
(4 registers) + a 6850 ACIA (2 registers)**, and the layout matches the CPC version
offset for offset (§4.2):

| Oric address | Component | Register |
|--------------|-----------|----------|
| `#3F8`       | PIA 6821  | Port A — data / DDRA (depending on CRA bit 2) |
| `#3F9`       | PIA 6821  | Control A (CRA) |
| `#3FA`       | PIA 6821  | Port B — data / DDRB |
| `#3FB`       | PIA 6821  | Control B (CRB) |
| `#3FC`       | ACIA 6850 | Control (write) / Status (read) |
| `#3FD`       | ACIA 6850 | Transmit / Receive (data) |

**Confidence level:**
- Range `#3F8`–`#3FD` → **confirmed** (primary period source).
- Layout of the 6 registers → **near-certain**: consistent with the number of
  registers of both chips, with the PRA/CRA/PRB/CRB order observed on
  Oric cards with a 6821 (cf. CEO forum analysis of a 6821 interface: `#380`=A/DDRA,
  `#381`=control A, `#382`=B/DDRB, `#383`=control B), and identical to the CPC version.
- **Bit-by-bit meaning** of the OR registers (PIA) and of the ACIA configuration for the
  *Oric* variant → **not sourced in text form** (only present in the scanned
  "Pour en savoir plus" leaflet / the "Programmation carte DTL V23" PDF, images
  without OCR). → still **to be extracted (§10)**.

> ⚠️ **Page 3 address conflict.** `#3F8`–`#3FD` lies in the upper half of
> page 3, shared with the disk electronics: a **Jasmin** occupies exactly
> `03F8`–`03FF` (side select, FDC reset, RAM overlay, ROMDIS, drive select).
> Since page-3 cards often use coarse decoding (address mirroring), the
> Digitelec DTL 2000 and a floppy drive **do not coexist cleanly**
> without precautions. To keep in mind for emulation and for real hardware.

### 4.2 Amstrad CPC (cross-reference, well documented)

Reliable, detailed source (Weka binder). **Same chips**, different addresses —
used as a model to deduce/validate the Oric behaviour.

| CPC address | Component | Role |
|-------------|-----------|------|
| `&F8F8`     | PIA 6821  | Dual DDR/OR register (port A) |
| `&F8F9`     | PIA 6821  | Control register CR (only **CR2** used) |
| `&F8FC`     | ACIA 6850 | Control (write) / Status (read) |
| `&F8FD`     | ACIA 6850 | Transmit / Receive (data) |

---

## 5. Programming — EF6821 PIA

- **Control** register: only **CR2** is used. `CR2=0` → addresses the **DDR**
  (direction). `CR2=1` → addresses the **OR** (data).
- **DDR** (direction): 1 bit per line, 1 = output, 0 = input. DTL 2000+ configuration:
  **all lines as outputs except line 0 as input** → `DDR = &FE`.
- **OR** (data): drives the modem states (mode selection, line connection…).

Known CPC values (to be transposed to the Oric addresses):

```basic
OUT &F8F9,&00   : REM CR2=0  -> select DDR
OUT &F8F8,&FE   : REM lines 1..7 as outputs, line 0 as input
OUT &F8F9,&04   : REM CR2=1  -> select OR
OUT &F8F8,&00   : REM modem init
```

Behavioural note: on the **connect command**, the modem waits ~**45 s** then
disconnects automatically if the remote carrier is lost. To restart a
connection: set **OR2 to 1** then back **to 0**.

---

## 6. Programming — EF6850 ACIA

- **Dual** control/status register:
  - **write** = **control** register (format, clock, transmit mode).
  - **read** = **status** register (carrier present, data received, transmit
    buffer empty…).
- **Mandatory init** before any configuration: write `&03` (master reset).
- Useful status bits: **bit 0 = data received (RDRF)**, **bit 1 = transmit
  data register empty (TDRE)**.
- Minitel/Videotex format: **7 bits, even parity, 1 stop**.

Example exchange loop (logic, CPC):

```basic
X = INP(&F8FC)                 : REM read the ACIA status register
IF (X AND 1) = 1 THEN GOSUB lire_caractere   : REM bit0: a byte has arrived
' ... to transmit, wait for TDRE:
WAIT: X = INP(&F8FC) : IF (X AND 2) <> 2 THEN GOTO WAIT
OUT &F8FD, Z                   : REM Z = ASCII code to transmit
```

Reading a received byte: `Y = INP(&F8FD)`.

---

## 7. Typical sequences

### 7.1 Generic initialisation (CPC, to be transposed)

```basic
10 OUT &F8F9,&00 : REM SELECT DDR (PIA)
20 OUT &F8F8,&FE : REM OR1..OR7 as outputs, OR0 as input
30 OUT &F8F9,&04 : REM SELECT OR
40 OUT &F8F8,&00 : REM MODEM INIT
50 OUT &F8FC,&03 : REM ACIA INIT (reset)
60 OUT &F8FC,&40 : REM no transmission
```

> Period tip: insert short wait loops (`FOR I=1 TO 1:NEXT`)
> between instructions, as the modem needs time to "digest" the writes.

### 7.2 Minitel emulation (V23 originate full duplex 75/1200)

```basic
70  OUT &F8F8,&BC : REM modem config  (OR: 10111100 ; OR2=1 -> not yet connected)
80  OUT &F8FC,&49 : REM ACIA config   (V23, 7 bits even parity 1 stop, no transmission)
90  REM *** wait for carrier ***
100 OUT &F8F8,&B8 : REM line connection (OR2 -> 0 : 10111000)
110 OUT &F8FC,&09 : REM command to send the carrier (CR6 -> 0 : 00001001)
```

On the Minitel terminal side, original keyboard mappings (cassette software):
`RETURN`→ENVOI, left arrow→RETOUR, right arrow→SUITE, `CTRL-D` ×2→CONNEXION/FIN
(Minitel function keys: Send, Back, Next, Connect/End).

---

## 8. Operating modes

| Mode                    | Transmit | Receive   | Typical use |
|-------------------------|----------|-----------|---------------|
| V23 originate full duplex | 75 baud | 1200 baud | Minitel / Télétel terminal (electronic directory, servers) |
| V23 answer full duplex  | 1200     | 75        | Minitel **server** (DTL 2000+ required) |
| V23 half duplex         | 1200     | 1200      | Oric↔Oric link (one transmits, the other receives, alternating) |
| V21 full duplex         | 300      | 300       | Transpac, general-purpose modem↔modem |
| Bell 103                | 300      | 300       | US compatibility (2100/3000 variants) |

Server limitation (without `+`): the plain V23 card does not handle asymmetric *answer mode*
→ no bidirectional interactive dialogue. The 2000+ lifts this limitation.

---

## 9. Original software and disks

- **Supplied cassette**: side 1 = Minitel terminal emulator (text only, **no**
  Videotex semi-graphics or colour in the original software) with a built-in **number
  dialler** (`-` inserts a 5 s delay); side 2 = **Oric↔Oric** comms
  (1200 baud, half-duplex, via POKE/DOKE depending on whether BASIC or a
  memory area is transferred).
- **Archived disk images** (.dsk, 140 KB): `Digitelec DTL 2000 Disk.dsk`,
  `Digitelec DTL 2000 Plus Disk.dsk` (see Sources).

---

## 10. Points to confirm on the scans

Range and register layout: **resolved** (cf. §4.1). Still to be checked in
**"Programmation carte DTL V23.pdf"** and the **"Pour en savoir plus"** leaflet
(8 pages) — not automatically extractable (image scans):

1. **Bit-by-bit** meaning of the PIA OR register for the Oric version
   (mode selection, OR2 = line connection, etc.).
2. ACIA init values for the Oric (the clock/divider may differ from the CPC).
3. **Ring detection** procedure (PIA line 0 input?) for server use.
4. Exact pinout of the Oric **34-pin connector** ↔ link card.

> Suggested action: OCR of the two PDFs (Tesseract `fra`) to resolve these points and
> freeze a definitive register table.

---

## 11. Implementation notes

### For emulation (Phosphoric)
- Model a **memory-mapped peripheral** on `#3F8`–`#3FD` exposing two
  components: a **6821 PIA** and a **6850 ACIA** (reuse generic 6821/6850 models
  if available).
- The **ACIA** carries the serial semantics: RDRF (bit 0) / TDRE (bit 1) flags on the status side,
  clock divider → bit rate. Emulating V23 = asymmetric 75/1200 rates (the "real"
  timing only matters for fidelity; a byte-by-byte model is functionally
  sufficient).
- The **PIA** carries the modem control: line connection (OR2), mode selection,
  ring detection. Wire these bits to a virtual "backend"
  (local loopback, TCP bridge, or Videotex gateway).
- **Page 3 decoding**: reproduce (or at least document) the `#3F8`–`#3FF` conflict
  with the disk electronics (Jasmin) — useful to diagnose cases where the modem and a
  drive are declared at the same time. See the box in §4.1.

### For PAVI / Minitel
- The DTL 2000 in **V23 answer** mode on the server side corresponds exactly to the role of a
  Télétel server: 75 baud upstream (client), 1200 downstream (server) — useful if
  PAVI ever wants to talk to real Oric hardware through a modem gateway.
- Beware: the original software does **not** decode Videotex semi-graphics.
  A modern terminal (or PAVI on the server side) must handle the G0/G1 set, serial
  attributes, etc., independently of the modem.

---

## 12. Sources

**Scanned manuals / disks (Apple II Documentation Project)**
- Programmation carte DTL V23 (PDF, scan) —
  `https://mirrors.apple2.org.za/Apple%20II%20Documentation%20Project/Peripherals/Modems/Digitelec%20DTL%202000/Manuals/Programmation%20carte%20DTL%20V23.pdf`
- User manual (PDF) —
  `https://mirrors.apple2.org.za/Apple%20II%20Documentation%20Project/Peripherals/Modems/Digitelec%20DTL%202000/Manuals/Digitelec%20DTL%202000%20Manuel%20de%20l%27utilisateurpdf.pdf`
- Operating instructions (PDF) —
  `https://mirrors.apple2.org.za/Apple%20II%20Documentation%20Project/Peripherals/Modems/Digitelec%20DTL%202000/Manuals/Digitelec%20DTL%202000%20Notice%20d%27utilisationpdf.pdf`
- Disk images (.dsk) —
  `https://mirrors.apple2.org.za/Apple%20II%20Documentation%20Project/Peripherals/Modems/Digitelec%20DTL%202000/Disk%20Images/`

**Programming (registers, chips)**
- Weka binders, « 8/5.3.1 Le modem Digitelec DTL 2000 » —
  `https://cpcrulez.fr/codingBOOK_weka_08531.htm`

**Reviews / press**
- L'Ordinateur Individuel n°69 (+ CPC Revue, Amstrad Mag n°5, Hebdogiciel) —
  `https://cpcrulez.fr/hardware-modem-modem_digitelec_DTL_2000.htm`
- Théoric, « L'Atmos à cœur ouvert » —
  `https://docplayer.fr/111169416-L-atmos-a-coeur-ouvert.html`
- Microstrad n°5 (full text, range comparison) —
  `https://archive.org/stream/microstrad05/Microstrad05_djvu.txt`
- Microstrad, « Les modems Digitelec DTL » —
  `https://cpcrulez.fr/hardware-modem-modem_digitelec_DTL_MS.htm`

**Reference**
- CPCWiki, Digitelec DTL 2000/2100 Modem —
  `https://www.cpcwiki.eu/index.php/Digitelec_DTL_2000/2100_Modem`

**Oric context**
- CEO Oric, « Logiciels de communication Oric » —
  `https://ceo.oric.org/community/applications/logiciels-de-communication-oric/`
- CEO Oric, « Interface Oric inconnue avec un 6821 » (corroborates the register order
  PRA/CRA/PRB/CRB and the coarse page-3 decoding) —
  `https://ceo.oric.org/community/peripheriques/interface-oric-inconnue-avec-un-6821/`

---

*Disclaimer: Oric addresses `#3F8`–`#3FD` **confirmed** and layout of the 6 registers
**near-certain** (cf. §4.1). The **bit values** of the sequences in §5–§7 are documented
for the Amstrad CPC version (identical chips) and remain to be validated bit for bit on the Oric
via §10.*
