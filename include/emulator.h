/**
 * @file emulator.h
 * @brief Phosphoric — ORIC-1 Emulator core structure and API
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-24
 * @version 1.1.0-alpha
 *
 * Shared emulator state structure, accessible by all modules
 * (main loop, debugger, etc.)
 */

#ifndef EMULATOR_H
#define EMULATOR_H

#include <stdint.h>
#include <stdbool.h>

#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "io/via6522.h"
#include "video/video.h"
#include "video/osd.h"
#include "video/avi_recorder.h"
#include "utils/movie.h"
#include "audio/audio.h"
#include "io/keyboard.h"
#include "io/joystick.h"
#include "io/printer.h"
#include "io/cassette.h"
#include "io/microdisc.h"
#include "io/acia6551.h"
#include "io/serial_backend.h"
#include "io/dtl2000.h"
#include "io/mageco.h"
#include "storage/sedoric.h"
#include "hostfs/hostfs.h"
#include "debugger.h"
#include "utils/trace.h"
#include "utils/profiler.h"
#include "utils/symbols.h"
#include "io/loci.h"
#include "io/ocula_gpu.h"
#include "network/cast_server.h"

#define EMU_VERSION "1.59.0-alpha"

/**
 * @brief ORIC machine model
 */
typedef enum {
    ORIC_MODEL_ORIC1  = 0,  /**< ORIC-1 with BASIC 1.0 */
    ORIC_MODEL_ATMOS  = 1   /**< ORIC Atmos with BASIC 1.1 */
} oric_model_t;

/**
 * @brief ROM-version-specific tape patch addresses
 *
 * Addresses used to intercept ROM cassette loading routines for
 * fast tape loading (CLOAD patching). Different ROM versions
 * have different routine addresses.
 */
typedef struct rom_patches_s {
    const char* name;           /**< ROM version name (e.g. "BASIC 1.0") */
    uint16_t getsync_entry;     /**< getsync() entry point */
    uint16_t getsync_end;       /**< getsync() RTS address */
    uint16_t getsync_loop;      /**< getsync() recovery loop address */
    uint16_t readbyte_entry;    /**< readbyte() entry point */
    uint16_t readbyte_end;      /**< readbyte() RTS address */
    uint16_t readbyte_store;    /**< readbyte() byte store address in RAM */
    uint16_t readbyte_storezero;/**< extra RAM byte zeroed by GetTapeByte (0 = none).
                                  *  Atmos: $02B1 (tape parity accumulator). */
    bool     readbyte_setcarry; /**< true if real GetTapeByte returns with C=1.
                                  *  Atmos: true ; ORIC-1: false. */
    uint16_t csave_header_buf;  /**< Base of 9-byte header staging buffer the
                                  *  ROM populates before WriteFileHeader.
                                  *  CSAVE-variant-agnostic source of truth.
                                  *  Atmos: $02A8. ORIC-1: $005E (zero page).
                                  *  Read at writefileheader_entry trap, NOT at
                                  *  csave_end (the data-write loop mutates
                                  *  $5F/$60 on ORIC-1 — senior 34at). */
    uint16_t csave_filename_buf;/**< Base of filename buffer the ROM uses
                                  *  during CSAVE. Atmos: $027F.
                                  *  ORIC-1: $0035. */
    uint16_t writefileheader_entry; /**< Entry of WriteFileHeader. Trap fires
                                      *  here to snapshot the header/filename
                                      *  buffers BEFORE any in-flight mutation
                                      *  (Sprint 34at). ORIC-1: $E57B. Atmos:
                                      *  $E607. 0 = no snapshot, fall back to
                                      *  live RAM at csave_end. */
    uint16_t cload_data_rts;    /**< CLOAD data loop RTS (triggers post-load rechain) */
    uint16_t putbyte_entry;     /**< putbyte() entry point (CSAVE) */
    uint16_t putbyte_end;       /**< putbyte() RTS address */
    uint16_t csave_end;         /**< CSAVE complete RTS address */
    uint16_t writeleader_entry; /**< writeleader() entry point */
    uint16_t writeleader_end;   /**< writeleader() RTS address */
    uint16_t tape_type_addr;    /**< RAM address where the ROM stores the tape
                                  *  header file-type byte ($00=BASIC,
                                  *  $80=machine code) after CLOAD header read.
                                  *  Header is stored reversed (STA base,X /
                                  *  DEX): ORIC-1 $0064, Atmos $02AE. Used to
                                  *  gate the post-CLOAD BASIC rechain. */
} rom_patches_t;
#define ORIC_CLOCK_HZ   1000000
#define ORIC_FRAME_RATE  50

/* PAL timing constants (real hardware values) */
#define PAL_LINES_PER_FRAME  312
#define PAL_CYCLES_PER_LINE  64
#define CYCLES_PER_FRAME     (PAL_LINES_PER_FRAME * PAL_CYCLES_PER_LINE)  /* 19968 */
#define VSYNC_START_LINE     256
#define VSYNC_CYCLE          (VSYNC_START_LINE * PAL_CYCLES_PER_LINE)     /* 16384 */

/* Nombre max d'entrées --type-keys séquençables sur une ligne de commande */
#define TYPE_KEYS_SEQ_MAX    16

typedef struct emulator_s {
    /* Machine model */
    oric_model_t model;
    const rom_patches_t* rom_patches;

    cpu6502_t cpu;
    memory_t memory;
    via6522_t via;
    ay3891x_t psg;
    video_t video;
    osd_t osd;                /* OSD overlay : changement de média à chaud (F6) */
    ocula_gpu_t ocula_gpu;   /* OCULA-GPU window $03E8-$03EF (étape 5) */
    hostfs_t hostfs;

    /* Keyboard */
    oric_keyboard_t keyboard;

    /* Joystick (IJK interface) */
    oric_joystick_t joystick;

    /* Centronics parallel printer */
    oric_printer_t printer;

    /* ACIA 6551 serial interface (Digitelec DTL 2000, MCP RS232-C, etc.) */
    acia6551_t acia;
    uint16_t acia_base_addr;
    serial_backend_t* serial_backend;
    bool has_serial;

    /* Digitelec DTL 2000 — faithful PIA 6821 + ACIA 6850 at $03F8-$03FD */
    dtl2000_t dtl2000;
    serial_backend_t* dtl2000_backend;
    bool has_dtl2000;

    /* Mageco MIDI interface — MC6850 ACIA at $03FE-$03FF (31250 baud MIDI) */
    mageco_t mageco;
    serial_backend_t* mageco_backend;
    bool has_mageco;

    /* Microdisc controller */
    microdisc_t microdisc;
    sedoric_disk_t* disks[MICRODISC_MAX_DRIVES]; /* 4 drives: A, B, C, D */
    const char* disk_paths[MICRODISC_MAX_DRIVES]; /* fichier .dsk par lecteur (write-back/éjection) */
    bool disk_writeback;     /* --disk-writeback : réécrire les .dsk modifiés */
    bool has_microdisc;

    /* Tape buffer for ROM patching (CLOAD support) */
    uint8_t* tapebuf;       /* TAP file data loaded in memory */
    int tapelen;             /* Total length of tape data */
    int tapeoffs;            /* Current read offset */
    bool tape_loaded;        /* A tape is loaded and available */
    int tape_syncstack;     /* Saved SP for sync loop recovery (-1 = none) */

    /* Signal-level cassette (Sprint 90): generates the tape waveform on VIA
     * CB1 so the real ROM read routine / custom loaders sample a genuine
     * signal. Enabled via --tape-signal; disables the getsync/readbyte patches. */
    cassette_t cassette;

    /* Deferred fast-load (inject after RAM test completes) */
    uint8_t* fastload_buf;       /* Buffered TAP data */
    uint16_t fastload_addr;      /* Target start address */
    uint16_t fastload_end;       /* Target end address (from TAP header) */
    uint16_t fastload_size;      /* Data size in bytes */
    uint8_t  fastload_type;      /* TAP type: 0x00=BASIC, 0x80=MC */
    uint8_t  fastload_auto_run;  /* TAP auto-run flag: $00=none, $80=BASIC RUN, $C7=MC JMP */
    bool     fastload_pending;   /* RAM injection pending (fires at ~3M cycles) */
    bool     fastload_autoexec_pending; /* Auto-exec/RUN pending (fires at ~5M, after VIA stable) */
    bool     tape_auto_cload_pending; /* -t without -f: auto-type CLOAD"" once BASIC ready */

    /* Post-CLOAD BASIC rechain (line pointers in TAP may be stale) */
    bool     tape_readbyte_active;  /* Set when readbyte patch fires (CLOAD in progress) */

    /* CSAVE support: capture saved data to .TAP file */
    FILE*    csave_file;            /* Open TAP file for CSAVE output */
    int      csave_byte_count;     /* Bytes written in current CSAVE */
    char     csave_last_path[64];   /* Path of last CSAVE for re-buffering */
    /* Sprint 34at : header + filename snapshot taken at writefileheader_entry.
     * On ORIC-1 the data-write loop reuses $5F/$60 → reading them at csave_end
     * gives the END address instead of START. Snapshot avoids this race. */
    uint8_t  csave_header_snap[9];
    char     csave_fname_snap[17];
    bool     csave_snap_valid;
    /* Sprint 34at : ORIC-1 csave_end ($E80A) is on a code path shared by CLOAD,
     * so the trap can fire twice per CSAVE+CLOAD turn. csave_in_progress is
     * set at writeleader_entry, cleared at the first csave_end, so the second
     * one becomes a no-op rather than rebuilding from stale state. */
    bool     csave_in_progress;

    bool running;
    bool fast_load;
    bool headless;
    bool realtime;          /* --realtime : cadence à 50 Hz PAL même en headless
                             * (pacing nanosleep, indépendant de SDL) pour les
                             * E/S réseau (modem/XMODEM) et le séquençage clavier */
    int64_t max_cycles;

    /* Sprint 34d4 (P2-G audit) — current cycle position within the PAL frame.
     * Updated by the main emulation loop after each cpu_step so the debugger
     * can derive the raster line via `frame_cycles / PAL_CYCLES_PER_LINE`. */
    int frame_cycles;

    /* Sprint 35a — IPC control mode for OricForge IDE integration. When set,
     * stdin/stdout speak a line-based protocol (CMD/REP/EVT). Logs are
     * routed to stderr at startup so stdout stays clean. */
    bool control_mode;
    /* Sprint 35a freeze — set by control_poll_pause when async `pause`
     * acknowledged. The next REPL re-entry will emit `EVT stopped
     * reason=user` instead of the default `reason=break` and reset this
     * flag. Lets the IDE see exactly one event per pause cycle. */
    bool control_async_pause_pending;

    /* Sprint 36a — `--bench` flag : at exit, emulator_run prints a
     * single-line throughput report (cycles, wall_ms, MHz_equivalent,
     * speed_ratio vs real ORIC) to stdout. Implies --headless so the
     * SDL2 frame limiter doesn't cap the measurement. */
    bool bench_mode;

    /* Screenshot options */
    const char* screenshot_file;
    int64_t screenshot_at_cycles;
    const char* screenshot_at_file;

    /* Frame dump options */
    const char* frame_dump_dir;
    int frame_dump_interval;

    /* Deterministic input record/replay (TAS movie) */
    movie_t movie;

    /* Video recording (Motion-JPEG AVI) */
    const char* video_avi_file;   /* output .avi path, NULL = disabled */
    int video_avi_fps;            /* recording frame rate (default 50) */
    int video_avi_quality;        /* JPEG quality 1..100 (default 85) */
    avi_recorder_t video_avi_rec; /* recorder state */
    bool video_avi_active;        /* true once the file is open */

    /* RAM dump at cycle: write 64KB of RAM to FILE when cycle >= threshold */
    int64_t dump_ram_at_cycles;
    const char* dump_ram_at_file;
    bool dump_ram_at_done;

    /* IRQ trace: log each IRQ entry + RTI to FILE */
    FILE* irq_trace_fp;
    bool irq_trace_active;
    int32_t irq_trace_depth;  /* Track IRQ nesting (incremented on IRQ, decremented on RTI) */

    /* Auto-type: inject keystrokes at specified cycle count */
    const char* type_keys_text;
    int64_t type_keys_at;
    int type_keys_idx;
    int64_t type_keys_next_cycle;
    bool type_keys_done;
    char type_keys_last_char;       /* Last typed char (debounce repeated keys) */
    int type_keys_debounce;         /* Debounce frames remaining (0 = ready) */
    /* Sprint 34av : si true, les chars sont injectés via le HID LOCI
     * (loci_kbd_set_report) au lieu de la matrice ORIC. Activé par le
     * préfixe "loci-hid:" dans le TEXT de --type-keys. Pour automatiser
     * la navigation TUI LOCI sans une vraie SDL keyboard event. */
    bool type_keys_loci_hid;
    /* File de séquences --type-keys : permet de passer plusieurs
     * --time-keys CYCLES:TEXT sur la même ligne de commande. Chaque entrée
     * est activée (chargée dans les champs type_keys_* actifs ci-dessus) dès
     * que son cycle d'armement est atteint ET que l'entrée précédente est
     * terminée. Donne un séquençage par cycles propre pour les parcours
     * multi-écrans automatisés (cf. wait_release des TUI/terminaux). */
    struct {
        int64_t at;          /* cycle d'armement absolu */
        const char* text;    /* texte (sans le préfixe loci-hid:) */
        bool loci_hid;       /* routage HID LOCI plutôt que matrice ORIC */
    } type_keys_seq[TYPE_KEYS_SEQ_MAX];
    int type_keys_seq_count; /* nombre d'entrées valides */
    int type_keys_seq_idx;   /* prochaine entrée à activer */

    /* Dynamic keyboard injection (sprint 95, API REST Epic 4). A growable
     * byte buffer appended to by the `keys` control command and consumed one
     * key per few frames by the main loop (press/hold/release). Both producer
     * (control_queue drain) and consumer (main loop) run on the emulator
     * thread, so no lock is needed. Distinct from the CLI --type-keys path. */
    char*  kbd_inject_buf;
    size_t kbd_inject_len;
    size_t kbd_inject_cap;
    size_t kbd_inject_pos;   /* next byte to press */
    int    kbd_inject_delay; /* frames to wait before the next phase */
    bool   kbd_inject_pressed; /* true while the current key is held down */

    /* Breakpoint (legacy single breakpoint, -1 = none) */
    int32_t breakpoint;

    /* Display scaling (1-4, default 3) */
    int scale_factor;

    /* Force the SDL software renderer (--render-software). Works around setups
     * where the accelerated renderer presents an all-black window. */
    bool render_software;

    /* Disable the OCULA overscan border in the window (--no-border). The
     * border is composited on by default (Sprint 65). */
    bool no_border;

    /* Include the OCULA overscan border in image/AVI exports (--export-border).
     * Off by default so exports keep the active-area dimensions (Sprint 77). */
    bool export_border;

    /* Interactive debugger */
    debugger_t debugger;

    /* Symbol table for debugger (loaded via --symbols) */
    symbol_table_t symbols;

    /* LOCI peripheral (Lovely Oric Computer Interface, sodiumlb 2024).
     * Only active when --loci is passed; reserves MIA bus at $03A0-$03BF. */
    loci_t loci;
    bool   has_loci;
    /* Sprint 34c hardening — owns the overlay ROM buffer that LOCI's
     * rom_swap callback installs into memory.overlay_rom (was a static
     * inside main.c with a comment acknowledging "acceptable leak at
     * shutdown"). Freed by emulator_cleanup. */
    uint8_t* loci_overlay_buf;
    /* True while the LOCI menu ROM (warm boot via the Action button) is
     * mapped. Guards the button against re-entry: pressing it inside the
     * menu must NOT re-snapshot (it would clobber the session snapshot
     * with the menu's own state) nor re-boot the menu. Cleared when the
     * menu leaves (resume or MIA_BOOT into another ROM). */
    bool loci_menu_active;
    /* Set when the Action button was held ≥ 2 s (firmware
     * EXT_BTN_LONGPRESS_MS): the release boots the diagnostic ROM
     * (test108k) instead of the menu. */
    bool loci_button_long;

    /* TUI mode flag: when true, breakpoints route to the ncurses TUI
     * instead of the line-based REPL (build with TUI=1). */
    bool tui_mode;

    /* GDB remote stub: when true, CPU stops route to the GDB RSP server
     * (--gdb [PORT]) instead of the interactive REPL. gdb_stub points to a
     * gdb_stub_t owned by main() (void* keeps emulator.h decoupled). */
    bool gdb_mode;
    void* gdb_stub;

    /* CPU trace logging */
    cpu_trace_t trace;

    /* CPU performance profiler */
    cpu_profiler_t profiler;

    /* Cast server (MJPEG streaming) */
    cast_server_t cast_server;
    bool has_cast_server;

    /* CASTV2 client (native Chromecast control) */
    castv2_client_t castv2_client;
    bool has_castv2;

    /* HTTP control API (sprint 94, API REST Epic 3). Opaque pointers keep the
     * pthread/socket details out of this header; both are NULL unless
     * --http-api is given. The queue is the frame-boundary hand-off drained
     * once per frame by the main loop. */
    struct control_queue_s* control_queue;   /* producer→emulator commands   */
    struct http_api_server_s* http_api;       /* HTTP server (own thread)     */
    bool  has_http_api;
    const char* http_api_bind;                /* bind address (default local) */
    const char* http_api_root;                /* file-op sandbox root         */

    /* Loaded file paths (for save state metadata) */
    const char* rom_path;
    const char* disk_path;
    const char* diskrom_path;
    const char* tape_path;
} emulator_t;

#endif /* EMULATOR_H */
