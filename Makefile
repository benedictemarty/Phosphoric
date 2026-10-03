# Phosphoric — ORIC-1 Emulator Makefile
# Complete build system for emulator, tools, and tests

CC = gcc
# -MMD -MP : generate per-object .d files capturing header dependencies so
# touching include/*.h triggers recompilation of the .c files that use them.
# Émulateur RP2040 embarqué (backend --loci-emu). Chemin surchargeable.
# Dépendance EXTERNE non versionnée : détectée automatiquement. Sans elle
# (CI, machine neuve), src/io/loci_emu_stub.c prend la place de loci_emu.c :
# tout se construit, seul `--loci-emu` refuse de démarrer. Forcer : LOCI_EMU=0/1.
LOCI_EMUL_DIR ?= $(HOME)/loci/emul
LOCI_EMU ?= $(if $(wildcard $(LOCI_EMUL_DIR)/src/emul_lib.h),1,0)
# Backend MATÉRIEL RÉEL (--loci-hw DEV) : `make LOCI_HW=1` remplace loci_emu.c par
# src/io/loci_hw.c (copie de ~/loci/loci-usb/phosphoric/loci_hw.c) + le client du
# protocole loci-usb. Un binaire = un backend (mêmes symboles loci_emu_*).
LOCI_USB_DIR ?= $(HOME)/loci/loci-usb
LOCI_HW ?= 0
# Backend du NOUVEAU firmware loci-fw (reprise de zéro, ~/loci/reprise) : `make LOCI_NEO=1`
# remplace loci_emu.c par src/io/loci_neo.c (pont emul_neo de libemul).
LOCI_NEO ?= 0
ifeq ($(LOCI_NEO),1)
LOCI_EMU_SRC = src/io/loci_neo.c
LOCI_EMUL_LIB = $(LOCI_EMUL_DIR)/libemul.a
LOCI_EMU_CFLAGS = -I$(LOCI_EMUL_DIR)/src
else ifeq ($(LOCI_HW),1)
LOCI_EMU_SRC = src/io/loci_hw.c $(LOCI_USB_DIR)/host/loci_usb_client.c
LOCI_EMUL_LIB =
LOCI_EMU_CFLAGS = -I$(LOCI_USB_DIR)/host -I$(LOCI_USB_DIR)/proto -DNO_LOCI_EMU
else ifeq ($(LOCI_EMU),1)
LOCI_EMU_SRC = src/io/loci_emu.c
LOCI_EMUL_LIB = $(LOCI_EMUL_DIR)/libemul.a
LOCI_EMU_CFLAGS = -I$(LOCI_EMUL_DIR)/src
else
LOCI_EMU_SRC = src/io/loci_emu_stub.c
LOCI_EMUL_LIB =
LOCI_EMU_CFLAGS = -DNO_LOCI_EMU
endif
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -I./include $(LOCI_EMU_CFLAGS) -MMD -MP
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
    # Prefer pkg-config; fall back to sdl2-config (robust on macOS/Homebrew where
    # PKG_CONFIG_PATH may not point at the keg — `brew install sdl2` ships both).
    SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
    SDL2_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
    ifeq ($(strip $(SDL2_LIBS)),)
        SDL2_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
        SDL2_LIBS   := $(shell sdl2-config --libs 2>/dev/null)
    endif
    CFLAGS  += -DHAS_SDL2 $(SDL2_CFLAGS)
    LDFLAGS += $(SDL2_LIBS)
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

# SANITIZE=1 : AddressSanitizer + UndefinedBehaviorSanitizer (dossier de build
# `-san`). Toute erreur UBSan arrête le programme (-fno-sanitize-recover) : un
# test qui la déclenche échoue au lieu de seulement l'afficher.
SANITIZE ?= 0
ifeq ($(SANITIZE), 1)
    CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1
    LDFLAGS += -fsanitize=address,undefined
    # third_party/stb_image_write.h (v1.16, laissé intact) décale un `int` signé
    # au-delà de son bit de signe dans l'écriture des bits JPEG
    # (stbiw__jpg_writeBits) : exemption limitée à son unité de compilation.
    STB_SAN_CFLAGS = -fno-sanitize=shift
    # Fuites de bibliothèques tierces (SDL2) ignorées : tests/lsan.supp.
    export LSAN_OPTIONS = suppressions=$(CURDIR)/tests/lsan.supp:print_suppressions=0
endif

# Source files
# Cartes d'extension en modules (ADR 0006) : registre (card_modules.c), une
# carte par fichier (card_<id>.c, pris par motif : ajouter une carte ne touche
# pas au Makefile ; sa ligne va dans include/cards_list.h), et le registre des
# sources audio qu'elles alimentent.
CARD_MODULE_SRCS = $(sort $(wildcard src/cards/card_*.c)) src/audio/audio_sources.c

SOURCES = src/main.c \
          src/rom_patches.c \
          src/cards.c \
          $(CARD_MODULE_SRCS) \
          src/cpu/cpu6502.c \
          src/cpu/opcodes.c \
          src/cpu/addressing.c \
          src/cpu/microseq.c \
          src/memory/memory.c \
          src/memory/banking.c \
          src/io/via6522.c \
          src/io/keyboard.c \
          src/io/joystick.c \
          src/io/printer.c \
          src/io/mcp40.c \
          src/io/cassette.c \
          src/io/microdisc.c \
          src/io/jasmin.c \
          src/io/sp0256.c \
          src/io/mea8000.c \
          src/io/loci_core.c \
          src/io/loci_gfx.c \
          $(LOCI_EMU_SRC) \
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
          src/video/iomenu.c \
          src/iomenu_glue.c \
          src/audio/ay3891x.c \
          src/audio/audio_output.c \
          src/storage/tap.c \
          src/storage/sedoric.c \
          src/storage/disk.c \
          src/storage/disk_http.c \
          src/hostfs/hostfs.c \
          src/hostfs/vfs.c \
          src/emu_clock.c \
          src/savestate.c \
          src/debugger.c src/debugger_view.c src/debugger_mem.c src/debugger_asm.c src/debugger_repl.c \
          src/network/gdbstub.c \
          src/control.c src/control_util.c src/control_cmd_mem.c src/control_cmd_debug.c src/control_cmd_media.c \
          src/control_queue.c \
          src/utils/logging.c \
          src/utils/config.c \
          src/utils/trace.c \
          src/utils/cycle_trace.c \
          src/utils/profiler.c \
          src/utils/rominfo.c \
          src/utils/symbols.c \
          src/utils/movie.c \
          src/utils/netutil.c \
          src/utils/appsignal.c \
          src/cli/cli_usage.c \
          src/cli/cli_parse.c \
          src/cli/cli_opts.c \
          src/cli/cli_args.c \
          src/io/tape_patches.c \
          src/io/loci_glue.c

ifeq ($(CAST), 1)
    SOURCES += src/network/cast_server.c src/network/castv2.c
endif

ifeq ($(HTTPAPI), 1)
    SOURCES += src/network/http_api.c
endif


# Windows v1 : swap the POSIX-only modules for their Windows variants
ifeq ($(WIN), 1)
    SOURCES := $(filter-out src/io/serial_backend.c src/io/serial_picowifi.c \
                            src/network/gdbstub.c src/storage/disk_http.c, $(SOURCES))
    SOURCES += src/io/serial_backend_win.c src/network/gdbstub_win.c \
               src/storage/disk_http_win.c
endif

TUI ?= 0
ifeq ($(TUI), 1)
    SOURCES += src/tui.c
    CFLAGS  += -DHAS_TUI
    LDFLAGS += -lncursesw
endif

# VIA_NO_LAZY=1 : VIA pas-à-pas à chaque cycle (référence de la preuve
# d'équivalence du chemin paresseux, `make test-via-lazy`).
VIA_NO_LAZY ?= 0
ifeq ($(VIA_NO_LAZY), 1)
    CFLAGS += -DVIA_NO_LAZY
endif

# Build hors des sources, un répertoire par configuration : les objets d'une
# variante (SDL2=0, HTTPAPI=1, backend LOCI…) ne se mélangent jamais avec ceux
# d'une autre. Les binaires finaux sont recopiés à la racine (chemins attendus
# par les scripts), seulement quand leur contenu change.
LOCI_BACKEND = $(if $(filter 1,$(LOCI_NEO)),neo,$(if $(filter 1,$(LOCI_HW)),hw,$(if $(filter 1,$(LOCI_EMU)),emu,stub)))
CONFIG := $(if $(filter 1,$(WIN)),win,host)-sdl$(SDL2)-http$(HTTPAPI)-cast$(CAST)-midi$(MIDI)-tls$(PICOTLS)-tui$(TUI)-loci$(LOCI_BACKEND)$(if $(filter 1,$(DEBUG)),-debug)$(if $(filter 1,$(COVERAGE)),-cov)$(if $(filter 1,$(VIA_NO_LAZY)),-vianolazy)$(if $(filter 1,$(SANITIZE)),-san)
BUILD ?= build/$(CONFIG)
TBIN = $(BUILD)/tests
# $(call obj,sources.c) → objets correspondants dans $(BUILD)
obj = $(patsubst %.c,$(BUILD)/%.o,$(1))

OBJECTS = $(call obj,$(SOURCES))

# Core libraries (no main)
LIB_SOURCES = $(filter-out src/main.c, $(SOURCES))
LIB_OBJECTS = $(call obj,$(LIB_SOURCES))

# Tools
TOOL_SOURCES = src/storage/tap.c src/utils/logging.c

# Targets
TARGET = oric1-emu$(EXE)
TOOLS = bas2tap bin2tap tap2sedoric sedoric-info tap2wav dsk2hfe

# Install paths
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
DATADIR = $(PREFIX)/share/phosphoric
DOCDIR = $(PREFIX)/share/doc/phosphoric

.PHONY: all release dist clean tools tests tests-strict valgrind-core coverage-check test-via-lazy test-gdb-bind test-fuzz-replay test-dsk2hfe test-ci-apt-install test-suite-targets fuzz test-http-parse test-cards test-check-skips test-cli-golden test-cli-golden-self FORCE test-cpu test-memory test-io test-ula-ng test-jasmin test-storage test-system test-rom test-video test-avi test-audio test-debugger test-gdbstub test-movie test-movie-replay test-cast test-savestate test-atmos test-joystick test-sp0256 test-mea8000 test-printer test-mcp40 test-renderer test-osd test-iomenu test-iomenu-glue test-trace test-profiler test-rominfo test-serial test-serial-backends test-pia6821 test-acia6850 test-dtl2000 test-dtl2000-txrx test-midi test-smf test-serial-file test-picowifi test-keyboard test-autotype test-symbols test-loci test-loci-acia-miss test-loci-sdimg test-loci-sdimg-write test-loci-e2e test-loci-acia-e2e test-web-loci test-web-picowifi test-web-iomenu test-loci-golden test-control test-game-compat test-mc-autorun test-control-dispatch test-control-queue test-httpapi test-loadstate test-sedoric-tools test-ula-ng-visible test-docs-claims test-comment-diff test-clock test-cycle test-dormann test-raster-split test-tape-signal test-savestate-determinism test-bench test-corpus fetch-vectors bench valgrind static-analysis cppcheck flawfinder security-check coverage coverage-report install uninstall help wasm

all: $(TARGET)

FORCE:

$(BUILD)/bin/$(TARGET): $(OBJECTS) $(LOCI_EMUL_LIB)
	@mkdir -p $(@D)
	$(CC) $(OBJECTS) $(LOCI_EMUL_LIB) $(LDFLAGS) -o $@

# Copie à la racine, refaite à chaque appel mais seulement si le binaire de la
# configuration courante diffère (changer de configuration change de binaire).
# Copie + renommage : `cp` échouerait (« Text file busy ») si un autre processus
# exécute le binaire racine ; `mv` remplace l'entrée sans toucher au fichier ouvert.
$(TARGET): $(BUILD)/bin/$(TARGET) FORCE
	@cmp -s $< $@ || { cp $< $@.tmp && mv -f $@.tmp $@; }

# Construit la bibliothèque de l'émulateur RP2040 si absente (LOCI_EMU=1 seulement).
ifeq ($(LOCI_EMU),1)
$(LOCI_EMUL_DIR)/libemul.a:
	$(MAKE) -C $(LOCI_EMUL_DIR) lib
endif

# Copie strippée pour la distribution (symboles retirés → binaire plus petit).
# Produit $(TARGET)-release SANS toucher au binaire de travail $(TARGET)
# (utilisé par d'autres programmes). Epic 7 / US1, Sprint 125.
release: $(TARGET)
	cp $(TARGET) $(TARGET)-release
	strip $(TARGET)-release
	@echo "Binaire de distribution : $(TARGET)-release ($$(stat -c%s $(TARGET)-release) o, vs $$(stat -c%s $(TARGET)) o non strippé)"

# Binaire de distribution UNIQUE : une seule version « riche » au lieu d'une
# multitude de variantes de build. TOUTES les fonctionnalités optionnelles sont
# compilées (SDL2, CAST, HTTPAPI, MIDI, PICOTLS) ; c'est ensuite au runtime que
# les flags CLI (--http-api, --cast, --serial midi/picowifi…) décident de ce qui
# est actif. Link dynamique : SDL2/OpenSSL/ALSA restent des dépendances système
# à déclarer dans le paquet (.deb/.rpm). Produit $(TARGET)-dist strippé.
# Rappel : Linux, Windows (WIN=1) et WASM restent des cibles distinctes.
dist:
	$(MAKE) SDL2=1 CAST=1 HTTPAPI=1 MIDI=1 PICOTLS=1
	cp $(TARGET) $(TARGET)-dist
	strip $(TARGET)-dist
	@echo "Binaire de distribution complet : $(TARGET)-dist ($$(stat -c%s $(TARGET)-dist) o)"
	@echo "Dépendances runtime : libSDL2, libssl/libcrypto, libasound (ALSA)"

tools: $(TOOLS)

# Outil = binaire lié dans $(BUILD)/bin puis recopié à la racine.
#   $(1) nom   $(2) sources
define TOOL_BIN
$(BUILD)/bin/$(1): $(call obj,$(2))
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $$^ $$(LDFLAGS) -o $$@
$(1): $(BUILD)/bin/$(1) FORCE
	@cmp -s $$< $$@ || { cp $$< $$@.tmp && mv -f $$@.tmp $$@; }
endef
$(eval $(call TOOL_BIN,bas2tap,tools/bas2tap.c $(TOOL_SOURCES)))
$(eval $(call TOOL_BIN,bin2tap,tools/bin2tap.c $(TOOL_SOURCES)))
$(eval $(call TOOL_BIN,tap2sedoric,tools/tap2sedoric.c $(TOOL_SOURCES) src/storage/sedoric.c))
$(eval $(call TOOL_BIN,sedoric-info,tools/sedoric_info.c))
$(eval $(call TOOL_BIN,tap2wav,tools/tap2wav.c))
$(eval $(call TOOL_BIN,dsk2hfe,tools/dsk2hfe.c))

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/src/video/stb_image_write_impl.o: CFLAGS += $(STB_SAN_CFLAGS)

# Include auto-generated header-dependency files (-MMD output).
# Silent if absent (first build / after clean).
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

# ═══════════════════════════════════════════════════════════════
#  TESTS
# ═══════════════════════════════════════════════════════════════

# Aucun test ne doit dépendre d'un phosphoric.cfg personnel (menu F1).
export PHOSPHORIC_NO_CONFIG = 1

# Tests unitaires : chaque source est compilée UNE fois par configuration dans
# $(BUILD) ; le binaire de test lie exactement la liste d'objets donnée.
#   $(1) cible   $(2) binaire   $(3) sources   $(4) ldflags en plus   $(5) archives
define UNIT_TEST
$(TBIN)/$(2): $(call obj,$(3)) $(5)
	@mkdir -p $$(@D)
	@$$(CC) $$(CFLAGS) $(call obj,$(3)) $(5) $$(LDFLAGS) $(4) -o $$@
$(1): $(TBIN)/$(2)
	@$(TBIN)/$(2)
endef

# Tests dont les sources exigent des drapeaux PROPRES ($(4)), indépendants de la
# configuration (clavier toujours avec SDL2, cast toujours avec HAS_CAST) :
# compilation directe, refaite à chaque appel comme auparavant.
define DIRECT_TEST
$(TBIN)/$(2): $(3) FORCE
	@mkdir -p $$(@D)
	@$$(CC) $$(filter-out -MMD -MP,$$(CFLAGS)) $(4) $(3) $$(LDFLAGS) $(5) -o $$@
$(1): $(TBIN)/$(2)
	@$(TBIN)/$(2)
endef

# Briques partagées par les listes de sources des tests
CPU_SRCS = src/cpu/cpu6502.c src/cpu/opcodes.c src/cpu/addressing.c src/cpu/microseq.c
MEM_SRCS = src/memory/memory.c src/memory/banking.c
DISK_SRCS = src/storage/disk.c src/storage/disk_http.c src/storage/sedoric.c
LOCI_STUB = tests/support/loci_emu_stub.c

TEST_CPU_SRCS = tests/support/loci_emu_stub.c tests/unit/test_cpu.c $(CPU_SRCS) \
                 $(MEM_SRCS) \
                src/utils/logging.c

TEST_MEM_SRCS = tests/support/loci_emu_stub.c tests/unit/test_memory.c src/memory/memory.c \
                src/memory/banking.c src/utils/logging.c

TEST_IO_SRCS = tests/unit/test_io.c src/io/via6522.c src/utils/logging.c

TEST_ULA_NG_SRCS = tests/unit/test_ula_ng.c src/io/ula_ng.c

TEST_CASSETTE_SRCS = tests/unit/test_cassette.c src/io/cassette.c src/io/via6522.c

TEST_STORAGE_SRCS = tests/unit/test_storage.c src/storage/sedoric.c \
                    src/storage/disk.c src/storage/disk_http.c \
                    src/io/microdisc.c src/utils/logging.c

TEST_JASMIN_SRCS = tests/unit/test_jasmin.c src/io/jasmin.c src/io/microdisc.c \
                   src/storage/sedoric.c src/storage/disk.c \
                   src/storage/disk_http.c src/utils/logging.c

TEST_SYSTEM_SRCS = tests/support/loci_emu_stub.c tests/unit/test_full_system.c src/cpu/cpu6502.c \
                   src/cpu/opcodes.c src/cpu/addressing.c src/cpu/microseq.c src/memory/memory.c \
                   src/memory/banking.c src/io/via6522.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-cpu,test_cpu,$(TEST_CPU_SRCS),,))

$(eval $(call UNIT_TEST,test-memory,test_memory,$(TEST_MEM_SRCS),,))

$(eval $(call UNIT_TEST,test-ula-ng,test_ula_ng,$(TEST_ULA_NG_SRCS),,))

$(eval $(call UNIT_TEST,test-io,test_io,$(TEST_IO_SRCS),,))

$(eval $(call UNIT_TEST,test-cassette,test_cassette,$(TEST_CASSETTE_SRCS),,))

$(eval $(call UNIT_TEST,test-jasmin,test_jasmin,$(TEST_JASMIN_SRCS),,))

$(eval $(call UNIT_TEST,test-storage,test_storage,$(TEST_STORAGE_SRCS),,))

$(eval $(call UNIT_TEST,test-system,test_system,$(TEST_SYSTEM_SRCS),,))

TEST_ROM_SRCS = tests/support/loci_emu_stub.c tests/unit/test_rom.c $(CPU_SRCS) \
                 $(MEM_SRCS) \
                src/io/via6522.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-rom,test_rom,$(TEST_ROM_SRCS),,))

TEST_VIDEO_SRCS = tests/support/loci_emu_stub.c tests/unit/test_video.c src/video/video.c src/video/export.c \
                  src/video/stb_image_write_impl.c \
                  $(CPU_SRCS) \
                  $(MEM_SRCS) src/io/via6522.c \
                  src/io/ula_ng.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-video,test_video,$(TEST_VIDEO_SRCS),,))

TEST_AVI_SRCS = tests/unit/test_avi.c src/video/avi_recorder.c \
                src/video/stb_image_write_impl.c

$(eval $(call UNIT_TEST,test-avi,test_avi,$(TEST_AVI_SRCS),,))

TEST_MOVIE_SRCS = tests/unit/test_movie.c src/utils/movie.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-movie,test_movie,$(TEST_MOVIE_SRCS),,))

test-movie-replay: $(TARGET)
	@bash tests/integration/test_movie_replay.sh

TEST_GDB_SRCS = tests/support/loci_emu_stub.c tests/unit/test_gdbstub.c src/network/gdbstub.c src/debugger.c src/debugger_view.c src/debugger_mem.c src/debugger_asm.c src/debugger_repl.c \
                $(CPU_SRCS) \
                $(MEM_SRCS) \
                src/io/via6522.c src/utils/logging.c src/utils/symbols.c \
                src/utils/trace.c

$(eval $(call UNIT_TEST,test-gdbstub,test_gdbstub,$(TEST_GDB_SRCS),,))

TEST_AUDIO_SRCS = tests/unit/test_audio.c src/audio/ay3891x.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-audio,test_audio,$(TEST_AUDIO_SRCS),,))

TEST_DEBUGGER_SRCS = tests/support/loci_emu_stub.c tests/unit/test_debugger.c src/debugger.c src/debugger_view.c src/debugger_mem.c src/debugger_asm.c src/debugger_repl.c \
                     $(CPU_SRCS) \
                     $(MEM_SRCS) \
                     src/io/via6522.c src/utils/logging.c src/utils/symbols.c \
                     src/utils/trace.c

$(eval $(call UNIT_TEST,test-debugger,test_debugger,$(TEST_DEBUGGER_SRCS),,))

# stb_image_write : objet de la build (exempté de -fsanitize=shift, cf.
# STB_SAN_CFLAGS), pas sa source, que DIRECT_TEST compilerait avec tous les
# sanitizers (erreur UBSan dans stbiw__jpg_writeBits en CI sanitizers, 2.12.7).
TEST_CAST_SRCS = tests/unit/test_cast.c src/network/cast_server.c src/network/castv2.c \
                 $(BUILD)/src/video/stb_image_write_impl.o src/utils/logging.c

# Le serveur cast chiffre en TLS : sans OpenSSL (pkg-config openssl, cf.
# PICOTLS), le test est sauté au lieu d'échouer à la compilation.
ifeq ($(PICOTLS),1)
$(eval $(call DIRECT_TEST,test-cast,test_cast,$(TEST_CAST_SRCS),-DHAS_CAST,-lpthread -lssl -lcrypto))
else
test-cast:
	@echo "  SKIP test-cast: OpenSSL absent (pkg-config openssl)"
endif

TEST_SAVESTATE_SRCS = tests/support/loci_emu_stub.c tests/unit/test_savestate.c src/savestate.c src/io/jasmin.c \
                      $(CPU_SRCS) \
                      $(MEM_SRCS) \
                      src/io/via6522.c src/io/keyboard.c src/io/microdisc.c \
                      src/audio/ay3891x.c src/video/video.c src/io/ula_ng.c \
                      $(DISK_SRCS) \
                      src/utils/logging.c

$(eval $(call UNIT_TEST,test-savestate,test_savestate,$(TEST_SAVESTATE_SRCS),,))

TEST_ATMOS_SRCS = tests/support/loci_emu_stub.c tests/unit/test_atmos.c src/memory/memory.c \
                  src/memory/banking.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-atmos,test_atmos,$(TEST_ATMOS_SRCS),,))

TEST_MCP40_SRCS = tests/unit/test_mcp40.c src/io/mcp40.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-mcp40,test_mcp40,$(TEST_MCP40_SRCS),,))

TEST_PRINTER_SRCS = tests/unit/test_printer.c src/io/printer.c src/io/mcp40.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-printer,test_printer,$(TEST_PRINTER_SRCS),,))

TEST_JOYSTICK_SRCS = tests/unit/test_joystick.c src/io/joystick.c src/io/via6522.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-joystick,test_joystick,$(TEST_JOYSTICK_SRCS),,))

TEST_SP0256_SRCS = tests/unit/test_sp0256.c src/io/sp0256.c

$(eval $(call UNIT_TEST,test-sp0256,test_sp0256,$(TEST_SP0256_SRCS),,))

TEST_MEA8000_SRCS = tests/unit/test_mea8000.c src/io/mea8000.c

$(eval $(call UNIT_TEST,test-mea8000,test_mea8000,$(TEST_MEA8000_SRCS),-lm,))

TEST_RENDERER_SRCS = tests/support/loci_emu_stub.c tests/unit/test_renderer.c src/video/video.c src/video/renderer.c \
                     src/io/ula_ng.c \
                     $(MEM_SRCS) src/utils/logging.c

$(eval $(call UNIT_TEST,test-renderer,test_renderer,$(TEST_RENDERER_SRCS),,))

TEST_OSD_SRCS = tests/unit/test_osd.c src/video/osd.c

# Menu des périphériques E/S (F1) : modèle, touches, sélecteur, dessin.
# Le registre des cartes lie les cartes en modules, donc leurs périphériques :
# la bibliothèque entière, comme test-cards.
$(eval $(call UNIT_TEST,test-iomenu,test_iomenu,tests/unit/test_iomenu.c $(LIB_SOURCES),,$(LOCI_EMUL_LIB)))

# Liaison menu F1 ↔ émulateur (médias, réglages, phosphoric.cfg) : lie le cœur.
$(eval $(call UNIT_TEST,test-iomenu-glue,test_iomenu_glue,tests/unit/test_iomenu_glue.c $(LIB_SOURCES),,$(LOCI_EMUL_LIB)))

$(eval $(call UNIT_TEST,test-osd,test_osd,$(TEST_OSD_SRCS),,))


TEST_TRACE_SRCS = tests/support/loci_emu_stub.c tests/unit/test_trace.c src/utils/trace.c \
                  $(CPU_SRCS) \
                  $(MEM_SRCS) src/utils/logging.c \
                  src/utils/symbols.c

$(eval $(call UNIT_TEST,test-trace,test_trace,$(TEST_TRACE_SRCS),,))

TEST_PROFILER_SRCS = tests/support/loci_emu_stub.c tests/unit/test_profiler.c src/utils/profiler.c \
                     $(CPU_SRCS) \
                     $(MEM_SRCS) src/utils/logging.c

$(eval $(call UNIT_TEST,test-profiler,test_profiler,$(TEST_PROFILER_SRCS),,))

TEST_ROMINFO_SRCS = tests/support/loci_emu_stub.c tests/unit/test_rominfo.c src/utils/rominfo.c \
                    $(CPU_SRCS) \
                    $(MEM_SRCS) src/utils/logging.c

$(eval $(call UNIT_TEST,test-rominfo,test_rominfo,$(TEST_ROMINFO_SRCS),,))

TEST_SYMBOLS_SRCS = tests/unit/test_symbols.c src/utils/symbols.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-symbols,test_symbols,$(TEST_SYMBOLS_SRCS),,))

TEST_LOCI_SRCS = tests/support/loci_emu_stub.c tests/unit/test_loci.c \
                 src/io/loci_core.c src/io/loci_gfx.c src/io/loci_fs.c \
                 src/io/loci_bus.c src/io/loci_boot.c src/io/loci_sdimg.c \
                 src/utils/logging.c $(DISK_SRCS) \
                 $(CPU_SRCS) \
                 $(MEM_SRCS)

$(eval $(call UNIT_TEST,test-loci,test_loci,$(TEST_LOCI_SRCS),,))

# Course PHI2 du LOCI sur l'ACIA $0380 (picowifi) — dispatch io_bus complet, donc
# tout l'arbre des périphériques de page 3 est lié.
TEST_LOCI_ACIA_MISS_SRCS = tests/unit/test_loci_acia_miss.c src/io/io_bus.c \
                 src/io/acia6551.c src/io/serial_backend.c src/io/smf.c \
                 $(LOCI_EMU_SRC) src/io/loci_gfx.c \
                 src/io/loci_core.c src/io/loci_fs.c src/io/loci_bus.c \
                 src/io/loci_boot.c src/io/loci_sdimg.c \
                 src/io/microdisc.c src/io/jasmin.c src/io/mageco.c \
                 src/io/pia6821.c src/io/acia6850.c src/io/dtl2000.c \
                 src/io/sp0256.c src/io/mea8000.c src/io/ula_ng.c \
                 src/video/video.c src/audio/ay3891x.c \
                 $(CPU_SRCS) \
                 $(DISK_SRCS) \
                 $(MEM_SRCS) \
                 src/utils/logging.c src/utils/netutil.c $(CARD_MODULE_SRCS) \
                 src/cli/cli_parse.c src/io/serial_picowifi.c

$(eval $(call UNIT_TEST,test-loci-acia-miss,test_loci_acia_miss,$(TEST_LOCI_ACIA_MISS_SRCS),-lutil,$(LOCI_EMUL_LIB)))

TEST_LOCI_SDIMG_SRCS = tests/unit/test_loci_sdimg.c src/io/loci_sdimg.c \
                       src/utils/logging.c

$(eval $(call UNIT_TEST,test-loci-sdimg,test_loci_sdimg,$(TEST_LOCI_SDIMG_SRCS),,))

TEST_LOCI_SDIMG_WRITE_SRCS = tests/unit/test_loci_sdimg_write.c \
                             src/io/loci_sdimg.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-loci-sdimg-write,test_loci_sdimg_write,$(TEST_LOCI_SDIMG_WRITE_SRCS),,))

TEST_SERIAL_SRCS = tests/unit/test_serial.c src/io/acia6551.c \
                   src/io/serial_backend.c src/io/smf.c src/utils/netutil.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-serial,test_serial,$(TEST_SERIAL_SRCS),-lutil,))

# Transports série (modem AT, tcp, pty, port COM, smf) : hermétique (127.0.0.1, openpty).
TEST_SERIAL_BACKENDS_SRCS = tests/unit/test_serial_backends.c src/io/acia6551.c \
                   src/io/serial_backend.c src/io/smf.c src/utils/netutil.c src/utils/logging.c
$(eval $(call UNIT_TEST,test-serial-backends,test_serial_backends,$(TEST_SERIAL_BACKENDS_SRCS),-lutil,))

TEST_PIA6821_SRCS = tests/unit/test_pia6821.c src/io/pia6821.c

$(eval $(call UNIT_TEST,test-pia6821,test_pia6821,$(TEST_PIA6821_SRCS),,))

TEST_ACIA6850_SRCS = tests/unit/test_acia6850.c src/io/acia6850.c

$(eval $(call UNIT_TEST,test-acia6850,test_acia6850,$(TEST_ACIA6850_SRCS),,))

TEST_DTL2000_SRCS = tests/unit/test_dtl2000.c src/io/dtl2000.c src/io/pia6821.c \
                    src/io/acia6850.c \
                    src/io/serial_backend.c src/io/smf.c src/utils/netutil.c src/io/acia6551.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-dtl2000,test_dtl2000,$(TEST_DTL2000_SRCS),-lutil,))

# Mageco MIDI interface — MC6850 ACIA at $03FE, 31250 baud (forum t=2525)
TEST_MIDI_SRCS = tests/unit/test_midi.c src/io/mageco.c src/io/acia6850.c \
                 src/io/serial_backend.c src/io/smf.c src/utils/netutil.c src/io/acia6551.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-midi,test_midi,$(TEST_MIDI_SRCS),-lutil,))

# Standard MIDI File (.mid) parser — timed MIDI IN replay source
TEST_SMF_SRCS = tests/unit/test_smf.c src/io/smf.c

$(eval $(call UNIT_TEST,test-smf,test_smf,$(TEST_SMF_SRCS),,))

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

$(eval $(call UNIT_TEST,test-picowifi,test_picowifi,$(TEST_PICOWIFI_SRCS),,))

TEST_KEYBOARD_SRCS = tests/unit/test_keyboard.c src/io/keyboard.c src/utils/logging.c

$(eval $(call DIRECT_TEST,test-keyboard,test_keyboard,$(TEST_KEYBOARD_SRCS),-DHAS_SDL2 $(shell pkg-config --cflags sdl2 2>/dev/null),$(shell pkg-config --libs sdl2 2>/dev/null)))

TEST_AUTOTYPE_SRCS = tests/unit/test_autotype.c

$(eval $(call UNIT_TEST,test-autotype,test_autotype,$(TEST_AUTOTYPE_SRCS),,))

TEST_COVERAGE_SRCS = tests/support/loci_emu_stub.c tests/unit/test_coverage.c $(CPU_SRCS) \
                      $(MEM_SRCS) \
                     src/io/via6522.c src/io/keyboard.c src/io/joystick.c \
                     src/io/printer.c src/io/mcp40.c src/io/microdisc.c \
                     src/storage/sedoric.c src/storage/disk.c src/storage/disk_http.c \
                     src/savestate.c src/io/jasmin.c src/debugger.c src/debugger_view.c src/debugger_mem.c src/debugger_asm.c src/debugger_repl.c \
                     src/audio/ay3891x.c src/video/video.c src/io/ula_ng.c \
                     src/utils/logging.c src/utils/symbols.c src/utils/trace.c

$(eval $(call UNIT_TEST,test-coverage,test_coverage,$(TEST_COVERAGE_SRCS),,))

# E2E regression of the LOCI/Sedoric pipe (sprints 34b0/b1/b2).
# Skipped gracefully when required ROM/disk assets are absent.
test-loci-e2e:
	@bash tests/integration/test_loci_sedoric_e2e.sh

# E2E web : cartouche LOCI dans la build WASM (Chrome headless + Playwright ;
# SKIP si emcc/node/Playwright/Chrome absents). Hors `make tests` (lourd).
test-web-loci:
	@bash tests/integration/test_web_loci.sh

# E2E web : modem picowifi WASM via tools/picowifi_ws_relay.py (BASIC -> ACIA ->
# WebSocket -> relais -> TCP local, et retour ; sans relais : fetch, httpsame, LOCI+cassette).
# Mêmes prérequis/SKIP, hors `make tests`.
test-web-picowifi:
	@bash tests/integration/test_web_picowifi.sh

# E2E web : menu des périphériques (F1) dans la build WASM (F1 soustrait au
# navigateur, pause, clavier virtuel, bouton I/O). Mêmes prérequis/SKIP, hors `make tests`.
test-web-iomenu:
	@bash tests/integration/test_web_iomenu.sh

# E2E: BASIC drives the LOCI ACIA 6551 at $0380 (TX + RX round-trip).
test-loci-acia-e2e: $(TARGET)
	@bash tests/integration/test_loci_acia_e2e.sh

test-loci-golden: $(TARGET)
	@bash tests/integration/test_loci_golden.sh

# --control IPC media hot-swap commands (load-disk / eject-disk / eject-tape).
test-control: $(TARGET)
	@bash tests/integration/test_control_media_swap.sh

# Sprint 92 (Epic 1) — transport-agnostic control_dispatch via a buffer sink.
# Links the core library objects (no main) and drives control_dispatch()
# directly, asserting byte-exact replies + CONTINUE/RESUME/QUIT results.
$(eval $(call UNIT_TEST,test-control-dispatch,test_control_dispatch,tests/unit/test_control_dispatch.c $(LIB_SOURCES),,$(LOCI_EMUL_LIB)))

# Sprint 93 (Epic 2) — thread-safe command queue. Spawns producer threads that
# submit() concurrently while a consumer thread drain()s per "frame", asserting
# correct per-producer routing (unique addr write/read) and zero corruption.
$(eval $(call UNIT_TEST,test-control-queue,test_control_queue,tests/unit/test_control_queue.c $(LIB_SOURCES),,$(LOCI_EMUL_LIB)))

# Sprint 94 (Epic 3) — HTTP control API end-to-end (curl vs a live headless
# emulator). Skips gracefully unless the emulator was built with HTTPAPI=1.
test-httpapi: $(TARGET)
	@bash tests/integration/test_http_api_e2e.sh

# Chargement cassette natif (tape_patches.c) : synchro, lecture, cassette épuisée.
$(eval $(call UNIT_TEST,test-tape-patches,test_tape_patches,tests/unit/test_tape_patches.c $(LIB_SOURCES),,$(LOCI_EMUL_LIB)))

# Registre des cartes d'extension (menu F1) : état, conflits, relance.
$(eval $(call UNIT_TEST,test-cards,test_cards,tests/unit/test_cards.c $(LIB_SOURCES),,$(LOCI_EMUL_LIB)))

# Analyse/routage HTTP sans réseau ni émulateur (inclut http_api.c, toutes builds).
$(eval $(call UNIT_TEST,test-http-parse,test_http_parse,tests/unit/test_http_parse.c src/utils/logging.c,,))

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

test-audio-capture: $(TARGET)
	@bash tests/integration/test_audio_capture.sh

test-tape-roundtrip: $(TARGET)
	@bash tests/integration/test_tape_roundtrip.sh

test-cli-parsing: $(TARGET)
	@bash tests/integration/test_cli_parsing.sh

# V2-E6 — CLOAD au niveau signal sur les deux ROM (chemin non couvert jusque-là :
# test_tape_roundtrip recharge en fast-load).
test-tape-signal: $(TARGET) tools
	@bash tests/integration/test_tape_signal_load.sh

# V2-E4 — preuve du fetch ULA par cycle : un programme 6502 réécrit l'écran
# pendant le balayage ; la coupure doit tomber au milieu d'une ligne.
test-raster-split: $(TARGET) tools
	@bash tests/integration/test_raster_split.sh

# V2-S4 — horloge maître : un appel = un cycle de toute la machine.
TEST_CLOCK_SRCS = tests/support/loci_emu_stub.c tests/unit/test_clock.c \
                  src/emu_clock.c $(CPU_SRCS) \
                   \
                  $(MEM_SRCS) src/io/via6522.c \
                  src/video/video.c src/io/ula_ng.c src/utils/logging.c

$(eval $(call UNIT_TEST,test-clock,test_clock,$(TEST_CLOCK_SRCS),,))

# V2-S1 — oracle de conformité cycle par cycle (SingleStepTests/65x02) et test
# fonctionnel de Klaus Dormann. Les vecteurs ne sont pas versionnés : les deux
# cibles se mettent en SKIP quand ils sont absents (`tools/fetch_vectors.sh`).
#   make test-cycle                      200 cas par opcode (défaut)
#   make test-cycle CYCLE_MAX_CASES=0    les 10 000 cas par opcode
#   make test-cycle CYCLE_OPCODES=a9,b1  un sous-ensemble, en verbeux utile
TEST_CYCLE_SRCS = tests/support/loci_emu_stub.c tests/unit/test_cpu_cycles.c \
                  $(CPU_SRCS) \
                  $(MEM_SRCS) src/utils/logging.c

$(eval $(call UNIT_TEST,test-cycle,test_cpu_cycles,$(TEST_CYCLE_SRCS),,))

TEST_DORMANN_SRCS = tests/support/loci_emu_stub.c tests/unit/test_dormann.c \
                    $(CPU_SRCS) \
                    $(MEM_SRCS) src/utils/logging.c

$(eval $(call UNIT_TEST,test-dormann,test_dormann,$(TEST_DORMANN_SRCS),,))

# Récupération des vecteurs d'oracle (tiers, non versionnés, ~1 Go).
fetch-vectors:
	@bash tools/fetch_vectors.sh $(VECTORS)

# V2-S0 — garde-fou sur les allégations de précision temporelle (docs/ACCURACY.md).
# Échoue si « cycle-accurate » réapparaît non qualifié dans un document de vitrine.
test-docs-claims:
	@bash tests/integration/test_docs_claims.sh

# Miroir anglais main-en : seul le texte des commentaires peut différer de main.
test-comment-diff:
	@sh tests/integration/test_comment_only_diff.sh

# Menu des périphériques (F1) : --menu-screenshot, --config, phosphoric.cfg.
test-iomenu-cli: $(TARGET)
	@sh tests/integration/test_iomenu_cli.sh

# ─── Fuzzing des lecteurs de fichiers (tests/fuzz/, sprint E) ───────────────
# Une cible = un LLVMFuzzerTestOneInput(). Deux usages :
#   make test-fuzz-replay   gcc (sanitizers si SANITIZE=1) : rejoue les graines
#                           (tools/fuzz_seeds.sh) et tests/fuzz/regressions/<cible>/
#   make fuzz [FUZZ_TIME=60] [FUZZ_TARGETS="disk tap"]
#                           clang + libFuzzer + ASan/UBSan, FUZZ_TIME s par cible ;
#                           entrées fautives dans $(FUZZ_DIR)/crashes/ (à recopier
#                           dans tests/fuzz/regressions/<cible>/ une fois corrigées).
FUZZ_TARGETS ?= disk tap tapenat ost smf sym cfg http hfe
FUZZ_SRCS_disk = tests/fuzz/fuzz_disk.c $(DISK_SRCS) src/utils/logging.c
FUZZ_SRCS_tap  = tests/fuzz/fuzz_tap.c src/storage/tap.c src/utils/logging.c
FUZZ_SRCS_ost  = tests/fuzz/fuzz_ost.c $(filter-out tests/unit/test_savestate.c,$(TEST_SAVESTATE_SRCS))
FUZZ_SRCS_smf  = tests/fuzz/fuzz_smf.c src/io/smf.c
FUZZ_SRCS_sym  = tests/fuzz/fuzz_sym.c src/utils/symbols.c src/utils/logging.c
FUZZ_SRCS_cfg  = tests/fuzz/fuzz_cfg.c $(LIB_SOURCES)
FUZZ_LIBS_cfg  = $(LOCI_EMUL_LIB)
FUZZ_SRCS_tapenat = tests/fuzz/fuzz_tapenat.c $(LIB_SOURCES)
FUZZ_LIBS_tapenat = $(LOCI_EMUL_LIB)
FUZZ_SRCS_http = tests/fuzz/fuzz_http.c src/utils/logging.c
FUZZ_SRCS_hfe  = tests/fuzz/fuzz_hfe.c
FUZZ_OPTS_cfg  = -detect_leaks=0
FUZZ_DIR   = build/fuzz
FUZZ_TIME ?= 60
FUZZ_CC   ?= clang
FUZZ_CFLAGS = -std=c11 -I./include $(LOCI_EMU_CFLAGS) -g -O1 -fno-omit-frame-pointer \
              -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
              $(filter -D%,$(CFLAGS))

define FUZZ_REPLAY
$(TBIN)/fuzz_$(1): $(call obj,$(FUZZ_SRCS_$(1)) tests/fuzz/fuzz_replay.c) $(FUZZ_LIBS_$(1))
	@mkdir -p $$(@D)
	@$$(CC) $$(CFLAGS) $$^ $$(LDFLAGS) -o $$@
endef
$(foreach t,$(FUZZ_TARGETS),$(eval $(call FUZZ_REPLAY,$(t))))

# Outil dsk2hfe : structure HFE produite, images invalides refusées sans
# toucher à la sortie (lecture hors tampon sur image tronquée avant 2.12.4).
test-dsk2hfe: tools
	@bash tests/integration/test_dsk2hfe.sh

# Installation apt de la CI : tentative bloquée coupée puis relancée, abandon
# après 3 essais (faux sudo, ni réseau ni droits).
test-ci-apt-install:
	@sh tests/integration/test_ci_apt_install.sh

test-fuzz-replay: tools $(foreach t,$(FUZZ_TARGETS),$(TBIN)/fuzz_$(t))
	@echo "=== Fuzzing : rejeu des graines et des régressions ==="
	@sh tools/fuzz_seeds.sh $(BUILD)/fuzz-seeds $(TBIN) >/dev/null
	@set -e; for t in $(FUZZ_TARGETS); do \
	    printf '  %-5s' "$$t"; \
	    $(TBIN)/fuzz_$$t $(BUILD)/fuzz-seeds/$$t tests/fuzz/regressions/$$t; \
	done

fuzz: tools $(foreach t,$(FUZZ_TARGETS),$(TBIN)/fuzz_$(t))
	@command -v $(FUZZ_CC) >/dev/null || { echo "fuzz : $(FUZZ_CC) introuvable (libFuzzer requis)"; exit 1; }
	@sh tools/fuzz_seeds.sh $(FUZZ_DIR)/seeds $(TBIN) >/dev/null
	@mkdir -p $(FUZZ_DIR)/crashes
	@set -e; for t in $(FUZZ_TARGETS); do \
	    echo "=== fuzz_$$t ($(FUZZ_TIME) s) ==="; \
	    $(MAKE) --no-print-directory $(FUZZ_DIR)/fuzz_$$t; \
	    mkdir -p $(FUZZ_DIR)/corpus/$$t; \
	    cp -n $(FUZZ_DIR)/seeds/$$t/* $(FUZZ_DIR)/corpus/$$t/ 2>/dev/null || true; \
	    opts=$$(case $$t in cfg) echo "$(FUZZ_OPTS_cfg)";; esac); \
	    env $$(case $$t in cfg) echo ASAN_OPTIONS=detect_leaks=0;; esac) \
	    $(FUZZ_DIR)/fuzz_$$t $$opts -max_total_time=$(FUZZ_TIME) -print_final_stats=1 \
	        -artifact_prefix=$(FUZZ_DIR)/crashes/$$t- \
	        $(FUZZ_DIR)/corpus/$$t tests/fuzz/regressions/$$t > $(FUZZ_DIR)/$$t.log 2>&1 || true; \
	    grep -E '^stat::(number_of_executed_units|average_exec_per_sec|new_units_added)' $(FUZZ_DIR)/$$t.log; \
	    if ls $(FUZZ_DIR)/crashes/$$t-* >/dev/null 2>&1; then \
	        grep -E 'ERROR:|runtime error|SUMMARY:' $(FUZZ_DIR)/$$t.log | head -5; \
	        echo "fuzz_$$t : entrée fautive (journal : $(FUZZ_DIR)/$$t.log)"; exit 1; fi; \
	done

$(FUZZ_DIR)/fuzz_%: FORCE
	@mkdir -p $(@D)
	@$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SRCS_$*) $(FUZZ_LIBS_$*) $(filter-out -fsanitize=%,$(LDFLAGS)) -o $@

# Stub GDB : écoute sur 127.0.0.1 par défaut, --gdb-bind pour l'ouvrir.
test-gdb-bind: $(TARGET)
	@bash tests/integration/test_gdb_bind.sh

# Preuve d'équivalence du chemin paresseux de la VIA : même corpus de lignes de
# commande (tools/cli_golden.sh) sur le binaire courant et sur un binaire
# VIA_NO_LAZY=1 (pas-à-pas) — codes, sorties, fichiers produits à l'octet.
test-via-lazy: $(BUILD)/bin/$(TARGET)
	@$(MAKE) --no-print-directory VIA_NO_LAZY=1 BUILD=$(BUILD)-vianolazy $(BUILD)-vianolazy/bin/$(TARGET) >/dev/null
	@sh tools/cli_golden.sh $(BUILD)-vianolazy/bin/$(TARGET) $(BUILD)/bin/$(TARGET)

# Auto-test de tools/check_skips.sh (vérificateur de `make tests-strict`).
test-check-skips:
	@sh tests/integration/test_check_skips.sh

# Garde-fou des refactors de main()/du parseur : rejoue tests/cli_golden/cases.txt
# sur un binaire de RÉFÉRENCE (ex. construit depuis le commit précédent) et sur
# celui de la configuration courante ; tout écart (code, sorties, fichiers) échoue.
#   make test-cli-golden GOLDEN_REF=/chemin/vers/oric1-emu-de-reference
test-cli-golden: $(BUILD)/bin/$(TARGET)
	@test -n "$(GOLDEN_REF)" || { echo "GOLDEN_REF=/chemin/binaire/de/reference requis"; exit 1; }
	@sh tools/cli_golden.sh "$(GOLDEN_REF)" $(BUILD)/bin/$(TARGET)

# Toute cible test-* est dans `make tests`, ou exemptée par écrit dans
# tests/out_of_suite.txt (test-serial-backends en était absent depuis 2.11.1).
test-suite-targets:
	@python3 tools/check_test_targets.py Makefile tests/out_of_suite.txt

# Auto-test du harnais cli_golden (détecte un écart, n'en invente pas).
test-cli-golden-self: $(TARGET)
	@sh tests/integration/test_cli_golden.sh

# Sprint 36a — throughput benchmark. Runs 4 scenarios headless and
# reports MHz-equivalent / speed ratio vs real ORIC (1 MHz).
# Usage: `make bench`               human-readable table
#        `make bench BENCH_TSV=1`   tab-separated for tracking
#        `make bench CYCLES=200000000`  longer run
bench:
	@bash tools/bench.sh $(if $(BENCH_TSV),--tsv,)

# V2-E7 (US7.1) — budget de performance BLOQUANT : ≤ 5 % de la trame (1000 µs)
# sur le scénario boot BASIC. SKIP explicite sur machine bridée (batterie,
# fréquence effondrée) ; BENCH_STRICT=1 pour trancher quand même.
#   make test-bench BENCH_BUDGET_US=1500     # relever le plafond (CI lente)
test-bench: $(TARGET)
	@bash tools/bench_check.sh

# V2-E7 (US7.3) — corpus de non-régression : chaque média local rejoué un
# nombre fixe de cycles, écran comparé au manifeste tests/corpus/manifest.sha256.
# Re-baseline après un changement VOULU : tools/corpus_replay.sh snapshot
test-corpus: $(TARGET)
	@bash tools/corpus_replay.sh check

# V2-E7 (US7.2) — un savestate pris en PLEINE trame est un point de reprise
# exact : mêmes arrêts raster, même VIA, même RAM qu'un run ininterrompu.
test-savestate-determinism: $(TARGET)
	@python3 tests/integration/test_savestate_determinism.py

# Sprint 36b — game compatibility regression. Boots 7 commercial
# Oric titles from OricProgramsLib and checks each reaches a known
# intro screen. Skips gracefully when the lib is absent.
# Override path via ORIC_PROGRAMS_LIB=/path or BASIC_ROM=...
test-game-compat:
	@bash tests/integration/test_game_compat.sh

tests: tools test-cpu test-memory test-io test-ula-ng test-cassette test-jasmin test-storage test-system test-video test-avi test-audio test-debugger test-gdbstub test-movie test-movie-replay test-savestate test-atmos test-joystick test-sp0256 test-mea8000 test-printer test-mcp40 test-renderer test-osd test-trace test-profiler test-rominfo test-serial test-pia6821 test-acia6850 test-dtl2000 test-dtl2000-txrx test-midi test-smf test-serial-file test-picowifi test-keyboard test-autotype test-symbols test-loci test-loci-acia-miss test-loci-sdimg test-loci-sdimg-write test-loci-acia-e2e test-loci-golden test-control test-control-dispatch test-control-queue test-httpapi test-http-parse test-cards test-tape-patches test-coverage test-rom-guard test-loadstate test-sedoric-tools test-ula-ng-visible test-audio-capture test-tape-roundtrip test-cli-parsing test-docs-claims test-comment-diff test-check-skips test-cli-golden-self test-iomenu test-iomenu-glue test-rom test-mc-autorun test-loci-e2e test-iomenu-cli test-via-lazy test-gdb-bind test-fuzz-replay test-dsk2hfe test-ci-apt-install test-serial-backends test-cast test-game-compat test-web-loci test-web-iomenu test-web-picowifi test-suite-targets test-clock test-cycle test-dormann test-raster-split test-tape-signal test-savestate-determinism test-bench test-corpus
	@echo ""
	@echo "═══════════════════════════════════════════════════════"
	@echo "  All test suites completed!"
	@echo "═══════════════════════════════════════════════════════"

# Suite complète + refus des tests sautés sans raison autorisée
# (tests/allowed_skips.txt). Journal : $(BUILD)/tests.log. Utilisé par la CI.
tests-strict:
	@mkdir -p $(BUILD)
	@{ $(MAKE) --no-print-directory tests; echo $$? > $(BUILD)/tests.rc; } 2>&1 | tee $(BUILD)/tests.log
	@test "$$(cat $(BUILD)/tests.rc)" = 0 || { echo "FAIL: make tests (rc=$$(cat $(BUILD)/tests.rc))"; exit 1; }
	@sh tools/check_skips.sh $(BUILD)/tests.log

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

valgrind: test-cpu test-memory test-io test-jasmin test-jasmin test-storage test-system test-rom test-video test-audio test-debugger test-cast
	@echo "Running tests under Valgrind..."
	@valgrind --leak-check=full --error-exitcode=1 --quiet $(TBIN)/test_cpu
	@valgrind --leak-check=full --error-exitcode=1 --quiet $(TBIN)/test_memory
	@valgrind --leak-check=full --error-exitcode=1 --quiet $(TBIN)/test_io
	@valgrind --leak-check=full --error-exitcode=1 --quiet $(TBIN)/test_storage
	@valgrind --leak-check=full --error-exitcode=1 --quiet $(TBIN)/test_system
	@valgrind --leak-check=full --error-exitcode=1 --quiet $(TBIN)/test_rom
	@valgrind --leak-check=full --error-exitcode=1 --quiet $(TBIN)/test_video
	@valgrind --leak-check=full --error-exitcode=1 --quiet $(TBIN)/test_audio
	@valgrind --leak-check=full --error-exitcode=1 --quiet $(TBIN)/test_debugger
	@valgrind --leak-check=full --error-exitcode=1 --quiet $(TBIN)/test_cast
	@echo ""
	@echo "═══════════════════════════════════════════════════════"
	@echo "  Valgrind: No memory leaks detected!"
	@echo "═══════════════════════════════════════════════════════"

# Suites cœur sous Valgrind (job CI) : binaires pris dans $(TBIN).
VALGRIND_CORE = test_cpu test_memory test_io test_clock test_savestate test_audio
valgrind-core: test-cpu test-memory test-io test-clock test-savestate test-audio
	@for t in $(VALGRIND_CORE); do \
		valgrind --leak-check=full --error-exitcode=1 --quiet $(TBIN)/$$t >/dev/null || exit 1; \
	done
	@echo "Valgrind (core suites): OK"

# ═══════════════════════════════════════════════════════════════
#  CODE COVERAGE
# ═══════════════════════════════════════════════════════════════

coverage:
	@echo "Building and running tests with coverage instrumentation..."
	@$(MAKE) clean --no-print-directory
	@$(MAKE) tests COVERAGE=1 --no-print-directory
	@echo ""
	@echo "Generating coverage report..."
	@$(MAKE) coverage-report COVERAGE=1 --no-print-directory

# Seuil de couverture (cliquet) : à lancer après `make coverage`.
coverage-check:
	@sh tools/coverage_check.sh $(BUILD) tests/coverage_floor.txt

coverage-report:
	@echo "═══════════════════════════════════════════════════════"
	@echo "  Code Coverage Report — Phosphoric"
	@echo "═══════════════════════════════════════════════════════"
	@echo ""
	@total_lines=0; covered_lines=0; \
	echo "File                                      Lines   Covered   Coverage"; \
	echo "────────────────────────────────────────────────────────────────────"; \
	for gcno in $$(find $(BUILD)/src -name '*.gcno' 2>/dev/null); do \
		src=$$(echo $$gcno | sed 's|^$(BUILD)/||; s/\.gcno$$/.c/'); \
		if [ -f "$$src" ]; then \
			gcov -n -o "$$(dirname $$gcno)" "$$src" 2>/dev/null | grep -A1 "^File '$$src'" | tail -1 | \
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
	@for gcno in $$(find $(BUILD)/src -name '*.gcno' 2>/dev/null); do \
		gcov -n -o "$$(dirname $$gcno)" "$$(echo $$gcno | sed 's|^$(BUILD)/||; s/\.gcno$$/.c/')" 2>/dev/null; \
	done | grep -E "^Lines executed:" | \
		awk -F'[:%]' 'BEGIN{tl=0;te=0;n=0} {split($$3,a," of "); te+=$$2*a[2]/100; tl+=a[2]; n++} \
		END{if(tl>0) printf "TOTAL: %.1f%% (%d/%d lines in %d files)\n", te/tl*100, te, tl, n; \
		else print "No coverage data found"}'
	@echo ""
	@echo "═══════════════════════════════════════════════════════"

coverage-clean:
	@find build . -maxdepth 1 -name '*.gcov' -delete 2>/dev/null; find build -name '*.gcda' -delete 2>/dev/null; true
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
	rm -rf build
	rm -f $(TARGET) $(TARGET)-release $(TARGET)-dist $(TOOLS)
	rm -f web/phosphoric.html web/phosphoric.js web/phosphoric.wasm web/phosphoric.data
	@# Restes de l'ancien build dans l'arbre des sources (avant 2.2.0)
	@find src tests tools -name '*.o' -delete -o -name '*.d' -delete -o -name '*.gcno' -delete -o -name '*.gcda' -delete 2>/dev/null; true
	@find . -maxdepth 1 -type f \( -name 'test_*' -perm -u+x -o -name '*.d' -o -name '*.gcov' \) -delete 2>/dev/null; true

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
               -sEXPORTED_RUNTIME_METHODS=ccall,FS,ENV,callMain \
               -sEXPORTED_FUNCTIONS=_main,_web_key,_web_key_release_all,_web_io_activity,_web_peek,_web_save_state,_web_load_state,_web_insert_tap,_web_insert_disk,_web_iomenu_toggle,_malloc,_free \
               -lidbfs.js --preload-file roms@/roms --shell-file web/shell.html

# La co-simulation RP2040 (libemul, natif) n'existe pas en WebAssembly : le stub
# prend toujours la place de loci_emu.c ici, quel que soit LOCI_EMU.
WASM_SOURCES = $(subst src/io/loci_emu.c,src/io/loci_emu_stub.c,$(LIB_SOURCES))
wasm: web/shell.html
	$(EMCC) $(WASM_CFLAGS) -DNO_LOCI_EMU $(WASM_SOURCES) src/main.c $(WASM_LDFLAGS) -o $(WASM_OUT)
	@echo "WASM ready → serve web/ over HTTP and open phosphoric.html"
	@echo "  e.g.  (cd web && python3 -m http.server 8000)  then  http://localhost:8000/phosphoric.html"

help:
	@echo "Phosphoric — ORIC-1 Emulator Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build emulator (default)"
	@echo "  dist         - Build the single all-in-one distribution binary"
	@echo "  release      - Strip current build into a distribution copy"
	@echo "  tools        - Build conversion tools"
	@echo "  tests        - Build and run all tests"
	@echo "  tests-strict - Run all tests, fail on skips not allowed by tests/allowed_skips.txt"
	@echo "  valgrind-core- Run the core suites under Valgrind (CI)"
	@echo "  (objects live in build/<config>/, one directory per option set)"
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
