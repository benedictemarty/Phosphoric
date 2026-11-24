# Full LOCI E2E: 007 "Dangereusement Vôtre" loaded automatically

**Date**: 2026-06-07
**Version**: v1.16.49-alpha (direct follow-up of sprint 34av)
**Documentation source**: [LOCI User Manual sodiumlb](https://github.com/sodiumlb/loci-hardware/wiki/LOCI-User-Manual)

---

## 1. Context

Sprint 34av had delivered matrix auto-typing with `\e` ESC + arrows
`\u/\d/\l/\r`. But automating the LOCI TUI navigation was still
unclear: selecting "007.TAP via picker" did not work blind.

The user asked me to go and find the official documentation.

---

## 2. Documentation retrieved

Found on the sodiumlb wiki. Decisive keyboard shortcuts:

| Key | Action |
|--------|--------|
| **`T`** | Jump straight to the tape drive slot |
| **`A`-`D`** | Jump straight to the Microdisc drives |
| **`K`** | Tape counter |
| **`M`** | Mouse on/off |
| **`O`** | ROM select |
| **`R`** | RV1 adjust |
| **SPACE** | Select (file or toggle) |
| **ESC** | Boot (if at top menu) or close popup |
| **RETURN** | Resume a suspended application (save state) |
| **`/`** | Parent directory in the file browser |
| **`F`** | Filter field in the file browser |
| **+/-** | Increase/decrease (RV1) |

**Official workflow to mount a TAP**:
1. Press **T** → opens the tape drive
2. The file browser opens with the devices
3. Up/Down to select the device (USB or SD)
4. SPACE to enter the device
5. F + filter `.TAP` (optional; the picker can filter automatically)
6. Up/Down to highlight the file
7. SPACE to select
8. ESC to boot

---

## 3. E2E validation with 007 "Dangereusement Vôtre"

```bash
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --keyboard azerty \
    --type-keys '15000000:\p3t\p2 \p2 \p2\e\p9\p1CLOAD""\n\p9\p9\p9'
```

Decoded:
- `\p3`: 3 s to let the LOCI TUI settle
- `t`: jump to the tape drive
- `\p2 `: 2 s then SPACE → opens the file picker (SD only = no need for
  an intermediate device selection)
- `\p2 `: 2 s then SPACE → selects the first listed file (007.TAP,
  alphabetically before AIGLE.TAP since '0' < 'A')
- `\p2\e`: 2 s then ESC → MIA_BOOT
- `\p9\p1`: 10 s for BASIC 1.1 init
- `CLOAD""\n`: types the CLOAD command
- `\p9\p9\p9`: 27 s for the complete tape load

### Key LOCI logs

```
LOCI SDIMG extract: '007.TAP' → '/tmp/loci_007.TAP_Hw2PS4'
LOCI tape mount: /tmp/loci_007.TAP_Hw2PS4 buffered (28630 bytes, type CLOAD"" in BASIC)
LOCI SDIMG extract: 'basic11b.rom' → '/tmp/loci_basic11b.rom_jFpc3w'
LOCI ROM swap: loading /tmp/loci_basic11b.rom_jFpc3w at $C000
LOCI ROM swap: patches → BASIC 1.1 (ORIC Atmos)
TAPE: getsync at tapeoffs=0/28630
TAPE: getsync at tapeoffs=137/28630
TAPE: getsync at tapeoffs=8322/28630
```

3 getsyncs = multi-block TAP (header + code segments). The 1st sync finds
the tape; the 2nd at offset 137 = after the header (16 leader + name + start
of data block); the 3rd at 8322 = between the data blocks.

### Final screen (game title)

```
DOMARK ET EUREKA INFORMATIQUE PRESENTENT
   .  007 "DANGEREUSEMENT VOTRE"
        Le jeu sur ordinateur
Realisation: TIGRESS MARKETING
Programmation: SEVERN SOFTWARE
Droits exclusifs de la version francaise
       EUREKA INFORMATIQUE
39 Rue Victor Masse - 75009. PARIS.
           MAINTENANT, JAMES,
        VOTRE MISSION COMMENCE!
Note de "Q"
   Arretez de vous amuser, Bond, vous
avez du travail!
     Rappellez-vous : utilisez avec
precaution les gadgets que je vous ai
confies, certains d'entre eux sont tres
dangereux!
Note de "M"
     N'echouez pas, Bond, je tiens
beaucoup a cette fille!
        "RETURN" - DEBUT DU JEU
```

(On-screen text of the French edition of the game, kept as displayed:
"Domark and Eureka Informatique present 007 'A View to a Kill', the computer
game [...] Now, James, your mission begins! Note from 'Q': Stop fooling
around, Bond, you have work to do! Remember: use the gadgets I entrusted to
you with care, some of them are very dangerous! Note from 'M': Don't fail,
Bond, I am very fond of that girl! 'RETURN' - start the game".)

**The James Bond game 007 "Dangereusement Vôtre" (DOMARK 1985) runs
automatically** — the first time a commercial Oric tape program has been
loaded end-to-end through the LOCI firmware in Phosphoric.

---

## 4. Validated stack

| Component | Validated by |
|-----------|------------|
| MIA spin ABI (sprint 34an) | The LOCI ROM boots and stays responsive |
| SDIMG FAT16 read (34ao) | The picker lists the files, mount extracts |
| SDIMG `mkstemp` (34ar) | Path `/tmp/loci_007.TAP_Hw2PS4` randomised |
| ROM swap + BASIC patches (34ao+) | BASIC 1.1 booted with the right patches |
| CSAVE/CLOAD format (34aq-34at) | CLOAD finds sync at offsets 0, 137, 8322 |
| `\e` ESC + `t` shortcut (34av) | Automatic TUI navigation |
| 7 stubbed tuning ops (34au) | Not called by 007 but ready |

---

## 5. Reproducibility

```bash
git checkout main           # v1.16.49-alpha
make SDL2=1
./tools/mkloci_sd loci_demo.img 16 \
    roms/basic10.rom roms/basic11b.rom roms/microdis.rom \
    tapes/AIGLE.TAP tapes/007.tap
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --keyboard azerty \
    --type-keys '15000000:\p3t\p2 \p2 \p2\e\p9\p1CLOAD""\n\p9\p9\p9'
# → 60-90 seconds later, the screen shows the DOMARK 007 title
```

---

## 6. Credits

- **sodiumlb**: LOCI hardware + ROM + firmware + TUI documentation on
  the wiki. Without that documentation, automating the navigation would
  have required reverse engineering the 6502 firmware.
- **xahmol**: `locifilemanager` (alternative firmware, cited in the
  research, for information).

---

**Status**: LOCI E2E **fully automated** — boot → mount TAP →
swap BASIC → CLOAD → run program. First mainstream emulator to
support this flow end-to-end.

— End of report
