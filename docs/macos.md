# Building on macOS

Phosphoric builds and runs natively on macOS (Intel and Apple Silicon) using
Apple's Command Line Tools (clang) and SDL2 installed through Homebrew.

> **Status**: **verified on real macOS** (Apple Silicon) by the
> `macos-build` CI (`.github/workflows/macos-build.yml`, runner `macos-latest`):
> headless build (`SDL2=0`), full build (`SDL2=1`), **full test suite**
> and `HTTPAPI=1` build all pass green. The CI also exposed several
> macOS discrepancies that are invisible on Linux (see "Portability details").

## Dependencies

```bash
# Command Line Tools (provides clang, make, git)
xcode-select --install

# Homebrew : https://brew.sh
brew install sdl2 pkg-config      # display/audio/keyboard
brew install openssl@3            # optional: PicoWiFi TLS (PICOTLS)
```

`gcc` on macOS is an alias for **clang** (via the Command Line Tools): the
Makefile works as-is. To be explicit: `make CC=clang`.

## Building

```bash
make                     # standard build with SDL2 (Homebrew)
make SDL2=0              # headless build (without SDL2)
make tests               # full test suite
```

The Makefile detects SDL2 through `pkg-config`, falling back to **`sdl2-config`**
(shipped by `brew install sdl2`) when `PKG_CONFIG_PATH` does not point to the keg —
robust on Apple Silicon (Homebrew under `/opt/homebrew`).

## macOS-specific options

| Build | macOS note |
|-------|------------|
| `make MIDI=1` | Real-time MIDI through **CoreMIDI** (frameworks linked automatically). The CoreMIDI backend is written against the documented API but still has to be validated on a Mac. |
| `make CAST=1` | Chromecast MJPEG; requires OpenSSL (`brew install openssl@3`, export `PKG_CONFIG_PATH` to its `lib/pkgconfig`). |
| `--serial com:…` | Real serial port through **termios** (POSIX) — now enabled on macOS as on Linux. |
| `--serial pty` | Pseudo-terminal via `openpty()` (`<util.h>` on macOS). |

## Portability details

The adaptations that make the macOS build possible:

- **PTY**: `openpty()` is declared in `<util.h>` on macOS/BSD (not
  `<pty.h>` as on Linux) — conditional include in
  `src/io/serial_backend.c`.
- **Serial COM**: `HAS_COM` now covers `__APPLE__` (full POSIX termios
  on macOS), not only Linux.
- **`MSG_NOSIGNAL`**: missing on macOS/BSD; falls back to `0` in
  `include/utils/oscompat.h`. Since SIGPIPE is ignored at process level
  (`oscompat_ignore_sigpipe()`), writing to a dead socket returns `EPIPE`
  instead of killing the emulator.
- **SDL2**: `pkg-config` detection → `sdl2-config` fallback in the `Makefile`.
- **`clock_nanosleep`/`TIMER_ABSTIME`**: missing on macOS; the `--realtime`
  pacing falls back to a relative `nanosleep` (`src/main.c`, guarded by
  `__APPLE__`).
- **`_DARWIN_C_SOURCE`**: files defining `_POSIX_C_SOURCE`/
  `_XOPEN_SOURCE` (loci, gdbstub, control, several tests…) add
  `_DARWIN_C_SOURCE` under `__APPLE__` — without it, macOS hides `snprintf`,
  `MSG_DONTWAIT`, `INADDR_LOOPBACK` and other BSD extensions in `<stdio.h>`/
  `<netinet/in.h>`.
- **Optional `weak` symbol**: an *undefined weak* resolved to NULL works in ELF
  but not in Mach-O; the optional save-state of `debugger.c` uses a weak
  *definition* (portable), overridden by the real `savestate.c`.
- **bash**: macOS only ships bash 3.2; the integration test scripts
  target modern bash → the CI installs `bash` from Homebrew.

## Not covered (Linux-only backends)

The `--serial com:` backend relies on the standard POSIX baud constants;
non-standard rates (above `B230400`) are not exposed. No other
feature is disabled on macOS.
