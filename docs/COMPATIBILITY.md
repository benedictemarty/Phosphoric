# Compatibility List — Phosphoric v1.110.0-alpha

Last updated: 2026-11-01 (sweep of 1318 tapes, corrected in 2.31.10)

---

## Operating systems

| Program | Format | Model | Status | Notes |
|-----------|--------|--------|--------|-------|
| BASIC 1.0 ROM | .ROM | ORIC-1 | Working | Full BASIC interpreter |
| BASIC 1.1 ROM | .ROM | Atmos | Working | Auto-detection via JMP $ECCC |
| Sedoric V4.0 | .DSK | ORIC-1/Atmos | Working | Boot, keyboard, DOS commands |
| Sedoric V3.0 | .DSK | ORIC-1/Atmos | Working | Boot and DOS commands |

---

## Tape games (.TAP) — 26/26 working

| Program | Format | Size | Status | Notes |
|-----------|--------|--------|--------|-------|
| 007 | .TAP | 28 KB | Working | Fast load $0180-$7148 |
| Acheron's Rage | .TAP | 43 KB | Working | Fast load |
| Aigle d'Or | .TAP | 54 KB | Working | Fast load |
| Andromeda | .TAP | 35 KB | Working | Fast load |
| Airline | .TAP | 24 KB | Working | Fast load |
| Atlantis | .TAP | 46 KB | Working | Loads OK |
| Author | .TAP | 10 KB | Working | Loads OK |
| Bat Fly | .TAP | 20 KB | Working | Loads OK |
| Bataille Navale | .TAP | 6 KB | Working | Fast load |
| Breakout | .TAP | 4 KB | Working | Fast load $15BB-$2637, BASIC |
| Bricky | .TAP | 11 KB | Working | Fast load |
| Centipede | .TAP | 5 KB | Working | Loads OK |
| Chopper | .TAP | 40 KB | Working | Fast load (SCREEN mode) |
| Citadel | .TAP | 49 KB | Working | 32 KB loaded |
| Cite | .TAP | 47 KB | Working | Loads OK |
| Defender | .TAP | 58 KB | Working | Fast load $69FF-$80C7 |
| Johnny | .TAP | 17 KB | Working | Loads OK |
| Loi du West | .TAP | 47 KB | Working | Loads OK |
| Manic Miner | .TAP | 39 KB | Working | Loads OK |
| Manic Miner (proper) | .TAP | 39 KB | Working | 24 KB loaded |
| Pasta Blasta | .TAP | 22 KB | Working | Fast load |
| Psy | .TAP | 33 KB | Working | Fast load |
| Poker (poker-asn.tap) | .TAP | — | Working | Correct HIRES graphics |
| Soccer Manager | .TAP | 44 KB | Working | **Custom loader + EOR #$55 decryption → `--tape-signal`** (fails with fast load, as in Oricutron) |
| Spooky | .TAP | 24 KB | Working | Loads OK |
| Spooky (cracked) | .TAP | 24 KB | Working | Fast load |

---

## Disk games (.DSK) — 11/11 working

| Program | Format | Size | Geometry | Status | Notes |
|-----------|--------|--------|-----------|--------|-------|
| 3D Fongus | .DSK | 1 MB | — | Working | Sedoric boot |
| Aigle d'Or | .DSK | 537 KB | 2 sides x 42 tracks | Working | Sedoric boot |
| Citadelle | .DSK | 141 KB | 1 side x 22 tracks | Working | Sedoric boot |
| Le Manoir du Dr Genius | .DSK | 141 KB | 1 side x 22 tracks | Working | Sedoric boot |
| Manic Miner (Telestrat EN) | .DSK | 537 KB | — | Working | Sedoric boot |
| Manic Miner (Telestrat FR) | .DSK | 537 KB | — | Working | Sedoric boot |
| Oric Chess | .DSK | 141 KB | 1 side x 22 tracks | Working | Sedoric boot |
| Manic Miner | .DSK | 269 KB | — | Working | Sedoric boot |
| Pasta Blasta | .DSK | 1.2 MB | — | Working | Sedoric boot |
| Sedoric V4.0 | .DSK | 1 MB | 2 sides x 80 tracks | Working | Sedoric system |
| Sedoric V3.0 | .DSK | 1 MB | — | Working | Sedoric system |

---

## Demos / test programs

| Program | Format | Status | Notes |
|-----------|--------|--------|-------|
| Hello World | .TAP | Working | Text mode, PRINT output |
| Sound demo | .TAP | Working | PSG tone/envelope generation |
| HIRES graphics demo | .TAP | Working | 240x200 rendering |
| Explode | .TAP | Working | CLOAD, gameplay working |

---

## Compatibility rate

| Category | Tested | Working | Rate |
|-----------|--------|-------------|------|
| Tapes (.TAP) | 25 | 25 | **100%** |
| Disks (.DSK) | 11 | 11 | **100%** |
| Demos / tests | 4 | 4 | **100%** |
| **Total** | **40** | **40** | **100%** |

---

## Sweep of an archive of 1318 tapes (2.31.8, corrected in 2.31.10)

`tools/tap_sweep.sh DIR [JOBS]` loads every `.tap` of a folder the normal way
(automatic `CLOAD""`, no `-f`), 25 M cycles on the Atmos ROM, then runs the
suspicious cases again on the ORIC-1 ROM. Run on `~/oriclib/games/tap` (local
archive, not versioned) on 2026-11-01.

**Correction (2.31.10)**: the summary published in 2.31.8 claimed "0 stop of
the 6502". That was wrong: the tool read the emulator logs on stderr, while
they go to stdout, so it saw no stop at all. Summary redone with the fixed
tool:

| Atmos / ORIC-1 | Count | Reading |
|----------------|-------|---------|
| OK | 1053 | does not prove the program is right, only that it does not crash |
| `Ready` / `Ready` | 160 | mostly tapes with no auto-run flag (normal behaviour); 2 interrupt-driven music players (`SPACEMU2`, `POSDEMO`) |
| `Ready` / OK | 2 | ORIC-1 programs |
| stop / OK | 74 | ORIC-1 programs that crash on the Atmos ROM and work on ROM 1.0 |
| stop / stop | 16 | 13 `os*` tapes (music players) plus `vache.TAP`, `os2kata`, `os2vwtit`: see below |
| stop / `Ready` | 1 | `assdesasm`: Oricutron crashes too on Atmos |
| `ERROR` / OK | 10 | ORIC-1 programs, and 3 false positives (the word is part of the text) |
| `ERROR` / `ERROR` | 2 | including `ogp-nim.tap`, a damaged file (BASIC line chain broken after line 710) |

**Cross-check with Oricutron 1.2.0** (run under Xvfb, same tapes, Phosphoric
ROMs):
- `osarkpan`, `oscomman`, `osqix`, `vache`: Oricutron stops in its debugger
  **at the same address** as Phosphoric (`$B5AC`, `$B5AC`, `$0808`, `$5057`):
  defects of the files, not of the emulator. The 10 `os*` tapes stopping at
  `$B5AD` follow the same path (`JSR $023C` into the ROM, then return onto a
  `$02` byte in the character set area).
- HIRES screens: the 282 captures with no readable text were reviewed by eye
  on contact sheets (`tools/contact_sheet.py`). 23 were blank or stuck on
  "please wait"; 13 show their title with a longer load (80 M cycles). Of the
  10 left, 7 give the same screen under Oricutron (including 2 ORIC-1 programs
  and 1 tape waiting for a next file).

**Unresolved divergences** (no real ORIC to decide):
- `LOADER2.TAP` on the ORIC-1 ROM: Phosphoric stops the 6502 at `$B585` (also
  with `--tape-signal`, read by the real ROM); Oricutron returns to `Ready`.
  Neither shows the game.
- `ALCHIMIE.tap` on the Atmos ROM: Phosphoric runs the program; Oricutron
  crashes in the ROM (`$E786`, tape routines, possibly an effect of its fast
  loading).

Limit: the criterion is the CPU state, the text screen and, for HIRES, a visual
review with no reference; wrong sound or a later hang are not detected.

`make test-game-compat` also reads this archive (`ORICLIB_TAP`, default
`~/oriclib/games/tap`): 10 titles with a known intro screen, skipped when the
folder is absent.

---

## Unit test results

`make tests`: **908 tests, 100% pass** (35 suites), checked on 2026-08-30.

The per-suite breakdown changes with every version; the authoritative source
(total count + status per component) is maintained in **VERSION_TRACKING**
and **CIRRUS_OS** at the root of the repository. See also `make test-<suite>` (cpu,
memory, io, storage, system, video, audio, debugger, savestate, atmos,
joystick, printer, mcp40, renderer, trace, profiler, rominfo, serial, loci…).

---

## ROM compatibility

| ROM | Size | RESET vector | Model | Status |
|-----|--------|--------------|--------|--------|
| basic10.rom (BASIC 1.0) | 16384 bytes | $EA59 | ORIC-1 | Valid |
| basic11b.rom (BASIC 1.1) | 16384 bytes | $ECCC | Atmos | Valid |
| microdis.rom (Microdisc) | 8192 bytes | N/A | Overlay $E000 | Working |

---

## Emulation accuracy

| Component | Accuracy | Notes |
|-----------|----------|-------|
| 6502 CPU | Bus-cycle-accurate | 151 official opcodes, BCD, indirect JMP bug |
| VIA 6522 | Working | Timers, interrupts, port callbacks, CB1 edge |
| AY-3-8910 PSG | Accurate | Oricutron DAC curve, clock dividers |
| ULA Video | Working | Text + HIRES, serial attributes, PAL timing |
| WD1793 FDC | Working | Type I/II commands, sector read/write; persistent in-game saves (--disk-writeback / savestate) |
| Keyboard | Accurate | 8x8 matrix via VIA + PSG Port A |
| IJK joystick | Working | PSG Port A active low, keyboard + gamepad |
| Printer | Working | Centronics: data on ORA, STROBE on PB4, ACK on CA1 (2.31.4; before: CA2, wrong) |
| MCP-40 | Working | 8 commands, 4 colours, BMP export |
| ACIA 6551 | Working | Serial $031C-$031F, loopback/tcp/pty/com/modem backends |
| Digitelec DTL 2000 | Faithful | PIA 6821 + ACIA 6850 at $03F8-$03FD, OCR'd registers |
| Mageco MIDI | Working | MC6850 at $03FE-$03FF, 31250 baud, `--mageco` (original card, forum t=2525 p.1) |
| ORICON MIDI | Working | MC6850 at $031C-$031D + clock generator $031E-$031F, `--oricon` (modern reboot, p.3, LOCI-compatible) |
| Real-time MIDI | Working | Host MIDI port `--mageco midi[:TARGET]` (`MIDI=1` build): ALSA (Linux, verified), CoreMIDI/WinMM (written, not verified) |
| .mid (SMF) player | Working | `--mageco smf:FILE[:loop]` replays a .mid file into the Oric as paced MIDI IN (format 0/1, tempo map) |

---

## Known limitations

- Telestrat support is not implemented yet
- Some copy-protected programs may fail to load
- Code coverage has not been formally measured (estimate > 80%)
- Visual tests (HIRES rendering, colours) are validated by headless screenshot
- The game's disk writes (in-game saves) are only written to the .dsk file
  with --disk-writeback (opt-in, overwrites in place); otherwise they live in the
  session or in a savestate (.ost captures the disk image)
- The Mageko MIDI card occupies $03FE/$03FF: on real hardware, forum t=2525
  reports a risk of address conflicts with other expansions (the emulator
  warns if the Microdisc is present). The real-time MIDI backend `--mageco midi`
  requires a `MIDI=1` build; the ALSA branch (Linux) is verified, the
  CoreMIDI (macOS) / WinMM (Windows) ports are written but not verified on real hardware

---

## Reporting compatibility

If you test a program not listed here, please report:
- Program name and format (.TAP/.DSK)
- Whether it loads and runs correctly
- Any visual or audio glitch observed
- Steps to reproduce any problem
