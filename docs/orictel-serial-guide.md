# Phosphoric — Guide technique liaison série pour OricTel

## Architecture

```
Programme ORIC (BASIC/ASM)
        │
        │ POKE/PEEK $031C-$031F
        ▼
┌─────────────────────┐
│   ACIA 6551         │  Émulation fidèle MOS 6551
│   4 registres I/O   │  Cristal 1.8432 MHz / 16
│   $031C = DATA      │  Frame format variable (5-8 bits + parité + stop)
│   $031D = STATUS    │  IRQ TX/RX level-triggered
│   $031E = COMMAND   │
│   $031F = CONTROL   │
└────────┬────────────┘
         │ send()/recv()/poll()
         ▼
┌─────────────────────┐
│   Backend série     │  Abstraction vtable (6 implémentations)
│   (interchangeable) │
└────────┬────────────┘
         │
         ▼
    TCP / PTY / COM / etc.
```

## Les 4 registres ACIA

### $031C — DATA (POKE 796 / PEEK 796)

- **Écriture** : place un octet dans le Transmitter Data Register (TDR)
  - Le bit TDRE (status bit 4) passe à 0
  - L'octet est transmis après `tx_reload` cycles CPU
  - Le masque `bitmask` est appliqué (7-bit mode → bit 7 forcé à 0)

- **Lecture** : lit le Receiver Data Register (RDR)
  - Efface les flags RDRF, OVRN, PE, FE
  - Si le FIFO est actif, charge automatiquement le byte suivant depuis la file

### $031D — STATUS (PEEK 797) / RESET (POKE 797)

- **Lecture** : retourne l'état courant

  ```
  Bit 7 : IRQ (1=IRQ active) — effacé par la lecture
  Bit 6 : DSR (1=DSR inactive, 0=DSR active)
  Bit 5 : DCD (1=pas de porteuse, 0=porteuse détectée)
  Bit 4 : TDRE (1=registre TX vide, prêt à envoyer)
  Bit 3 : RDRF (1=donnée reçue disponible)
  Bit 2 : OVRN (1=overrun, donnée précédente non lue)
  Bit 1 : FE (framing error)
  Bit 0 : PE (parity error)
  ```

  **ATTENTION** : lire le status efface le bit IRQ ! C'est le comportement MOS 6551 réel. Si vous utilisez les IRQ en TX+RX simultané, activez `--serial-irq-on-rdrf` (mode WDC 65C51) pour que l'IRQ re-fire tant que RDRF est set.

- **Écriture** : programmed reset (n'importe quelle valeur)
  - Remet TDRE=1, efface overrun
  - Efface les bits 0-4 du Command Register (DTR off, IRQ off)
  - Préserve les bits 5-7 du Command Register (parité)
  - Ne touche PAS le Control Register (baud rate conservé)

### $031E — COMMAND (POKE 798 / PEEK 798)

```
Bit 0 : DTR (1=Data Terminal Ready — active le modem)
Bit 1 : IRD (1=IRQ réception DÉSACTIVÉE, 0=IRQ RX active)
Bits 3-2 : TIC (contrôle IRQ transmission)
  00 = RTS high, TX IRQ off
  01 = RTS low, TX IRQ on
  10 = RTS low, TX IRQ off
  11 = RTS low, break on line
Bit 4 : ECHO (1=mode écho, renvoie les bytes reçus)
Bit 5 : PME (1=parité activée)
Bits 7-6 : PMC (type de parité)
```

**Valeur recommandée : `POKE 798,3`** (DTR on + IRQ RX désactivée)

Si vous utilisez les IRQ, `POKE 798,1` active DTR + IRQ RX. Mais ATTENTION : chaque byte reçu génère une IRQ. Sans handler ASM, le CPU entre en boucle IRQ infinie car la ROM ne gère pas l'ACIA.

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

**Valeur recommandée : `POKE 799,31`** ($1F = 19200 baud, 8-N-1, clock interne)

Pour 1200 baud 8-N-1 : `POKE 799,24` ($18)

## Séquence d'initialisation minimale

```basic
10 DA=796:ST=797:CM=798:CT=799
20 POKE ST,0          : REM Reset ACIA
30 POKE CT,31         : REM 19200 baud, 8-N-1
40 POKE CM,3          : REM DTR on, IRQ RX off
```

## Envoi d'un octet

```basic
100 REM Attendre que le TX soit libre
110 IF (PEEK(ST) AND 16)=0 THEN 110
120 POKE DA, octet
```

## Réception d'un octet

```basic
200 REM Vérifier si un byte est disponible
210 IF (PEEK(ST) AND 8)=0 THEN GOTO 200
220 C=PEEK(DA)
```

## Vérification de la porteuse (DCD)

```basic
300 S=PEEK(ST)
310 IF (S AND 32)=32 THEN PRINT"PAS DE PORTEUSE"
320 IF (S AND 32)=0 THEN PRINT"CONNECTE"
```

## Backends disponibles

### 1. Loopback (test)

```bash
./oric1-emu -r roms/basic10.rom --serial loopback
```

TX revient directement en RX. Buffer circulaire 256 octets. Utile pour vérifier que l'ACIA fonctionne sans réseau.

### 2. TCP brut

```bash
./oric1-emu -r roms/basic10.rom --serial tcp:bbs.host:23
```

Connexion TCP directe. Pas de commandes AT, pas de flow control modem. Les données sont envoyées/reçues byte par byte sur le socket. Connexion établie au démarrage.

### 3. Modem Hayes (commandes AT)

```bash
# Mode commande pur — le programme ORIC dial via ATD
./oric1-emu -r roms/basic10.rom --serial modem

# Host prédéfini — ATD seul connecte ici
./oric1-emu -r roms/basic10.rom --serial modem:bbs.host:23

# Mode serveur — accepte les connexions entrantes
./oric1-emu -r roms/basic10.rom --serial modem:listen:2323
```

Démarre en **mode commande**. L'ACIA voit les réponses AT comme des données reçues.

Commandes supportées :
- `AT` → `OK`
- `ATZ` → reset → `OK`
- `ATE0` / `ATE1` → echo off/on → `OK`
- `ATH` → raccrocher → `OK`
- `ATD host:port` → connecter via TCP → `CONNECT` ou `NO CARRIER`
- `ATDT host:port` → idem (T ignoré)
- `ATA` → accepter connexion (mode listen) → `CONNECT` ou `ERROR`
- `ATS0=N` → auto-answer après N rings → `OK`
- `+++` → retour mode commande (guard time) → `OK`

Buffers internes 64 Ko RX/TX.

Exemple BASIC complet :
```basic
10 DA=796:ST=797:CM=798:CT=799
20 POKE ST,0:POKE CT,31:POKE CM,3
30 REM ENVOYER ATD
40 A$="ATD pavi.3617.fr:3617"+CHR$(13)
50 FOR I=1 TO LEN(A$)
60 POKE DA,ASC(MID$(A$,I,1))
70 FOR W=1 TO 100:NEXT W
80 NEXT I
90 REM ATTENDRE REPONSE
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

### 4. Digitelec DTL 2000

```bash
./oric1-emu -r roms/basic10.rom --serial digitelec:minitel.host:516
```

Émule le modem externe Digitelec DTL 2000 (1984). Pas de commandes AT. Contrôle par les lignes RS232 :
- DTR on (POKE CM,3) → le modem connecte via TCP
- DTR off (POKE CM,0) → le modem raccroche
- DCD dans le status register → état de la connexion
- CTS flow control automatique (buffer interne 512 octets)
- V23 1200/75 activé automatiquement

### 5. PTY (pseudo-terminal)

```bash
./oric1-emu -r roms/basic10.rom --serial pty
```

Crée un pseudo-terminal. Le log affiche le device path :
```
Serial PTY: opened /dev/pts/3 (master fd=5)
```
Connectez minicom/screen/picocom sur ce device depuis un autre terminal.

### 6. COM (port série réel)

```bash
./oric1-emu -r roms/basic10.rom --serial com:9600,8,N,1,/dev/ttyUSB0
```

Vrai port série via termios (Linux). Format : `baud,databits,parité,stopbits,device`.

### 7. PicoWiFiModemUSB (modem WiFi LOCI)

```bash
# Sous --loci, l'ACIA est mappée à $0380 (défaut firmware LOCI)
./oric1-emu -r roms/basic10.rom --loci --serial picowifi:MonWiFi:motdepasse
```

Émule le modem WiFi de sodiumlb (Pico W exposée par LOCI comme ACIA). Le
programme ORIC compose en commandes AT. Jeu v0.1.0 complet : `AT$SSID=`,
`AT$PASS=`, `ATC1` (connexion WiFi), `ATDT host:port` (TCP), `ATNET`,
numéros rapides `AT&Z`, registres S, etc. WiFi simulé, données = vrai TCP.

**⚠ Minitel / Vidéotex : forcer NET0.** Le mode telnet réel (NET1, défaut)
traduit `CR`→`CR+NUL` et double `IAC` (0xFF) — ce qui **corrompt** un flux
Vidéotex (où ces octets sont des données). Compose en NET0 pour un flux
transparent :

```basic
A$="ATDT-mon.serveur.minitel:516"+CHR$(13) : REM le '-' force NET0
```

Le préfixe `-` (NET0), `=` (NET1 réel), `+` (NET2 faux) fixe le mode telnet
de la session. Pour du Vidéotex pur, le backend `digitelec` + `--serial-v23`
reste l'option la plus idiomatique (V23 1200/75 natif, sans commandes AT).

## Options d'amélioration

### FIFO RX (anti-overrun)

```bash
--serial-buffer 256
```

L'ACIA réelle a 1 seul octet RX. Si le CPU n'a pas lu avant l'arrivée du suivant → overrun. Le FIFO ajoute une file d'attente transparente. Quand on lit DATA ($031C), le byte suivant est chargé automatiquement. RDRF reste à 1 tant que la file n'est pas vide.

**Quand l'utiliser** : quand le programme ORIC fait des opérations longues entre les lectures (clear screen, scroll, affichage Vidéotex).

### IRQ WDC 65C51

```bash
--serial-irq-on-rdrf
```

Le MOS 6551 a un bug : lire le status ($031D) efface le bit IRQ. Si on lit le status pour vérifier TDRE (TX), l'IRQ pour un byte RX en attente est perdue. Le mode WDC 65C51 re-fire l'IRQ tant que RDRF est set.

**Quand l'utiliser** : quand le programme utilise les IRQ en TX+RX simultané.

### V23 Minitel

```bash
--serial-v23
```

Baud asymétrique : 1200 RX / 75 TX (standard Minitel/Prestel). Activé automatiquement avec le backend digitelec.

### Debug trace

```bash
--serial-trace serial.log
```

Log tous les TX/RX, écritures registres, changements de signaux avec timestamps CPU :
```
CYCLE       DIR  HEX  CHR  STATUS    FIFO  SIGNALS
0003689369  TX   42   B    ........    0   DTR=1 DCD=1 CTS=1 DSR=1
0003689832  RX   42   B    ...T....    0   DTR=1 DCD=1 CTS=1 DSR=1
```

### Offset ACIA

```bash
--acia-addr 0320
```

Change l'adresse de base (défaut $031C). Pour les interfaces non-standard.

## Timing

- **Horloge** : cristal 1.8432 MHz / 16 = 115200 Hz interne
- **Cycles CPU par octet** : `(1000000 × framebits) / baud`
  - 19200 baud 8-N-1 : 520 cycles
  - 1200 baud 8-N-1 : 8333 cycles
  - 75 baud 8-N-1 : 133333 cycles
- **Frame limiter** : 50 Hz PAL (20 ms/frame) — même vitesse qu'un vrai ORIC
- **tx_cycles/rx_cycles** : initialisés à tx_reload/rx_reload (pas 0), resynchronisés au changement de baud

## Ligne de commande type pour OricTel

```bash
# Minitel via modem Hayes (le programme ORIC fait ATD)
./oric1-emu -r roms/basic10.rom -t orictel.tap -f \
  --serial modem \
  --serial-buffer 256 \
  --serial-trace serial.log

# Ou via Digitelec (DTR pour connecter, pas de commandes AT)
./oric1-emu -r roms/basic10.rom -t orictel.tap -f \
  --serial digitelec:pavi.3617.fr:3617 \
  --serial-trace serial.log

# Ou TCP direct (connexion immédiate, pas de modem)
./oric1-emu -r roms/basic10.rom -t orictel.tap -f \
  --serial tcp:pavi.3617.fr:3617 \
  --serial-v23 \
  --serial-buffer 256 \
  --serial-trace serial.log
```

---

bmarty — Phosphoric v1.15.1-alpha, 303 tests
