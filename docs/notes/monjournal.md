# My journal — approach & methodology on Phosphoric

> File kept **in real time**: it describes how I work on the Phosphoric
> emulator (investigation, implementation, tests), the **difficulties** encountered
> and how I resolve them. Goal: to let you understand and reproduce my
> approach.
>
> Rule of conduct I apply here: **measure, do not invent, do not assume.**
> When I don't know, I say so; when I assert something, it is because I measured it.

---

## Method principles (constant)

1. **Read before writing.** I locate the existing code (grep + targeted reading)
   before proposing anything. I never describe a feature without
   having seen the function in the source.
2. **Measure rather than assume.** A hypothesis ("the screen should show X")
   only has value once verified by an actual run (memory dump,
   screenshot, test).
3. **Isolate the cause before blaming a component.** A test or build failure
   is first suspected to be an artefact (incremental build, stale objects) before
   being blamed on the business code. I prove it with `git stash` / `make clean`.
4. **Tell false positives from real problems.** Diagnostics from the isolated LSP
   linter (unresolved include paths) are not compilation errors;
   only the real `make` build is authoritative.
5. **Trace everything.** Tests + CHANGELOG + tracking files on every change,
   in accordance with the project rules.

---

## Current session — Adding a "text screenshot"

### 1. The request
"Is there a text screenshot?" then "implement both":
- **A** — export the screen as an **ANSI true-colour image** (pixels → ANSI background
  colours in a terminal);
- **B** — export the **actual text content** of the screen (the characters displayed).

### 2. What I found while exploring (measured, not assumed)
- `video_export_ascii()` **already existed** in `src/video/export.c` but:
  - was **not** wired to the CLI (`--help` did not mention it);
  - was called **only** by the tests;
  - produces an **ANSI image** (each pixel = a coloured space), not a text dump.
- The **actual text content** (option B) did not exist.
- The `textmode_*` helpers (`src/video/textmode.c`, screen base `$BB80`, 40×28)
  exist but are **orphaned** (no header, no caller) → I don't rely
  on them, I put the logic in the export module.

### 3. Design (minimal, testable, no unnecessary surface)
Two new functions in `export.c` / `export.h`:
- `video_export_ascii_file(vid, filename, sx, sy)` — file wrapper around the existing function (A);
- `video_export_screen_text(memory, fp)` — reads `$BB80` (40×28), decodes each byte
  with `byte & 0x7F` (masks the inverse-video bit), replaces control codes
  (< 0x20) with a space, rtrims trailing spaces (B).

Two CLI output options, modelled on `--screenshot`:
- `--screenshot-text FILE` (B);
- `--screenshot-ansi FILE` (A).

### 4. Difficulties encountered and how I resolved them

| # | Difficulty | Diagnosis | Resolution |
|---|-----------|-----------|-----------|
| 1 | Headless build fails at **link** (undefined `SDL_*`) | `.o` objects from an earlier SDL2 build mixed into a headless link | `make clean` then clean rebuild |
| 2 | Bursts of **clang** diagnostics "file not found / unknown type" | False positives: the isolated LSP does not have `-Iinclude` | Ignored; **the real `make` build is the authority** |
| 3 | **Nearly empty text dump** at 2,000,000 cycles | Measuring the raw `$BB80` content = `$FF` everywhere → screen not yet filled, not a decoding bug | Measured at 3M/5M/8M cycles → stable, readable boot screen |
| 4 | The `©` of "© 1983 TANGERINE" comes out as `` ` `` | ORIC charset: code `0x60`. My decoding assumes standard ASCII | **Accepted and documented limitation** (ASCII approximation), not a hidden defect |
| 5 | `test-control-dispatch`: **7 failures** in `make tests` | Suspected artefact rather than regression | Proven by measurement: `make clean && make tests` (canonical sequence) → **0 failures**. The failure only appears if preceded by a `make SDL2=0` (mix of headless/SDL2 objects). Reproduced **identically on a clean HEAD** (`git stash` + same sequence → 19/7) → **pre-existing Makefile artefact, NOT my code nor a regression** |

### 5. Validation by measurement
- `make test-video`: **16/16** (including 2 new tests: `test_ascii_export_file`,
  `test_screen_text_export`).
- Real Atmos run at 3M cycles, `--screenshot-text`:
  ```
                                      CAPS
    ORIC EXTENDED BASIC V1.1
    ` 1983 TANGERINE

     37631 BYTES FREE
  ```
- `--screenshot-ansi`: ~264 KB, `ESC[48;2;R;G;Bm` sequences present, screen
  structure visible in the terminal.
- Full suite `make clean && make tests` (canonical sequence): **0 failures**.
- Side discovery (measured): a `make SDL2=0` followed by `make tests` makes
  `test-control-dispatch` fail (7/26) through object mixing; **pre-existing
  Makefile artefact** (reproduced on a clean HEAD), unrelated to this feature.

### 6. What I don't know / honest limits
- The text dump **assumes the standard ORIC character set**; a charset
  redefined by a program will not be resolved (the bytes are still interpreted as
  ASCII). This is documented in the function header.
- The dump always reads the 28 lines of `$BB80`, whatever the mode (TEXT/HIRES):
  in HIRES only the last 3 text lines are actually on screen, but the
  `$BB80` buffer is read as is.
- ~~The options are output-only, not yet "at a given cycle".~~ **Filled**:
  see iteration 2 below.

### 7. Iteration 2 — "at a given cycle" variants (v1.93.0-alpha)
The limit noted in point 6 ("no `--screenshot-at` equivalent") is **filled**:
- `--screenshot-text-at C:FILE` and `--screenshot-ansi-at C:FILE`, modelled on the
  existing `--screenshot-at`, **reusing** the already shared helper
  `cli_split_cycles_file()` (v1.91.1) — no duplication of the `CYCLES:FILE` parsing.
- **Method**: I started from the exact code of `--screenshot-at` (parsing + block in the
  loop) and derived variants from it, to guarantee **identical** error behaviour
  (format without `:` → fatal rc 1, same message via `optname`).
- **Measured**: `--screenshot-text-at 3000000:FILE` writes the same readable Atmos screen
  at cycle 3015456; malformed input → rc 1 verified **without a pipe** (a `| grep` would have hidden the
  real return code — trap avoided).
- **Tests**: CLI safety net `test-cli-parsing` **29/29** (+4 cases: 2 fatal malformed + 2
  positive "non-empty file" checks), modelled on the `--screenshot-at` cases.

---

_Last updated: iteration 2 (text/ANSI screenshot at a given cycle, v1.93.0)._
