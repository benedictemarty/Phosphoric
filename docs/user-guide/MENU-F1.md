# The F1 menu — user manual

The **F1** menu (« Périphériques E/S », I/O peripherals) controls everything around
the Oric: floppies, tape, expansion cards, snapshots, printer, joystick, keyboard. It
replaces most command-line options while the machine is running.

![Main page of the F1 menu](../images/menu-peripheriques.png)

## 1. Opening and closing

- **F1** opens the menu; **F1** or **Esc** closes it.
- While the menu is open, **the machine is frozen and the sound is muted**. It
  resumes exactly where it was.
- The menu takes up the whole screen. Keys typed do not go to the Oric.
- The last result (success in yellow, error in red) is shown above the help bar,
  for example « Lecteur A : SEDORIC.DSK ».

## 2. Keys

The bottom bar recalls the keys of the current page.

| Key | Main page | File selector | Card page |
|---|---|---|---|
| ↑ ↓ | change line | change file | change setting |
| ← → | column within a line (media / protection, rewind), move from one button to another | ←: close the selector | ←: back to the card list |
| Enter | activate the line | choose the file | change the setting |
| Delete or Backspace | eject (drive, tape) | — | restore the default setting |
| Esc | resume the machine | close the selector | back to the card list |
| Home / End | first / last line | first / last file | — |
| Page Up / Page Down | — | 20 files up / down | — |
| A letter | — | go to the next file starting with that letter | — |

## 3. The main page

### Floppies A to D

The frame title shows the disk interface in place: **Microdisc**, **Jasmin**,
**LOCI**, or « pas d'interface disque » (no disk interface). Without an interface,
nothing can be mounted: first enable a disk card (section 4).

- **Enter** on a drive opens the file selector (`.dsk` images). The first line,
  « Éjecter la disquette » (eject the floppy), empties the drive.
- **Delete** ejects directly.
- **→** then **Enter** toggles the drive's **write protection**
  (« écriture » (writable) / « protégée » (protected)). With LOCI, there is no
  per-drive protection: this column does not appear.
- The same image cannot be in two drives at once (the menu refuses).
- With `--disk-writeback` at launch, a modified floppy is written back to its `.dsk`
  file when it is ejected or replaced. Without this option, the Oric's writes stay
  in memory and are lost on ejection. With the LOCI in `intégré` mode, ejecting
  writes the modified sectors.
- « — absent — »: this drive does not exist on the interface in place.
- With the LOCI in `firmware` or `usb` mode, its own menu mounts the floppies (MENU
  button: **F8**); the F1 menu reminds you of this.

### Tape

- **Enter** opens the selector (`.tap` files). The first line ejects.
- **Delete** ejects.
- **→** then **Enter** rewinds the tape to the start.
- The line shows the name, the motor state, the percentage read and a gauge.
- Once the tape is inserted, type `CLOAD""` on the Oric.

### Expansion cards

The frame summarizes the cards: **●** present (with its address and main setting),
**✗** absent. **Enter** on this frame opens the card list (section 4).

### Peripherals

| Line | Enter does | Detail |
|---|---|---|
| **Instantanés** (snapshots) | opens the `.ost` selector | first line « Enregistrer un nouvel instantané » (save a new snapshot) → `snapshots/etatNNNN.ost` (next free number). Choosing a snapshot **resumes the machine at that instant** and closes the menu. |
| **Imprimante** (printer) | coupée → texte → traceur MCP-40 → coupée (off → text → MCP-40 plotter → off) | text: `LPRINT`, `LLIST` go to `impression.txt`; plotter: `traceur.bmp`, written when switched off. |
| **Joystick** | aucun → clavier → manette → aucun (none → keyboard → gamepad → none) | keyboard: keyboard arrows + fire; gamepad: the first gamepad plugged in (otherwise it is waited for). |
| **Clavier** (keyboard) | QWERTY ↔ AZERTY | PC keyboard layout translated for the Oric. |
| **Cassette** (at launch) | CLOAD through the patched ROM ↔ direct injection | only takes effect **at the next launch**: remember « Enregistrer la configuration » (save configuration). |

### The three buttons

- **Redémarrer (RESET)** (restart): like F5, restarts the 6502 (the LOCI reset
  button if it is there; floppies stay mounted) and closes the menu.
- **Enregistrer la configuration** (save configuration): writes `phosphoric.cfg`
  (section 6).
- **Reprendre** (resume): closes the menu (like Esc or F1).

## 4. Expansion cards

**Enter** on the « Cartes d'extension » (expansion cards) frame opens the list. Each
line shows the card, « présente » (present) or « absente » (absent), and a red star
**\*** if it has been modified. Below the list, the explanation of the selected card.

![Card list, LOCI selected in usb mode](../images/menu-cartes.png)

- **Enter** on a card opens its page.
- **Appliquer et redémarrer** (apply and restart) relaunches the emulator with the
  chosen cards. This is a **cold restart**: the program relaunches itself with the
  same command line, minus the card options, plus the new ones. The machine starts
  from scratch.
- **Annuler les changements** (cancel changes) goes back to the cards of the running
  machine.
- If two cards overlap in memory, or if a setting is missing, the menu says so in
  red and **refuses to apply** (see section 5).

### Card page

- First line **Carte** (card): **Enter** makes it present or absent.
- Then one setting per line, with its explanation at the bottom of the page, its
  default value and what Enter does:

| Setting type | Enter does |
|---|---|
| file (ROM, image) | opens the selector (folders `roms`, `roms/loci`, `disks`, `tapes`, `media` and the current folder); « Aucun fichier » (no file) clears the value |
| text, folder, number | opens the input field: type, **Enter** confirms, **Delete** erases, **Esc** cancels |
| I/O address | hexadecimal input, checked between `0300` and `03FF` |
| yes / no | toggles |
| choice | moves to the next value |

**Delete** restores the setting to its default value.

### Exclusive groups

Only one card per group at a time; enabling one turns the other off:

- **disk** group: Microdisc, Jasmin, LOCI;
- **midi** group: Mageco MIDI, ORICON.

### Cards and their settings

Setting names are shown as they appear in the menu.

| Card | Role | Settings (default value) |
|---|---|---|
| **Microdisc** | Oric floppy controller (WD1793), 4 drives, at `$0310` | ROM du contrôleur (controller ROM) (`roms/microdis.rom`) |
| **Jasmin** | Jasmin floppy controller (WD177x, TDOS), at `$03F4` | ROM de démarrage (boot ROM) (`roms/jasmin.rom`) |
| **LOCI** | LOCI cartridge, at `$03A0`; see below | Mode, then the settings of that mode |
| **ACIA 6551** | serial port: modem, terminal, Minitel, BBS | Ligne série (serial line) (`loopback`), Adresse d'E/S (I/O address) (`031C`; `0380` for the LOCI's ACIA), Vitesse (bauds) (baud rate) (empty: instantaneous), Tampon de réception (receive buffer) (empty), Mode V23 (1200/75) (non) |
| **DTL 2000** | Digitelec modem (PIA 6821 + ACIA 6850), V23 line to a Minitel server | Ligne V23 (V23 line) (`loopback`), Adresse d'E/S (`03F8`) |
| **Mageco MIDI** | MIDI interface (ACIA 6850 at 31,250 baud) | Liaison MIDI (MIDI link) (`loopback`), Adresse d'E/S (`03FE`) |
| **ORICON** | MIDI variant at `$031C` | Liaison MIDI (`loopback`) |
| **SP0256** | Mageco speech synthesis (SP0256-AL2, allophones) | ROM d'allophones (allophone ROM) (`roms/al2.bin`), Adresse d'E/S (`03F1`) |
| **MEA8000** | TMPI speech synthesis (formants, no ROM) | Adresse d'E/S (`03FE`) |
| **Hôte (hostfs)** | PC folder mounted in the Oric | Dossier monté (mounted folder) (`.`) |
| **ULA-NG** | extended ULA (palette, modes), always present at `$0340` | none |

Possible values of a **serial line** (ACIA, DTL 2000): `loopback` (local echo),
`tcp:hôte:port`, `modem:hôte:port` (incoming calls), `pty` (pseudo-terminal),
`com:bauds,bits,parité,stop,périphérique` (real serial port), `file:entrée[:sortie]`,
and for the ACIA only `picowifi[:ssid[:mot_de_passe]]` (emulated Wi-Fi modem).
**MIDI link**: `loopback`, `midi[:cible]` (real-time MIDI, if the binary has it),
`smf:fichier.mid[:loop]`, `tcp:hôte:port`, `file:entrée[:sortie]`.

Mageco MIDI and MEA8000 both use `$03FE` by default: change the address of one of
them to have both together.

### The LOCI card and its three modes

The first setting, **Mode**, chooses which LOCI; **only the settings of the chosen
mode are shown** (the others are remembered, but not used). In `usb` mode, only
the LOCI-USB port is left:

![LOCI card page in usb mode](../images/menu-carte-loci.png)

The three modes:

| Mode | What it is | Settings |
|---|---|---|
| `intégré` | LOCI emulated by Phosphoric (`--loci`) | **Démarrer sur le menu LOCI** (boot into the LOCI menu) (oui), **Image de carte SD** (SD card image), **Dossier flash interne** (internal flash folder), **Modem Wi-Fi (picowifi)**: aucun / simulé / réel (none / emulated / real), **Port du picowifi réel** (real picowifi port) (empty: detected) |
| `firmware` | the real RP2040 firmware runs inside the emulator (`--loci-emu`), for firmware development | **Firmware (ELF)** (mandatory), **Image flash du firmware** (firmware flash image) (empty: `<ELF>.flash`, « - »: volatile), **Image de clé USB** (USB stick image) |
| `usb` | a **LOCI-USB** plugged into the PC (`--loci-hw`): a Feather RP2040 runs the LOCI firmware, and Phosphoric plays the Oric over USB | **Port de la LOCI-USB** (LOCI-USB port) (empty: detected) |

- The Feather **is** the LOCI: it is not a bridge to a LOCI 1.3 cartridge, and it
  does not plug into a real Oric.
- Only the modes present in this binary are offered: `firmware` requires that
  `~/loci/emul` was there at build time, `usb` that `~/loci/loci-usb` was (not on
  Windows).
- **Automatic detection** (Linux): a port left empty is searched for from the
  device's USB name (« PicoWifiModemUSB » for the picowifi, « LOCI-USB… » for the
  Feather). The search reads `/sys` without opening the port.
- The LOCI **MENU button** is **F8**; a press of 2 seconds or more is a long press
  (diagnostics).
- LOCI modem and ACIA 6551 card do not go together: only one serial line at a time.

## 5. Card error messages

| Message | What to do |
|---|---|
| « X et Y se chevauchent en $03xx » | X and Y overlap: change the I/O address of one of the two |
| « Modem LOCI et ACIA 6551 : une seule ligne série à la fois » | only one serial line at a time: remove the ACIA card, or set the LOCI modem to « aucun » (none) |
| « Modem LOCI réel : aucun picowifi USB détecté » | no USB picowifi detected: plug in the picowifi, or give its port |
| « LOCI : mode « … » absent de ce binaire » | mode not in this binary: choose another mode (backend not compiled in) |
| « LOCI firmware : indiquer le fichier ELF du firmware » | firmware ELF file missing: fill in **Firmware (ELF)** |
| « LOCI-USB : aucune détectée (indiquer le port) » | no LOCI-USB detected: plug in the Feather, or give its port |
| « Version web : les cartes se choisissent au lancement » | web version, cards are chosen at launch: in the browser, the cards are read-only |

## 6. Saving the configuration (`phosphoric.cfg`)

**Enregistrer la configuration** (save configuration) writes `phosphoric.cfg` in the
current folder (or the file given by `--config FICHIER`), one `clé=valeur` line per
setting:

| Keys | Content |
|---|---|
| `a=` … `d=` | images in the drives |
| `protection_a=oui` … | write-protected drives |
| `cassette=` | inserted tape |
| `cassette_rapide=oui/non` | direct injection at launch |
| `imprimante=non/texte/mcp40`, `imprimante_fichier=` | printer |
| `joystick=aucun/clavier/manette` | joystick |
| `clavier=qwerty/azerty` | keyboard |
| `carte.<id>=oui/non` | present cards (`microdisc`, `jasmin`, `loci`, `acia`, `dtl2000`, `mageco`, `oricon`, `sp0256`, `mea8000`, `hostfs`) |
| `<id>.<réglage>=valeur` | card settings that differ from the default (e.g. `loci.mode=usb`, `acia.adresse=0380`) |

Lines the menu does not know are kept.

When it is read back, at the next launch:

- **the command line takes precedence**; cards from the file are only added for
  those the command line does not mention (`--no-config-cards` ignores them);
- the file is **not read in `--headless`**, except with an explicit
  `--config FICHIER`;
- `--no-config` or `PHOSPHORIC_NO_CONFIG=1` ignore it;
- old keys are still read (`interface_disque=`, `rom_disque=`,
  `carte.loci_emu`, `carte.loci_hw`, `loci.mode=réelle`, `loci.pont`…).

## 7. Miscellaneous

- `--menu-screenshot FICHIER` saves an image of the menu (PPM 640 × 640) at the end
  of the run, whether the menu is open or not.
- In the web version, the menu works (media, peripherals), but cards can only be
  changed at launch.
- The other function keys (F5 reset, F6 quick media, F8 LOCI button, F9 debugger,
  F12 screenshot…) are described in the [user guide](README.md).
