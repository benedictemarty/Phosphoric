# Deterministic input record / replay ("TAS" movie)

Phosphoric can **record** the keyboard input of a session and **replay** it
identically. The only non-deterministic input of the emulation is the keyboard
matrix (8 bytes); by capturing it per frame and replaying it bit for bit, a
session is reproduced exactly.

Use cases: tool-assisted runs (TAS), bug reproduction, and **CI regression**
(record once → replay headless → compare against a reference screenshot).

## Usage

```bash
# Record (interactive, SDL)
./oric1-emu -r roms/basic11b.rom --record session.phm

# Replay (the live keyboard is ignored)
./oric1-emu -r roms/basic11b.rom --replay session.phm

# CI regression: replay headless then capture the final screen
./oric1-emu -r roms/basic11b.rom -n --replay session.phm --screenshot out.ppm
```

In headless mode, replay **exits automatically** once the movie is exhausted.

## Determinism guarantee

**Replay is bit-deterministic**: replaying the same movie always produces the
same output (proven: two replays → byte-identical screenshots). This is what
makes CI regression reliable — you commit a movie + a reference screenshot,
and CI replays + compares.

Determinism relies on: same ROM/model, fixed RAM init (Oricutron-compatible),
CPU deterministic at bus-cycle level. The movie stores the model (`model 0|1`) and a
warning is issued if the replay model differs.

## File format (text, diffable)

```
PHOSPHORIC-MOVIE 1
model 1
F 0 ff ff ff ff ff ff ff ff
F 1 ff ff ff ff ff f7 ff ff
F 5 ff fb ff ff ff ff ff ff
```

- `F <frame> <m0..m7>`: frame index + the 8 matrix bytes (hex, active-low).
- **Changes only**: a line is written only when the matrix
  changes; replay keeps the last state between two changes.

## Scope and limitation

- **Exact** for interactive SDL input: the keyboard is polled once per
  frame, so the matrix is constant during each frame — per-frame sampling
  is lossless.
- **`--type-keys`** injects keys in the middle of a frame (cycle granularity).
  A movie recorded from a `--type-keys` run replays **deterministically**, but
  not necessarily identically to the live `--type-keys` run (sub-frame
  injection is quantised to the frame). For reproducible scripted input,
  prefer recording and then replaying the movie.
