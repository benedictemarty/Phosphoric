# Digitelec DTL 2000 — source documentation

Archive folder for the faithful emulation of the **Digitelec DTL 2000** modem
(V23 board, PIA 6821 + ACIA 6850, memory-mapped `$03F8-$03FD`).
See the implementation: `src/io/dtl2000.c`, `include/io/dtl2000.h`,
tests `tests/unit/test_dtl2000.c`, example `examples/dtl2000-test.bas`.

## Contents

| File | Description |
|------|-------------|
| [`registres-ocr.md`](registres-ocr.md) | **Frozen reference** of the registers (PIA + ACIA), bit-by-bit values, modes, page 3 conflict — consolidated from the OCR |
| [`contexte.md`](contexte.md) | Initial context document (press + period manuals), with the points that were still "to be confirmed" |
| [`ocr/`](ocr/) | Raw OCR (Tesseract `fra`, 300 dpi) of the 22 pages of the period manuals |

## Provenance

The 3 period manuals (image scans without a text layer) were retrieved from
the **apple2.org.za** mirror (Apple II Documentation Project), then rasterised
(300 dpi, greyscale) and OCR'd with **Tesseract `fra`**:

- `prog_v23-*.txt` — « Programmation carte DTL V23 » (*Programming the DTL V23 board*, 7 pages) → **exact
  register values** (POKE/PEEK); this is the primary source for the constants.
- `notice-*.txt` — « Notice d'utilisation » (*User guide*, 5 pages) → getting started, simulated
  Minitel keys, machine-to-machine communication software.
- `manuel-*.txt` — generic RS232/V24 manual (10 pages) → DIP switches
  + mapping of circuits 108/105/106/109 (DTR/RTS/CTS/DCD).

> ⚠️ The 3 PDFs are the **Apple II / RS232** variant, *not* Oric. The chips are
> identical (PIA EF6821 + ACIA EF6850) and the layout of the 6 bytes is
> offset-for-offset the same as the Oric `$03F8-$03FD`: the values carry over
> directly (only the base address changes, Apple `$C0n8` → Oric `$03F8`).

## Not included (on purpose)

- The source **PDFs** (~19 MB) and the 300 dpi **PNGs**: too large for the
  repository and still available online (URLs in `contexte.md` §12).
- No **original Oric software** for the DTL 2000 could be located (the bundled
  cassette is not publicly archived; Loritel/Tortosa drive the Minitel
  over RS-232, not the `$03F8` board). Hence the home-made test program
  `examples/dtl2000-test.bas`, derived from the POKE/PEEK sequences in the OCR.
