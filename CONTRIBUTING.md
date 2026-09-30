# Contributing to Phosphoric

Thank you for your interest in contributing to the Phosphoric project.

## Building from Source

### Prerequisites

```bash
# Debian/Ubuntu
sudo apt-get install build-essential libsdl2-dev

# Fedora
sudo dnf install gcc make SDL2-devel

# Arch
sudo pacman -S base-devel sdl2
```

### Build

```bash
# Standard build with SDL2
make SDL2=1

# Debug build
make DEBUG=1 SDL2=1

# Run tests
make tests
```

The `Makefile` is the only build system (the former `CMakeLists.txt`, which no
longer compiled, was removed in 2.1.3).

Objects are built **out of the source tree**, one directory per configuration:
`build/<config>/` (e.g. `build/host-sdl1-http0-cast0-midi0-tls1-tui0-lociemu/`).
Switching options (`SDL2=0`, `HTTPAPI=1`, `DEBUG=1`, LOCI backend…) never mixes
objects, so no `make clean` is needed between variants. Final binaries
(`oric1-emu`, tools) are copied to the repository root, where scripts expect
them; unit-test binaries stay in `build/<config>/tests/`. `make clean` removes
`build/` entirely.

## Project Layout

| Directory | Contents |
|-----------|----------|
| `src/cpu/` | 6502 CPU emulation |
| `src/memory/` | Memory system + ROM/RAM banking |
| `src/io/` | VIA 6522, keyboard, cassette, microdisc |
| `src/video/` | Text/HIRES rendering + export |
| `src/audio/` | AY-3-8910 PSG + SDL2 output |
| `src/storage/` | TAP, Sedoric, FDC WD1793 |
| `src/hostfs/` | Host filesystem sharing |
| `src/utils/` | Logging, config |
| `include/` | Public headers |
| `tests/unit/` | Unit tests |
| `tools/` | Conversion utilities |

## Coding Style

- **Language**: C11 (gcc/clang)
- **Indentation**: 4 spaces (no tabs)
- **Braces**: K&R style (opening brace on same line)
- **Naming**: `snake_case` for functions and variables, `UPPER_CASE` for macros
- **Headers**: Include guards with `#ifndef HEADER_H` / `#define HEADER_H`
- **Comments**: C-style `/* ... */` for block, `//` for single line
- **Line length**: 100 characters max

### Example

```c
void via_write(via6522_t* via, uint8_t reg, uint8_t data) {
    switch (reg) {
    case VIA_ORB:
        via->orb = (data & via->ddrb) | (via->orb & ~via->ddrb);
        break;
    default:
        break;
    }
}
```

## Testing

Every change should include tests. The test framework uses simple C macros:

```c
#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-50s", #name); name(); tests_passed++; printf("PASS\n"); } while(0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) { /* fail */ } } while(0)
```

### Running Tests

```bash
make tests              # All test suites (builds the tools first)
make tests-strict       # Same, and fails if a test is skipped without an
                        # allowed reason (tests/allowed_skips.txt) — used by CI
make test-cpu           # CPU tests only
make test-audio         # Audio tests only
make valgrind           # Memory leak check
make valgrind-core      # Core suites under Valgrind (CI job)
make static-analysis    # Compiler warnings analysis
make SANITIZE=1 tests   # Whole suite under ASan + UBSan (build/<config>-san)
make fuzz               # libFuzzer (clang), FUZZ_TIME seconds per target
```

### Sanitizers and Fuzzing

- `SANITIZE=1` builds with AddressSanitizer + UndefinedBehaviorSanitizer in its own
  build directory; any UBSan report aborts the program, so the test that triggers
  it fails. The only exemption is `third_party/stb_image_write.h` (left intact),
  for signed shifts, in its own compilation unit.
- `tests/fuzz/` holds one `LLVMFuzzerTestOneInput()` per file reader: `.dsk`
  (`sedoric_load` + FDC reads), `.tap`, `.ost`, `.mid`, symbol files and
  `phosphoric.cfg`. `make fuzz` runs them with libFuzzer; `make test-fuzz-replay`
  (part of `make tests`, gcc) replays the synthetic seeds (`tools/fuzz_seeds.sh`)
  and `tests/fuzz/regressions/<target>/`.
- When `make fuzz` finds an input, it is written to `build/fuzz/crashes/`: fix the
  bug, then copy the input into `tests/fuzz/regressions/<target>/` so it replays
  in every run of the suite.
- CI (`linux-ci.yml`, job `sanitizers`) runs `make SANITIZE=1 tests-strict` and
  `make fuzz FUZZ_TIME=30`.

### Adding a Test

1. Add test function in the appropriate `tests/unit/test_*.c` file
2. Add `RUN(test_name);` in `main()`
3. Verify with `make test-<suite>`

A new unit-test suite is declared with the `UNIT_TEST` macro of the `Makefile`
(sources are compiled once per configuration and linked from `build/`):

```make
TEST_FOO_SRCS = tests/unit/test_foo.c src/io/foo.c src/utils/logging.c
$(eval $(call UNIT_TEST,test-foo,test_foo,$(TEST_FOO_SRCS),,))
```

then add `test-foo` to the `tests` target. A test that is skipped for a new
legitimate reason must be justified in `tests/allowed_skips.txt`.

## Development Workflow

1. Create a feature branch from `main`
2. Write tests first (TDD encouraged)
3. Implement the feature
4. Run `make tests` to verify all tests pass
5. Update CHANGELOG with your changes
6. Commit with a descriptive message
7. Submit a pull request

## Language Mirrors

The project is published in two languages from one history:

| Branch | Language | Pushed to |
|---|---|---|
| `main` | French (source of truth) | Framagit, self-hosted (`origin`) |
| `main-en` | English | GitHub, Codeberg (as their `main`) |

`main-en` is `main` plus translation commits: only the documentation (`*.md`)
and the **comments** of the source code differ. Code, identifiers, program
messages and tests are identical. To synchronise after new work on `main`:

1. `git checkout main-en && git merge main`
2. translate the documents and comments changed by the merge;
3. `tools/check_comment_only_diff.py` (no argument: every code file that
   differs from `main`) must report `0 en échec`;
4. `make tests`, commit in English, push `main-en` to `github main` and
   `codeberg main`.

`make test-comment-diff` self-tests the checker.

## Commit Messages

Follow conventional commit format:

```
feat: add joystick support
fix: correct HIRES attribute parsing
docs: update user guide with disk loading
test: add envelope shape tests for PSG
```

## Reporting Issues

When reporting bugs, please include:
- Steps to reproduce
- Expected vs actual behavior
- ROM/program used (if applicable)
- Build configuration (SDL2, DEBUG, etc.)

## Command Line and `main()`

`main()` (in `src/main.c`) only sequences named steps: `cli_parse_args()`
(`src/cli/cli_args.c`, the getopt switch) fills a `cli_opts_t`
(`include/cli/cli_opts.h`, defaults in `cli_opts_init()`), then
`main_setup_*()` steps configure the machine, `emulator_run()` runs it and
`main_finish()` writes the outputs. Each step returns -1 to continue, or the
program's exit code.

Before changing the parser or a set-up step, keep a reference binary and
compare: `make test-cli-golden GOLDEN_REF=/path/to/old/oric1-emu` replays
`tests/cli_golden/cases.txt` on both and fails on any difference in exit code,
stdout, stderr or produced files. Add a case there for every new option.

## Architecture Notes

- The emulator is designed to be modular: each subsystem (CPU, memory, VIA, video, audio, storage) is independent
- I/O routing uses callbacks: `memory_set_io_callbacks()` wires read/write to VIA and peripherals
- SDL2 is optional: the emulator can run headless for testing
- The PSG (AY-3-8910) runs at 1 MHz clock, divided by 8 for tone/noise and 16 for envelopes
- Reference implementation: Oricutron (for PSG timing, ULA rendering, keyboard mapping)
