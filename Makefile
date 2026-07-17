# Phosphoric — ORIC-1 Emulator Makefile
# Complete build system for emulator, tools, and tests

CC = gcc
# -MMD -MP : generate per-object .d files capturing header dependencies so
# touching include/*.h triggers recompilation of the .c files that use them.
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -I./include -MMD -MP
# -lpthread: control_queue (sprint 93) hands commands from producer threads
# (e.g. the future HTTP API) to the single-threaded emulator loop. Harmless on
# glibc >= 2.34 where pthread is folded into libc. WIN redefines LDFLAGS below
# (winpthread is linked there instead).
LDFLAGS = -lm -lutil -lpthread

# Windows cross-build (Sprint 89) : make WIN=1 SDL2=1 with MinGW-w64.
# Expects the SDL2 MinGW development package; point SDL2_WIN_PREFIX at its
# x86_64-w64-mingw32 directory (contains include/ and lib/).
# v1 scope: serial pty/com/tcp/modem/picowifi, --gdb, CAST and MIDI host
# transports are excluded (clear runtime messages); everything else works.
WIN ?= 0
ifeq ($(WIN), 1)
    # -posix flavour: clock_gettime/clock_nanosleep live in winpthreads
    CC = x86_64-w64-mingw32-gcc-posix
    EXE = .exe
    SDL2_WIN_PREFIX ?= /opt/sdl2-mingw/x86_64-w64-mingw32
    LDFLAGS = -lm -lws2_32 -lwinpthread -static-libgcc
    PICOTLS = 0
endif

# Debug/Release
DEBUG ?= 0
ifeq ($(DEBUG), 1)
    CFLAGS += -g -O0 -DDEBUG
else
    CFLAGS += -O2 -DNDEBUG
endif

# SDL2 support — ON by default (affichage/audio/clavier réels). Pour un build
# headless (CI/automation, sans libSDL2), passer explicitement SDL2=0.
SDL2 ?= 1
ifeq ($(SDL2), 1)
ifeq ($(WIN), 1)
    CFLAGS += -DHAS_SDL2 -I$(SDL2_WIN_PREFIX)/include -I$(SDL2_WIN_PREFIX)/include/SDL2 -Dmain=SDL_main
    LDFLAGS += -L$(SDL2_WIN_PREFIX)/lib -lmingw32 -lSDL2main -lSDL2 -mwindows
else
    CFLAGS += -DHAS_SDL2 $(shell pkg-config --cflags sdl2 2>/dev/null)
    LDFLAGS += $(shell pkg-config --libs sdl2 2>/dev/null)
endif
endif

# Cast server support (optional)
CAST ?= 0
ifeq ($(CAST), 1)
    CFLAGS += -DHAS_CAST
    LDFLAGS += -lpthread -lssl -lcrypto
endif

# HTTP control API (sprint 94, API REST Epic 3) — optional. A background thread
# turns REST calls into --control commands run on the emulator thread via the
# control_queue (drained per frame). Sockets + pthread only (pthread already in
# base LDFLAGS); no extra libraries. `--http-api[=PORT]` at runtime.
HTTPAPI ?= 0
ifeq ($(HTTPAPI), 1)
    CFLAGS += -DHAS_HTTPAPI
endif

# Real-time host MIDI (ALSA sequencer) — optional. Bridges the Oric's MIDI byte
# stream (Mageco card, --mageco midi[:target]) to the host MIDI graph so it can
# drive FluidSynth / a DAW, or a MIDI keyboard can play into the Oric. Links
# -lasound. Without MIDI=1 the `midi` transport returns a clear "rebuild" error.
MIDI ?= 0
ifeq ($(MIDI), 1)
    CFLAGS += -DHAS_MIDI
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S), Linux)
        CFLAGS  += $(shell pkg-config --cflags alsa 2>/dev/null)
        LDFLAGS += $(shell pkg-config --libs alsa 2>/dev/null)
    endif
    ifeq ($(UNAME_S), Darwin)
        LDFLAGS += -framework CoreMIDI -framework CoreFoundation
    endif
    ifneq (,$(findstring MINGW,$(UNAME_S)))
        LDFLAGS += -lwinmm
    endif
endif

# PicoWiFi TLS termination (v0.2.0 firmware) — OpenSSL terminates TLS in the
# emulated modem so the Oric reaches HTTPS/secure BBS in cleartext, mirroring
# the real Pico W mbedTLS path. Auto-enabled when OpenSSL is available; set
# PICOTLS=0 to force a TLS-less build (secure dials then return NO CARRIER).
PICOTLS ?= auto
ifeq ($(PICOTLS), auto)
    PICOTLS := $(shell pkg-config --exists openssl && echo 1 || echo 0)
endif
ifeq ($(PICOTLS), 1)
    CFLAGS += -DHAS_PICOTLS $(shell pkg-config --cflags openssl 2>/dev/null)
    LDFLAGS += $(shell pkg-config --libs openssl 2>/dev/null)
endif

# Coverage support (optional)
COVERAGE ?= 0
ifeq ($(COVERAGE), 1)
    CFLAGS += --coverage -O0 -g
    LDFLAGS += --coverage
endif

# Source files
SOURCES = src/main.c \
          src/cpu/cpu6502.c \
          src/cpu/opcodes.c \
          src/cpu/addressing.c \
          src/memory/memory.c \
          src/memory/banking.c \
          src/io/via6522.c \
          src/io/keyboard.c \
          src/io/joystick.c \
          src/io/printer.c \
          src/io/mcp40.c \
          src/io/cassette.c \
          src/io/microdisc.c \
          src/io/loci_core.c \
          src/io/loci_fs.c \
          src/io/loci_bus.c \
          src/io/loci_boot.c \
          src/io/loci_sdimg.c \
          src/io/acia6551.c \
          src/io/serial_backend.c \
          src/io/smf.c \
          src/io/pia6821.c \
          src/io/acia6850.c \
          src/io/dtl2000.c \
          src/io/mageco.c \
          src/io/serial_picowifi.c \
          src/io/ula_ng.c \
          src/io/io_bus.c \
          src/video/video.c \
          src/video/textmode.c \
          src/video/hires.c \
          src/video/export.c \
          src/video/avi_recorder.c \
          src/video/stb_image_write_impl.c \
          src/video/renderer.c \
          src/video/osd.c \
          src/audio/ay3891x.c \
          src/audio/audio_output.c \
          src/storage/tap.c \
          src/storage/sedoric.c \
          src/storage/disk.c \
          src/hostfs/hostfs.c \
          src/hostfs/vfs.c \
          src/savestate.c \
          src/debugger.c \
          src/network/gdbstub.c \
          src/control.c \
          src/control_queue.c \
          src/utils/logging.c \
          src/utils/config.c \
          src/utils/trace.c \
          src/utils/profiler.c \
          src/utils/rominfo.c \
          src/utils/symbols.c \
          src/utils/movie.c \
          src/utils/netutil.c \
          src/utils/appsignal.c

ifeq ($(CAST), 1)
    SOURCES += src/network/cast_server.c src/network/castv2.c
endif

ifeq ($(HTTPAPI), 1)
    SOURCES += src/network/http_api.c
endif


# Windows v1 : swap the POSIX-only modules for their Windows variants
ifeq ($(WIN), 1)
    SOURCES := $(filter-out src/io/serial_backend.c src/io/serial_picowifi.c \
                            src/network/gdbstub.c, $(SOURCES))
    SOURCES += src/io/serial_backend_win.c src/network/gdbstub_win.c
endif

ifeq ($(TUI), 1)
    SOURCES += src/tui.c
    CFLAGS  += -DHAS_TUI
    LDFLAGS += -lncursesw
endif

OBJECTS = $(SOURCES:.c=.o)

# Core libraries (no main)
LIB_SOURCES = $(filter-out src/main.c, $(SOURCES))
LIB_OBJECTS = $(LIB_SOURCES:.c=.o)

# Tools
TOOL_OBJECTS = src/storage/tap.o src/utils/logging.o

# Targets
TARGET = oric1-emu$(EXE)
TOOLS = bas2tap bin2tap tap2sedoric sedoric-info

# Install paths
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
DATADIR = $(PREFIX)/share/phosphoric
DOCDIR = $(PREFIX)/share/doc/phosphoric

.PHONY: all release clean tools tests test-cpu test-memory test-io test-ula-ng test-storage test-system test-rom test-video test-avi test-audio test-debugger test-gdbstub test-movie test-movie-replay test-cast test-savestate test-atmos test-joystick test-printer test-mcp40 test-renderer test-osd test-trace test-profiler test-rominfo test-serial test-pia6821 test-acia6850 test-dtl2000 test-dtl2000-txrx test-midi test-smf test-serial-file test-picowifi test-keyboard test-symbols test-loci test-loci-sdimg test-loci-sdimg-write test-loci-e2e test-loci-acia-e2e test-control test-game-compat test-mc-autorun test-control-dispatch test-control-queue test-httpapi test-loadstate test-sedoric-tools test-ula-ng-visible bench valgrind static-analysis cppcheck flawfinder security-check coverage coverage-report install uninstall help wasm

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $(TARGET)

# Copie strippée pour la distribution (symboles retirés → binaire plus petit).
# Produit $(TARGET)-release SANS toucher au binaire de travail $(TARGET)
# (utilisé par d'autres programmes). Epic 7 / US1, Sprint 125.
release: $(TARGET)
	cp $(TARGET) $(TARGET)-release
	strip $(TARGET)-release
	@echo "Binaire de distribution : $(TARGET)-release ($$(stat -c%s $(TARGET)-release) o, vs $$(stat -c%s $(TARGET)) o non strippé)"

tools: $(TOOLS)

bas2tap: tools/bas2tap.c $(TOOL_OBJECTS)
	$(CC) $(CFLAGS) tools/bas2tap.c $(TOOL_OBJECTS) $(LDFLAGS) -o bas2tap

bin2tap: tools/bin2tap.c $(TOOL_OBJECTS)
	$(CC) $(CFLAGS) tools/bin2tap.c $(TOOL_OBJECTS) $(LDFLAGS) -o bin2tap

tap2sedoric: tools/tap2sedoric.c $(TOOL_OBJECTS) src/storage/sedoric.o
	$(CC) $(CFLAGS) tools/tap2sedoric.c $(TOOL_OBJECTS) src/storage/sedoric.o $(LDFLAGS) -o tap2sedoric

sedoric-info: tools/sedoric_info.c
	$(CC) $(CFLAGS) tools/sedoric_info.c $(LDFLAGS) -o sedoric-info

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Include auto-generated header-dependency files (-MMD output).
# Silent if absent (first build / after clean).
-include $(OBJECTS:.o=.d)
-include $(TOOL_OBJECTS:.o=.d)

# ═══════════════════════════════════════════════════════════════
#  TESTS
# ═══════════════════════════════════════════════════════════════

TEST_CPU_SRCS = tests/unit/test_cpu.c src/cpu/cpu6502.c src/cpu/opcodes.c \
                src/cpu/addressing.c src/memory/memory.c src/memory/banking.c \
                src/utils/logging.c

TEST_MEM_SRCS = tests/unit/test_memory.c src/memory/memory.c \
                src/memory/banking.c src/utils/logging.c

TEST_IO_SRCS = tests/unit/test_io.c src/io/via6522.c src/utils/logging.c

TEST_ULA_NG_SRCS = tests/unit/test_ula_ng.c src/io/ula_ng.c

TEST_CASSETTE_SRCS = tests/unit/test_cassette.c src/io/cassette.c src/io/via6522.c

TEST_STORAGE_SRCS = tests/unit/test_storage.c src/storage/sedoric.c \
                    src/storage/disk.c src/io/microdisc.c src/utils/logging.c

TEST_SYSTEM_SRCS = tests/unit/test_full_system.c src/cpu/cpu6502.c \
                   src/cpu/opcodes.c src/cpu/addressing.c src/memory/memory.c \
                   src/memory/banking.c src/io/via6522.c src/utils/logging.c

test-cpu: $(TEST_CPU_SRCS)
	@$(CC) $(CFLAGS) $(TEST_CPU_SRCS) $(LDFLAGS) -o test_cpu
	@./test_cpu

test-memory: $(TEST_MEM_SRCS)
	@$(CC) $(CFLAGS) $(TEST_MEM_SRCS) $(LDFLAGS) -o test_memory
	@./test_memory

test-ula-ng: $(TEST_ULA_NG_SRCS)
	@$(CC) $(CFLAGS) $(TEST_ULA_NG_SRCS) $(LDFLAGS) -o test_ula_ng
	@./test_ula_ng

test-io: $(TEST_IO_SRCS)
	@$(CC) $(CFLAGS) $(TEST_IO_SRCS) $(LDFLAGS) -o test_io
	@./test_io

test-cassette: $(TEST_CASSETTE_SRCS)
	@$(CC) $(CFLAGS) $(TEST_CASSETTE_SRCS) $(LDFLAGS) -o test_cassette
	@./test_cassette

test-storage: $(TEST_STORAGE_SRCS)
	@$(CC) $(CFLAGS) $(TEST_STORAGE_SRCS) $(LDFLAGS) -o test_storage
	@./test_storage

test-system: $(TEST_SYSTEM_SRCS)
	@$(CC) $(CFLAGS) $(TEST_SYSTEM_SRCS) $(LDFLAGS) -o test_system
	@./test_system

TEST_ROM_SRCS = tests/unit/test_rom.c src/cpu/cpu6502.c src/cpu/opcodes.c \
                src/cpu/addressing.c src/memory/memory.c src/memory/banking.c \
                src/io/via6522.c src/utils/logging.c

test-rom: $(TEST_ROM_SRCS)
	@$(CC) $(CFLAGS) $(TEST_ROM_SRCS) $(LDFLAGS) -o test_rom
	@./test_rom

TEST_VIDEO_SRCS = tests/unit/test_video.c src/video/video.c src/video/export.c \
                  src/cpu/cpu6502.c src/cpu/opcodes.c src/cpu/addressing.c \
                  src/memory/memory.c src/memory/banking.c src/io/via6522.c \
                  src/io/ula_ng.c src/utils/logging.c

test-video: $(TEST_VIDEO_SRCS)
	@$(CC) $(CFLAGS) $(TEST_VIDEO_SRCS) $(LDFLAGS) -o test_video
	@./test_video

TEST_AVI_SRCS = tests/unit/test_avi.c src/video/avi_recorder.c \
                src/video/stb_image_write_impl.c

test-avi: $(TEST_AVI_SRCS)
	@$(CC) $(CFLAGS) $(TEST_AVI_SRCS) $(LDFLAGS) -o test_avi
	@./test_avi

TEST_MOVIE_SRCS = tests/unit/test_movie.c src/utils/movie.c src/utils/logging.c

test-movie: $(TEST_MOVIE_SRCS)
	@$(CC) $(CFLAGS) $(TEST_MOVIE_SRCS) $(LDFLAGS) -o test_movie
	@./test_movie

test-movie-replay: $(TARGET)
	@bash tests/integration/test_movie_replay.sh

TEST_GDB_SRCS = tests/unit/test_gdbstub.c src/network/gdbstub.c src/debugger.c \
                src/cpu/cpu6502.c src/cpu/opcodes.c src/cpu/addressing.c \
                src/memory/memory.c src/memory/banking.c \
                src/io/via6522.c src/utils/logging.c src/utils/symbols.c \
                src/utils/trace.c

test-gdbstub: $(TEST_GDB_SRCS)
	@$(CC) $(CFLAGS) $(TEST_GDB_SRCS) $(LDFLAGS) -o test_gdbstub
	@./test_gdbstub

TEST_AUDIO_SRCS = tests/unit/test_audio.c src/audio/ay3891x.c src/utils/logging.c

test-audio: $(TEST_AUDIO_SRCS)
	@$(CC) $(CFLAGS) $(TEST_AUDIO_SRCS) $(LDFLAGS) -o test_audio
	@./test_audio

TEST_DEBUGGER_SRCS = tests/unit/test_debugger.c src/debugger.c \
                     src/cpu/cpu6502.c src/cpu/opcodes.c src/cpu/addressing.c \
                     src/memory/memory.c src/memory/banking.c \
                     src/io/via6522.c src/utils/logging.c src/utils/symbols.c \
                     src/utils/trace.c

test-debugger: $(TEST_DEBUGGER_SRCS)
	@$(CC) $(CFLAGS) $(TEST_DEBUGGER_SRCS) $(LDFLAGS) -o test_debugger
	@./test_debugger

TEST_CAST_SRCS = tests/unit/test_cast.c src/network/cast_server.c src/network/castv2.c src/utils/logging.c

test-cast: $(TEST_CAST_SRCS)
	@$(CC) $(CFLAGS) -DHAS_CAST $(TEST_CAST_SRCS) $(LDFLAGS) -lpthread -lssl -lcrypto -o test_cast
	@./test_cast

TEST_SAVESTATE_SRCS = tests/unit/test_savestate.c src/savestate.c \
                      src/cpu/cpu6502.c src/cpu/opcodes.c src/cpu/addressing.c \
                      src/memory/memory.c src/memory/banking.c \
                      src/io/via6522.c src/io/keyboard.c src/io/microdisc.c \
                      src/audio/ay3891x.c src/video/video.c src/io/ula_ng.c \
                      src/storage/disk.c src/storage/sedoric.c \
                      src/utils/logging.c

test-savestate: $(TEST_SAVESTATE_SRCS)
	@$(CC) $(CFLAGS) $(TEST_SAVESTATE_SRCS) $(LDFLAGS) -o test_savestate
	@./test_savestate

TEST_ATMOS_SRCS = tests/unit/test_atmos.c src/memory/memory.c \
                  src/memory/banking.c src/utils/logging.c

test-atmos: $(TEST_ATMOS_SRCS)
	@$(CC) $(CFLAGS) $(TEST_ATMOS_SRCS) $(LDFLAGS) -o test_atmos
	@./test_atmos

TEST_MCP40_SRCS = tests/unit/test_mcp40.c src/io/mcp40.c src/utils/logging.c

test-mcp40: $(TEST_MCP40_SRCS)
	@$(CC) $(CFLAGS) $(TEST_MCP40_SRCS) $(LDFLAGS) -o test_mcp40
	@./test_mcp40

TEST_PRINTER_SRCS = tests/unit/test_printer.c src/io/printer.c src/io/mcp40.c src/utils/logging.c

test-printer: $(TEST_PRINTER_SRCS)
	@$(CC) $(CFLAGS) $(TEST_PRINTER_SRCS) $(LDFLAGS) -o test_printer
	@./test_printer

TEST_JOYSTICK_SRCS = tests/unit/test_joystick.c src/io/joystick.c src/io/via6522.c src/utils/logging.c

test-joystick: $(TEST_JOYSTICK_SRCS)
	@$(CC) $(CFLAGS) $(TEST_JOYSTICK_SRCS) $(LDFLAGS) -o test_joystick
	@./test_joystick

TEST_RENDERER_SRCS = tests/unit/test_renderer.c src/video/video.c src/video/renderer.c \
                     src/io/ula_ng.c \
                     src/memory/memory.c src/memory/banking.c src/utils/logging.c

test-renderer: $(TEST_RENDERER_SRCS)
	@$(CC) $(CFLAGS) $(TEST_RENDERER_SRCS) $(LDFLAGS) -o test_renderer
	@./test_renderer

TEST_OSD_SRCS = tests/unit/test_osd.c src/video/osd.c

test-osd: $(TEST_OSD_SRCS)
	@$(CC) $(CFLAGS) $(TEST_OSD_SRCS) $(LDFLAGS) -o test_osd
	@./test_osd


TEST_TRACE_SRCS = tests/unit/test_trace.c src/utils/trace.c \
                  src/cpu/cpu6502.c src/cpu/opcodes.c src/cpu/addressing.c \
                  src/memory/memory.c src/memory/banking.c src/utils/logging.c \
                  src/utils/symbols.c

test-trace: $(TEST_TRACE_SRCS)
	@$(CC) $(CFLAGS) $(TEST_TRACE_SRCS) $(LDFLAGS) -o test_trace
	@./test_trace

TEST_PROFILER_SRCS = tests/unit/test_profiler.c src/utils/profiler.c \
                     src/cpu/cpu6502.c src/cpu/opcodes.c src/cpu/addressing.c \
                     src/memory/memory.c src/memory/banking.c src/utils/logging.c

test-profiler: $(TEST_PROFILER_SRCS)
	@$(CC) $(CFLAGS) $(TEST_PROFILER_SRCS) $(LDFLAGS) -o test_profiler
	@./test_profiler

TEST_ROMINFO_SRCS = tests/unit/test_rominfo.c src/utils/rominfo.c \
                    src/cpu/cpu6502.c src/cpu/opcodes.c src/cpu/addressing.c \
                    src/memory/memory.c src/memory/banking.c src/utils/logging.c

test-rominfo: $(TEST_ROMINFO_SRCS)
	@$(CC) $(CFLAGS) $(TEST_ROMINFO_SRCS) $(LDFLAGS) -o test_rominfo
	@./test_rominfo

TEST_SYMBOLS_SRCS = tests/unit/test_symbols.c src/utils/symbols.c src/utils/logging.c

test-symbols: $(TEST_SYMBOLS_SRCS)
	@$(CC) $(CFLAGS) $(TEST_SYMBOLS_SRCS) $(LDFLAGS) -o test_symbols
	@./test_symbols

TEST_LOCI_SRCS = tests/unit/test_loci.c \
                 src/io/loci_core.c src/io/loci_fs.c \
                 src/io/loci_bus.c src/io/loci_boot.c src/io/loci_sdimg.c \
                 src/utils/logging.c src/storage/disk.c src/storage/sedoric.c \
                 src/cpu/cpu6502.c src/cpu/opcodes.c src/cpu/addressing.c \
                 src/memory/memory.c src/memory/banking.c

test-loci: $(TEST_LOCI_SRCS)
	@$(CC) $(CFLAGS) $(TEST_LOCI_SRCS) $(LDFLAGS) -o test_loci
	@./test_loci

TEST_LOCI_SDIMG_SRCS = tests/unit/test_loci_sdimg.c src/io/loci_sdimg.c \
                       src/utils/logging.c

test-loci-sdimg: $(TEST_LOCI_SDIMG_SRCS)
	@$(CC) $(CFLAGS) $(TEST_LOCI_SDIMG_SRCS) $(LDFLAGS) -o test_loci_sdimg
	@./test_loci_sdimg

TEST_LOCI_SDIMG_WRITE_SRCS = tests/unit/test_loci_sdimg_write.c \
                             src/io/loci_sdimg.c src/utils/logging.c

test-loci-sdimg-write: $(TEST_LOCI_SDIMG_WRITE_SRCS)
	@$(CC) $(CFLAGS) $(TEST_LOCI_SDIMG_WRITE_SRCS) $(LDFLAGS) -o test_loci_sdimg_write
	@./test_loci_sdimg_write

TEST_SERIAL_SRCS = tests/unit/test_serial.c src/io/acia6551.c \
                   src/io/serial_backend.c src/io/smf.c src/utils/logging.c

test-serial: $(TEST_SERIAL_SRCS)
	@$(CC) $(CFLAGS) $(TEST_SERIAL_SRCS) $(LDFLAGS) -lutil -o test_serial
	@./test_serial

TEST_PIA6821_SRCS = tests/unit/test_pia6821.c src/io/pia6821.c

test-pia6821: $(TEST_PIA6821_SRCS)
	@$(CC) $(CFLAGS) $(TEST_PIA6821_SRCS) $(LDFLAGS) -o test_pia6821
	@./test_pia6821

TEST_ACIA6850_SRCS = tests/unit/test_acia6850.c src/io/acia6850.c

test-acia6850: $(TEST_ACIA6850_SRCS)
	@$(CC) $(CFLAGS) $(TEST_ACIA6850_SRCS) $(LDFLAGS) -o test_acia6850
	@./test_acia6850

TEST_DTL2000_SRCS = tests/unit/test_dtl2000.c src/io/dtl2000.c src/io/pia6821.c \
                    src/io/acia6850.c \
                    src/io/serial_backend.c src/io/smf.c src/io/acia6551.c src/utils/logging.c

test-dtl2000: $(TEST_DTL2000_SRCS)
	@$(CC) $(CFLAGS) $(TEST_DTL2000_SRCS) $(LDFLAGS) -lutil -o test_dtl2000
	@./test_dtl2000

# Mageco MIDI interface — MC6850 ACIA at $03FE, 31250 baud (forum t=2525)
TEST_MIDI_SRCS = tests/unit/test_midi.c src/io/mageco.c src/io/acia6850.c \
                 src/io/serial_backend.c src/io/smf.c src/io/acia6551.c src/utils/logging.c

test-midi: $(TEST_MIDI_SRCS)
	@$(CC) $(CFLAGS) $(TEST_MIDI_SRCS) $(LDFLAGS) -lutil -o test_midi
	@./test_midi

# Standard MIDI File (.mid) parser — timed MIDI IN replay source
TEST_SMF_SRCS = tests/unit/test_smf.c src/io/smf.c

test-smf: $(TEST_SMF_SRCS)
	@$(CC) $(CFLAGS) $(TEST_SMF_SRCS) $(LDFLAGS) -o test_smf
	@./test_smf

# DTL 2000 TX/RX loopback e2e: boots the BASIC driver on the faithful PIA/ACIA
# card and asserts the --serial-trace shows every TX byte echoed back on RX.
# Requires the emulator + bas2tap built; skips gracefully otherwise.
test-dtl2000-txrx: $(TARGET)
	@bash tests/integration/test_dtl2000_txrx.sh

# Serial file: backend e2e — deterministic replay (RX) / capture (TX). Asserts
# capture bytes, a replay->echo->capture round-trip, and --serial plumbing.
test-serial-file: $(TARGET)
	@bash tests/integration/test_serial_file_backend.sh

TEST_PICOWIFI_SRCS = tests/unit/test_picowifi.c src/io/serial_picowifi.c \
                     src/utils/logging.c

test-picowifi: $(TEST_PICOWIFI_SRCS)
	@$(CC) $(CFLAGS) $(TEST_PICOWIFI_SRCS) $(LDFLAGS) -o test_picowifi
	@./test_picowifi

TEST_KEYBOARD_SRCS = tests/unit/test_keyboard.c src/io/keyboard.c src/utils/logging.c

test-keyboard: $(TEST_KEYBOARD_SRCS)
	@$(CC) $(CFLAGS) -DHAS_SDL2 $(shell pkg-config --cflags sdl2 2>/dev/null) $(TEST_KEYBOARD_SRCS) $(LDFLAGS) $(shell pkg-config --libs sdl2 2>/dev/null) -o test_keyboard
	@./test_keyboard

TEST_COVERAGE_SRCS = tests/unit/test_coverage.c src/cpu/cpu6502.c src/cpu/opcodes.c \
                     src/cpu/addressing.c src/memory/memory.c src/memory/banking.c \
                     src/io/via6522.c src/io/keyboard.c src/io/joystick.c \
                     src/io/printer.c src/io/mcp40.c src/io/microdisc.c \
                     src/storage/sedoric.c src/storage/disk.c \
                     src/savestate.c src/debugger.c \
                     src/audio/ay3891x.c src/video/video.c src/io/ula_ng.c \
                     src/utils/logging.c src/utils/symbols.c src/utils/trace.c

test-coverage: $(TEST_COVERAGE_SRCS)
	@$(CC) $(CFLAGS) $(TEST_COVERAGE_SRCS) $(LDFLAGS) -o test_coverage
	@./test_coverage

# E2E regression of the LOCI/Sedoric pipe (sprints 34b0/b1/b2).
# Skipped gracefully when required ROM/disk assets are absent.
test-loci-e2e:
	@bash tests/integration/test_loci_sedoric_e2e.sh

# E2E: BASIC drives the LOCI ACIA 6551 at $0380 (TX + RX round-trip).
test-loci-acia-e2e: $(TARGET)
	@bash tests/integration/test_loci_acia_e2e.sh

# --control IPC media hot-swap commands (load-disk / eject-disk / eject-tape).
test-control: $(TARGET)
	@bash tests/integration/test_control_media_swap.sh

# Sprint 92 (Epic 1) — transport-agnostic control_dispatch via a buffer sink.
# Links the core library objects (no main) and drives control_dispatch()
# directly, asserting byte-exact replies + CONTINUE/RESUME/QUIT results.
test-control-dispatch: $(LIB_OBJECTS)
	@$(CC) $(CFLAGS) tests/unit/test_control_dispatch.c $(LIB_OBJECTS) $(LDFLAGS) -o test_control_dispatch
	@./test_control_dispatch

# Sprint 93 (Epic 2) — thread-safe command queue. Spawns producer threads that
# submit() concurrently while a consumer thread drain()s per "frame", asserting
# correct per-producer routing (unique addr write/read) and zero corruption.
test-control-queue: $(LIB_OBJECTS)
	@$(CC) $(CFLAGS) tests/unit/test_control_queue.c $(LIB_OBJECTS) $(LDFLAGS) -o test_control_queue
	@./test_control_queue

# Sprint 94 (Epic 3) — HTTP control API end-to-end (curl vs a live headless
# emulator). Skips gracefully unless the emulator was built with HTTPAPI=1.
test-httpapi: $(TARGET)
	@bash tests/integration/test_http_api_e2e.sh

# Sprint 36c -- machine-code autorun / rechain-gate regression.
# Requires the emulator + tools to be built (uses bin2tap/bas2tap).
test-mc-autorun:
	@bash tests/integration/test_mc_autorun_rechain.sh

# Sprint 57 — base ROM presence guard. Checks that --disk-rom without -r
# fails fast (impossible config) and that no-ROM warns. Fast + hermetic.
test-rom-guard: $(TARGET)
	@bash tests/integration/test_rom_guard.sh

test-loadstate: $(TARGET)
	@bash tests/integration/test_load_state_control.sh

test-sedoric-tools: tap2sedoric sedoric-info
	@bash tests/integration/test_sedoric_inject.sh

test-ula-ng-visible: $(TARGET)
	@bash tests/integration/test_ula_ng_visible.sh

# Sprint 36a — throughput benchmark. Runs 4 scenarios headless and
# reports MHz-equivalent / speed ratio vs real ORIC (1 MHz).
# Usage: `make bench`               human-readable table
#        `make bench BENCH_TSV=1`   tab-separated for tracking
#        `make bench CYCLES=200000000`  longer run
bench:
	@bash tools/bench.sh $(if $(BENCH_TSV),--tsv,)

# Sprint 36b — game compatibility regression. Boots 7 commercial
# Oric titles from OricProgramsLib and checks each reaches a known
# intro screen. Skips gracefully when the lib is absent.
# Override path via ORIC_PROGRAMS_LIB=/path or BASIC_ROM=...
test-game-compat:
	@bash tests/integration/test_game_compat.sh

tests: test-cpu test-memory test-io test-ula-ng test-cassette test-storage test-system test-video test-avi test-audio test-debugger test-gdbstub test-movie test-movie-replay test-savestate test-atmos test-joystick test-printer test-mcp40 test-renderer test-osd test-trace test-profiler test-rominfo test-serial test-pia6821 test-acia6850 test-dtl2000 test-dtl2000-txrx test-midi test-smf test-serial-file test-picowifi test-keyboard test-symbols test-loci test-loci-sdimg test-loci-sdimg-write test-loci-acia-e2e test-control test-control-dispatch test-control-queue test-httpapi test-coverage test-rom-guard test-loadstate test-sedoric-tools test-ula-ng-visible
	@echo ""
	@echo "═══════════════════════════════════════════════════════"
	@echo "  All test suites completed!"
	@echo "═══════════════════════════════════════════════════════"

# ═══════════════════════════════════════════════════════════════
#  QUALITY TARGETS
# ═══════════════════════════════════════════════════════════════

STATIC_CFLAGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
                -Wdouble-promotion -Wformat=2 -Wundef -Wstrict-prototypes \
                -Wmissing-prototypes -Wold-style-definition -std=c11 -I./include

static-analysis:
	@echo "Running static analysis with extra warnings..."
	@$(CC) $(STATIC_CFLAGS) -fsyntax-only $(LIB_SOURCES) 2>&1 || true
	@echo ""
	@echo "Static analysis complete."

cppcheck:
	@command -v cppcheck >/dev/null 2>&1 || { echo "cppcheck non trouvé — apt install cppcheck"; exit 1; }
	@echo "═══════════════════════════════════════════════════════"
	@echo "  cppcheck — analyse statique"
	@echo "═══════════════════════════════════════════════════════"
	@cppcheck --enable=warning,performance,portability \
	          --std=c11 \
	          --suppress=missingIncludeSystem \
	          --suppress=unusedFunction \
	          --suppress=normalCheckLevelMaxBranches \
	          --suppress=*:third_party/* \
	          -I ./include \
	          --error-exitcode=1 \
	          src/ 2>&1
	@echo "  cppcheck : OK"
	@echo "═══════════════════════════════════════════════════════"

flawfinder:
	@command -v flawfinder >/dev/null 2>&1 || { echo "flawfinder non trouvé — pip install flawfinder"; exit 1; }
	@echo "═══════════════════════════════════════════════════════"
	@echo "  flawfinder — analyse sécurité"
	@echo "═══════════════════════════════════════════════════════"
	@flawfinder --minlevel=2 --error-level=5 src/ include/
	@echo "  flawfinder : OK (aucun risque niveau 5 critique)"
	@echo "═══════════════════════════════════════════════════════"

security-check: cppcheck flawfinder
	@echo ""
	@echo "═══════════════════════════════════════════════════════"
	@echo "  Security check complet : cppcheck + flawfinder OK"
	@echo "═══════════════════════════════════════════════════════"

valgrind: test-cpu test-memory test-io test-storage test-system test-rom test-video test-audio test-debugger test-cast
	@echo "Running tests under Valgrind..."
	@valgrind --leak-check=full --error-exitcode=1 --quiet ./test_cpu
	@valgrind --leak-check=full --error-exitcode=1 --quiet ./test_memory
	@valgrind --leak-check=full --error-exitcode=1 --quiet ./test_io
	@valgrind --leak-check=full --error-exitcode=1 --quiet ./test_storage
	@valgrind --leak-check=full --error-exitcode=1 --quiet ./test_system
	@valgrind --leak-check=full --error-exitcode=1 --quiet ./test_rom
	@valgrind --leak-check=full --error-exitcode=1 --quiet ./test_video
	@valgrind --leak-check=full --error-exitcode=1 --quiet ./test_audio
	@valgrind --leak-check=full --error-exitcode=1 --quiet ./test_debugger
	@valgrind --leak-check=full --error-exitcode=1 --quiet ./test_cast
	@echo ""
	@echo "═══════════════════════════════════════════════════════"
	@echo "  Valgrind: No memory leaks detected!"
	@echo "═══════════════════════════════════════════════════════"

# ═══════════════════════════════════════════════════════════════
#  CODE COVERAGE
# ═══════════════════════════════════════════════════════════════

coverage:
	@echo "Building and running tests with coverage instrumentation..."
	@$(MAKE) clean --no-print-directory
	@$(MAKE) tests COVERAGE=1 --no-print-directory
	@echo ""
	@echo "Generating coverage report..."
	@$(MAKE) coverage-report --no-print-directory

coverage-report:
	@echo "═══════════════════════════════════════════════════════"
	@echo "  Code Coverage Report — Phosphoric"
	@echo "═══════════════════════════════════════════════════════"
	@echo ""
	@total_lines=0; covered_lines=0; \
	echo "File                                      Lines   Covered   Coverage"; \
	echo "────────────────────────────────────────────────────────────────────"; \
	for gcno in $$(find src/ -name '*.gcno' 2>/dev/null); do \
		src=$$(echo $$gcno | sed 's/\.gcno$$/.c/'); \
		if [ -f "$$src" ]; then \
			gcov -n "$$src" 2>/dev/null | grep -A1 "^File '$$src'" | tail -1 | \
			while read line; do \
				pct=$$(echo "$$line" | grep -oP '[0-9]+\.[0-9]+%' | head -1); \
				lines=$$(echo "$$line" | grep -oP 'of [0-9]+' | grep -oP '[0-9]+' | head -1); \
				if [ -n "$$pct" ] && [ -n "$$lines" ]; then \
					cov=$$(echo "$$pct" | sed 's/%//'); \
					covered=$$(echo "$$lines $$cov" | awk '{printf "%d", $$1 * $$2 / 100}'); \
					printf "%-42s %5s   %5s     %s\n" "$$src" "$$lines" "$$covered" "$$pct"; \
				fi; \
			done; \
		fi; \
	done
	@echo ""
	@echo "Generating aggregate summary..."
	@gcov -n src/**/*.c src/*.c 2>/dev/null | grep -E "^Lines executed:" | \
		awk -F'[:%]' 'BEGIN{tl=0;te=0;n=0} {split($$3,a," of "); te+=$$2*a[2]/100; tl+=a[2]; n++} \
		END{if(tl>0) printf "TOTAL: %.1f%% (%d/%d lines in %d files)\n", te/tl*100, te, tl, n; \
		else print "No coverage data found"}'
	@echo ""
	@echo "═══════════════════════════════════════════════════════"

coverage-clean:
	@find . -name '*.gcno' -o -name '*.gcda' -o -name '*.gcov' | xargs rm -f 2>/dev/null
	@echo "Coverage data cleaned."

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(DATADIR)/roms
	-install -m 644 roms/*.rom $(DESTDIR)$(DATADIR)/roms/ 2>/dev/null || true
	install -d $(DESTDIR)$(DOCDIR)
	install -m 644 README.md $(DESTDIR)$(DOCDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -rf $(DESTDIR)$(DATADIR)
	rm -rf $(DESTDIR)$(DOCDIR)

clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) $(TARGET) $(TOOLS)
	rm -f test_cpu test_memory test_io test_storage test_system test_rom test_video test_avi test_audio test_debugger test_gdbstub test_movie test_cast test_savestate test_atmos test_joystick test_printer test_mcp40 test_renderer test_trace test_profiler test_rominfo test_serial test_picowifi test_keyboard test_coverage
	rm -f tools/*.o tools/*.d
	rm -f web/phosphoric.html web/phosphoric.js web/phosphoric.wasm web/phosphoric.data
	find . -name '*.gcno' -o -name '*.gcda' -o -name '*.gcov' -o -name '*.d' | xargs rm -f 2>/dev/null

# ═══════════════════════════════════════════════════════════════
#  WEBASSEMBLY (Emscripten)
# ═══════════════════════════════════════════════════════════════
# Builds a browser bundle (HTML+JS+WASM+preloaded ROMs). Needs emsdk active
# (`emcc` in PATH). Networking features (serial sockets/PTY, GDB stub, cast,
# TLS) link as no-ops in the browser; the core machine, video, audio, keyboard,
# tape and disk all run. The C main loop yields per frame via Asyncify.
#   make wasm && (cd web && python3 -m http.server) → open localhost:8000/phosphoric.html
EMCC ?= emcc
WASM_OUT = web/phosphoric.html
WASM_CFLAGS  = -O2 -std=c11 -DHAS_SDL2 -sUSE_SDL=2 -I./include
WASM_LDFLAGS = -sUSE_SDL=2 -sASYNCIFY -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8MB \
               -sEXPORTED_RUNTIME_METHODS=ccall,FS,callMain \
               -sEXPORTED_FUNCTIONS=_main,_web_key,_web_key_release_all,_web_io_activity,_web_save_state,_web_load_state,_web_insert_tap,_web_insert_disk,_malloc,_free \
               --preload-file roms@/roms --shell-file web/shell.html

wasm: web/shell.html
	$(EMCC) $(WASM_CFLAGS) $(LIB_SOURCES) src/main.c $(WASM_LDFLAGS) -o $(WASM_OUT)
	@echo "WASM ready → serve web/ over HTTP and open phosphoric.html"
	@echo "  e.g.  (cd web && python3 -m http.server 8000)  then  http://localhost:8000/phosphoric.html"

help:
	@echo "Phosphoric — ORIC-1 Emulator Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build emulator (default)"
	@echo "  tools        - Build conversion tools"
	@echo "  tests        - Build and run all tests"
	@echo "  test-cpu     - Run CPU tests only"
	@echo "  test-memory  - Run memory tests only"
	@echo "  test-io      - Run VIA/I/O tests only"
	@echo "  test-storage - Run storage tests only"
	@echo "  test-system  - Run integration tests only"
	@echo "  test-rom     - Run ROM compatibility tests"
	@echo "  test-video   - Run video export tests"
	@echo "  test-avi     - Run MJPEG AVI recorder tests"
	@echo "  test-audio   - Run PSG audio tests"
	@echo "  test-debugger- Run debugger tests"
	@echo "  test-savestate - Run save state tests"
	@echo "  test-atmos   - Run Atmos support tests"
	@echo "  test-joystick- Run joystick tests"
	@echo "  test-printer - Run printer tests"
	@echo "  test-mcp40  - Run MCP-40 plotter tests"
	@echo "  test-renderer- Run display scaling tests"
	@echo "  test-trace   - Run CPU trace logging tests"
	@echo "  test-profiler- Run CPU profiler tests"
	@echo "  test-rominfo - Run ROM analysis tests"
	@echo "  test-cast    - Run cast server tests (requires CAST=1)"
	@echo "  valgrind     - Run all tests under Valgrind"
	@echo "  static-analysis - Run static analysis (extra compiler warnings)"
	@echo "  cppcheck     - Run cppcheck static analysis (apt install cppcheck)"
	@echo "  flawfinder   - Run flawfinder security scan (pip install flawfinder)"
	@echo "  security-check - Run cppcheck + flawfinder"
	@echo "  install      - Install emulator (PREFIX=/usr/local)"
	@echo "  uninstall    - Remove installed files"
	@echo "  coverage     - Build with coverage, run tests, generate report"
	@echo "  clean        - Remove build artifacts"
	@echo "  help         - Show this help"
	@echo ""
	@echo "Options:"
	@echo "  DEBUG=1      - Build with debug symbols"
	@echo "  SDL2=1       - Build with SDL2 display/audio"
	@echo "  CAST=1       - Build with MJPEG cast server"
	@echo "  COVERAGE=1   - Build with gcov coverage instrumentation"
