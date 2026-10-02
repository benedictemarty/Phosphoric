/* SPDX-License-Identifier: EUPL-1.2 */
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
#include <stdlib.h>
#include <string.h>

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
#include "io/jasmin.h"
#include "io/sp0256.h"
#include "io/mea8000.h"
#include "io/acia6551.h"
#include "io/serial_backend.h"
#include "cards.h"
#include "io/dtl2000.h"
#include "io/mageco.h"
#include "storage/sedoric.h"
#include "hostfs/hostfs.h"
#include "debugger.h"
#include "utils/trace.h"
#include "utils/profiler.h"
#include "utils/symbols.h"
#include "io/loci.h"
#include "video/iomenu.h"
#include "io/ula_ng.h"
#include "network/cast_server.h"

#define EMU_VERSION "2.12.6"

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

/* Max number of --type-keys entries that can be sequenced on one command line */
#define TYPE_KEYS_SEQ_MAX    16

/* Max number of deferred memory writes (--poke-at / --poke-when) */
#define POKE_MAX             32

/* Cycle-triggered, REPEATABLE captures (--screenshot-at / -text-at /
 * -ansi-at / --dump-ram-at): each entry fires once when total_executed
 * reaches its threshold. Array pattern consistent with --poke-at / --type-keys. */
#define TIMED_CAPTURE_MAX    64
typedef enum {
    TCAP_IMAGE = 0,   /* --screenshot-at      : PPM/BMP image */
    TCAP_TEXT,        /* --screenshot-text-at : $BB80 screen text */
    TCAP_ANSI,        /* --screenshot-ansi-at : ANSI image of the framebuffer */
    TCAP_DUMP_RAM     /* --dump-ram-at        : 64K RAM (RAM + CPU view $C000+) */
} timed_capture_type_t;
typedef struct {
    int64_t cycles;               /* trigger threshold (-1 = unused) */
    const char* file;             /* output path */
    timed_capture_type_t type;
    bool done;                    /* already fired */
} timed_capture_t;

typedef struct emulator_s {
    /* Machine model */
    oric_model_t model;
    const rom_patches_t* rom_patches;

    cpu6502_t cpu;
    memory_t memory;
    via6522_t via;
    ay3891x_t psg;
    video_t video;
    osd_t osd;                /* OSD overlay: hot media swap (F6) */
    ula_ng_t ula_ng;         /* ULA-NG: registers $0340-$035F (lock/extensions) */
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
    const char* disk_paths[MICRODISC_MAX_DRIVES]; /* .dsk file per drive (write-back/eject) */
    bool disk_path_owned[MICRODISC_MAX_DRIVES];  /* copy to free (emu_set_disk_path) */
    bool disk_writeback;     /* --disk-writeback: write modified .dsk files back */
    bool has_microdisc;

    /* Jasmin disk interface (WD177x at $03F4-$03FF, boot ROM $F800-$FFFF).
     * Alternative to the Microdisc; mutually exclusive at boot. */
    jasmin_t jasmin;
    bool has_jasmin;

    /* SP0256 Mageco "Synthétiseur Vocal" — GI SP0256-AL2 speech chip at $03F1.
     * Output mixed into the PSG audio; mutually exclusive with a $03F1 conflict. */
    sp0256_t sp0256;
    bool has_sp0256;

    /* MEA8000 TMPI "Synthétiseur Vocal" — Philips formant speech at $03F0/$03F1.
     * No speech ROM (host streams frames); mixed into the PSG audio. Mutually
     * exclusive with the SP0256 ($03F1 overlap). */
    mea8000_t mea8000;
    bool has_mea8000;

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

    /* Tape-OUT capture (CSAVE path A): samples PB7 (Timer-1 driven) and
     * reconstructs the .TAP the ROM emits. Armed by --tape-out-capture. */
    tape_capture_t tape_capture;
    const char*    tape_out_path;   /* Destination .TAP for the capture, or NULL */

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
    bool realtime;          /* --realtime: paces at 50 Hz PAL even in headless
                             * (nanosleep pacing, independent of SDL) for
                             * network I/O (modem/XMODEM) and keyboard sequencing */
    int64_t max_cycles;

    /* Sprint 34d4 (P2-G audit) — current cycle position within the PAL frame.
     * Maintained by the master clock (emu_cycle, src/emu_clock.c) so the
     * debugger can derive the raster line via `frame_cycles / PAL_CYCLES_PER_LINE`. */
    int frame_cycles;

    /* ─── Master clock (V2-E2, src/emu_clock.c) ───
     * Scan position within the PAL frame and rendering progress. These
     * counters used to be local variables of the main loop: moving them
     * into the emulator is what lets `emu_cycle()` advance the WHOLE
     * machine by one cycle, from any caller
     * (main loop, debugger, tests, replay). */
    int raster_cycle;     /**< current cycle within the frame (0 … CYCLES_PER_FRAME-1) */
    int raster_rendered;  /**< visible scanlines already rendered (0 … 224) */
    int raster_ng_line;   /**< ULA-NG line already processed (0 … 311) */
    int raster_next_line; /**< cycle of the next line crossing (fast
                           *   exit: 63 cycles out of 64 have nothing to emit) */
    bool clock_resume_pending; /**< V2-E7: a savestate has just restored the
                                *   scan position; the loop must resume
                                *   the frame at that point (emu_clock_resume) */

    /* ─── Per-cycle ULA (V2-E4) ───
     * When `ula_per_cycle` is true, the scan no longer renders a line in one
     * go at the end of the line: it fetches **one 6-pixel cell per cycle**, at
     * the moment the real ULA reads it. A CPU write in the middle of a line
     * then only affects the cells not yet scanned (raster splits).
     *
     * `ula_fetch_offset` is the cycle of the line at which column 0 is read.
     * ⚠️ This value is NOT calibrated against real hardware: only the
     * structure (one cell per cycle) is. Adjustable with --ula-fetch-offset
     * for calibration (same approach as the epic B constants). */
    bool ula_per_cycle;
    int  ula_fetch_offset;

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

    /* Set when a save state is restored at startup (--load-state), so
     * emulator_run() skips its power-on cpu_reset — which would otherwise
     * clobber the loaded PC/cycles and drop back to the reset vector. */
    bool startup_state_loaded;

    /* Screenshot options ("end of run" outputs) */
    const char* screenshot_file;
    const char* screenshot_text_file; /* dump of the $BB80 screen text content (output) */
    const char* screenshot_ansi_file; /* true-color ANSI image of the framebuffer (output) */

    /* Cycle-triggered, repeatable captures (see timed_capture_t):
     * --screenshot-at / -text-at / -ansi-at / --dump-ram-at. */
    timed_capture_t timed_captures[TIMED_CAPTURE_MAX];
    int             timed_capture_count;

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


    /* Captures triggered by a memory STATE (rising edge: 1st time that
     * RAM[addr] == val, sampled at end of frame like the -at variants).
     * addr = -1 → disarmed. Safety net: if armed but never fired before the end
     * of the run (--cycles), when_condition_unmet is set → exit 2. */
    int32_t screenshot_when_addr;      /* -1 = off */
    uint8_t screenshot_when_val;
    const char* screenshot_when_file;
    bool screenshot_when_done;
    int32_t dump_ram_when_addr;        /* -1 = off */
    uint8_t dump_ram_when_val;
    const char* dump_ram_when_file;
    bool dump_ram_when_done;
    int32_t screenshot_text_when_addr; /* -1 = off */
    uint8_t screenshot_text_when_val;
    const char* screenshot_text_when_file;
    bool screenshot_text_when_done;
    bool when_condition_unmet;         /* set if an armed -when never fired */

    /* Triggered memory writes (poke): actuator symmetric to the -when/-at
     * captures. Each entry writes RAM[target]=value exactly once, either at a
     * cycle threshold (at_cycles >= 0), or on the rising edge RAM[when_addr]==
     * when_val (when_addr >= 0). Sampled at end of frame like the -when ones.
     * Used to drive an application state deterministically (e.g. position
     * a cursor + request a click) without going through the keyboard. */
    struct poke_action {
        int64_t  at_cycles;   /* >= 0: fires when total_executed >= at_cycles    */
        int32_t  when_addr;   /* >= 0: fires when RAM[when_addr] == when_val      */
        uint8_t  when_val;
        uint16_t target;      /* address written                                  */
        uint8_t  value;       /* byte written                                     */
        bool     done;        /* already fired (only once)                        */
    } pokes[POKE_MAX];
    int poke_count;

    /* IRQ trace: log each IRQ entry + RTI to FILE */
    FILE* irq_trace_fp;
    bool irq_trace_active;
    int32_t irq_trace_depth;  /* Track IRQ nesting (incremented on IRQ, decremented on RTI) */

    /* Audio capture (headless-friendly, mirrors --screenshot-at for sound) :
     * --psg-trace  logs each AY sound-register write (reg 0..13) with its CPU cycle ;
     * --audio-wav  renders the PSG to PCM once per frame and writes a 16-bit stereo
     *              44.1 kHz WAV (uses ay_generate, the same engine as SDL playback). */
    FILE* psg_trace_fp;
    FILE* kbd_trace_fp;              /* --kbd-scan-trace : one line per VIA Port B read
                                      * (col, reg7, reg14, matrix[col], rendered PB3) */
    FILE* audio_wav_fp;
    uint32_t audio_wav_data_bytes;   /* PCM payload written so far (for the header patch) */

    /* Auto-type: inject keystrokes at specified cycle count */
    const char* type_keys_text;
    int64_t type_keys_at;
    int type_keys_idx;
    int64_t type_keys_next_cycle;
    bool type_keys_done;
    char type_keys_last_char;       /* Last typed char (debounce repeated keys) */
    int type_keys_debounce;         /* Debounce frames remaining (0 = ready) */
    /* Sprint 34av: if true, chars are injected via the LOCI HID
     * (loci_kbd_set_report) instead of the ORIC matrix. Enabled by the
     * "loci-hid:" prefix in the TEXT of --type-keys. Used to automate
     * LOCI TUI navigation without a real SDL keyboard event. */
    bool type_keys_loci_hid;
    /* Queue of --type-keys sequences: allows passing several
     * --time-keys CYCLES:TEXT on the same command line. Each entry
     * is activated (loaded into the active type_keys_* fields above) as soon
     * as its arming cycle is reached AND the previous entry has
     * finished. Gives clean cycle-based sequencing for automated
     * multi-screen walkthroughs (cf. wait_release of TUIs/terminals). */
    struct {
        int64_t at;          /* absolute arming cycle */
        const char* text;    /* text (without the loci-hid: prefix) */
        bool loci_hid;       /* LOCI HID routing rather than ORIC matrix */
    } type_keys_seq[TYPE_KEYS_SEQ_MAX];
    int type_keys_seq_count; /* number of valid entries */
    int type_keys_seq_idx;   /* next entry to activate */

    /* Scan-driven pacing (cf. include/io/autotype.h). kbd_scan_passes counts
     * completed keyboard-matrix scan passes (VIA Port B sweep), updated in
     * portb_read_callback. The auto-typer refuses to change the matrix until
     * the scanner has completed >= AUTOTYPE_MIN_PASS passes since the last
     * transition (type_keys_last_pass), so a program is guaranteed to have
     * observed the current key state before it changes — no dropped keys on
     * programs that scan slower than one frame. Additive to the existing
     * cycle schedule (never faster than before); a cycle watchdog prevents a
     * stall when the target never scans the keyboard. */
    uint32_t kbd_scan_passes;
    uint8_t  kbd_scan_prev_col;   /* last column read (0xFF = none yet) */
    uint32_t type_keys_last_pass; /* kbd_scan_passes at last transition */

    /* --type-keys-when A:V:TEXT : arm the auto-typer when RAM[A]==V instead of
     * at a guessed cycle count (reuses the --screenshot-when trigger idea).
     * addr = -1 disables. Fires once. */
    int32_t     type_keys_when_addr;
    uint8_t     type_keys_when_val;
    const char* type_keys_when_text;
    bool        type_keys_when_loci_hid;
    bool        type_keys_when_done;

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

    /* Disable the overscan border in the window (--no-border). The border is
     * composited on by default (generic overscan infra). */
    bool no_border;

    /* Include the overscan border in image/AVI exports (--export-border).
     * Off by default so exports keep the active-area dimensions. */
    bool export_border;

    /* Interactive debugger */
    debugger_t debugger;

    /* Symbol table for debugger (loaded via --symbols) */
    symbol_table_t symbols;

    /* LOCI peripheral (Lovely Oric Computer Interface, sodiumlb 2024).
     * Only active when --loci is passed; reserves MIA bus at $03A0-$03BF. */
    loci_t loci;
    bool   has_loci;
    /* Expansion cards (F1 menu): original launch options, choices being
     * edited, restart options (NULL: no restart requested). */
    int    argc;
    char** argv;
    cards_state_t cards;
    char** restart_argv;
    bool   loci_external;   /* --loci-emu / --loci-hw: the firmware (co-simulated or
                               real) handles its floppies, not the internal model */
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
    bool tape_path_owned;                        /* copy to free (emu_set_tape_path) */

    /* I/O peripherals menu (F1) and what it shows about the cards (read
     * only: filled in at startup, no effect on emulation). */
    iom_menu_t  iomenu;
    const char* jasmin_rom_path;
    const char* sp0256_rom_path;
    const char* serial_spec;        /* --serial */
    const char* dtl2000_spec;       /* --dtl2000 */
    const char* mageco_spec;        /* --mageco / --oricon */
    const char* config_path;        /* configuration file (NULL: default) */
} emulator_t;

/* ════════════════════════════════════════════════════════════════════
 *  Master clock (V2-E2, src/emu_clock.c)
 *
 *  Single entry point for time: one call = one cycle of the WHOLE machine,
 *  in a fixed intra-cycle order (φ1 ULA → φ2 CPU → φ2 peripherals).
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief Advances the whole machine by one cycle
 * @return true if the executed cycle completed an instruction
 *
 * With the legacy core (`--cpu-legacy`), which cannot stop between two
 * cycles, executes a whole instruction and always returns true.
 */
bool emu_cycle(emulator_t* emu);

/** @brief Executes a complete instruction via emu_cycle(); returns its cycles */
int emu_step(emulator_t* emu);

/** @brief Resets the scan position (start of frame).
 *  If a savestate has just restored a position (clock_resume_pending), the
 *  frame resumes at that position instead of restarting from zero. */
void emu_clock_frame_begin(emulator_t* emu);

/**
 * @brief Resumes the frame at the position restored by a savestate (V2-E7)
 *
 * Consumes `clock_resume_pending`: realigns the next line crossing
 * and rebuilds, from the restored RAM, the lines the beam had already
 * scanned (the framebuffer is not in the .ost). No effect if nothing is
 * pending. Called by the main loop before each instruction.
 */
void emu_clock_resume(emulator_t* emu);

/** @brief Finishes rendering the current frame (remaining lines) */
void emu_clock_frame_end(emulator_t* emu);

/** @brief Current beam position: PAL line (0-311) and cycle within the line (0-63) */
void emu_raster_pos(const emulator_t* emu, int* line, int* dot);

/* ── Active disk interface helpers ───────────────────────────────────────
 * The emulator boots either the Microdisc or the Jasmin (mutually exclusive),
 * and each carries its own per-drive dirty flag. Write-back sites must consult
 * the *active* interface, not hard-code the Microdisc. Both share the flat
 * emu->disks[]/emu->disk_paths[] arrays and the same 4-drive layout. */
static inline int emu_disk_max_drives(const emulator_t* emu) {
    return emu->has_jasmin ? JASMIN_MAX_DRIVES : MICRODISC_MAX_DRIVES;
}

static inline bool emu_disk_dirty(const emulator_t* emu, int drv) {
    if (drv < 0 || drv >= emu_disk_max_drives(emu)) return false;
    return emu->has_jasmin ? emu->jasmin.disk_dirty[drv]
                           : emu->microdisc.disk_dirty[drv];
}

static inline void emu_disk_clear_dirty(emulator_t* emu, int drv) {
    if (drv < 0 || drv >= emu_disk_max_drives(emu)) return;
    if (emu->has_jasmin) emu->jasmin.disk_dirty[drv] = false;
    else                 emu->microdisc.disk_dirty[drv] = false;
}

/* Install (or, with nd==NULL, eject) a disk into drive @p drv on the active
 * controller, so hot-swap paths (OSD/control load-disk/eject-disk) don't
 * hard-code the Microdisc. Both controllers share this exact signature. */
static inline void emu_disk_wire(emulator_t* emu, int drv, sedoric_disk_t* nd) {
    if (drv < 0 || drv >= emu_disk_max_drives(emu)) return;
    uint8_t* data    = nd ? nd->data    : NULL;
    uint32_t size    = nd ? nd->size    : 0;
    uint8_t  tracks  = nd ? nd->tracks  : 0;
    uint8_t  sectors = nd ? nd->sectors : 0;
    if (emu->has_jasmin)
        jasmin_set_disk(&emu->jasmin, (uint8_t)drv, data, size, tracks, sectors);
    else
        microdisc_set_disk(&emu->microdisc, (uint8_t)drv, data, size, tracks, sectors);
}

/* Media paths tracked by the emulator. Those given at startup come from argv
 * (never freed); a hot insertion (F1 menu, --control, web) makes a copy the
 * emulator owns: freed on replacement and on eject (before 2.9.1, every
 * insertion leaked it). NULL = clear. */
static inline char* emu_path_copy_(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
static inline void emu_set_disk_path(emulator_t* emu, int drv, const char* path) {
    if (drv < 0 || drv >= MICRODISC_MAX_DRIVES) return;
    if (emu->disk_path_owned[drv]) free((void*)emu->disk_paths[drv]);
    emu->disk_paths[drv] = path ? emu_path_copy_(path) : NULL;
    emu->disk_path_owned[drv] = emu->disk_paths[drv] != NULL;
    if (drv == 0) emu->disk_path = emu->disk_paths[0];
}
static inline void emu_set_tape_path(emulator_t* emu, const char* path) {
    if (emu->tape_path_owned) free((void*)emu->tape_path);
    emu->tape_path = path ? emu_path_copy_(path) : NULL;
    emu->tape_path_owned = emu->tape_path != NULL;
}

/* Per-drive write protection, on the active interface. */
static inline bool emu_disk_protected(const emulator_t* emu, int drv) {
    if (drv < 0 || drv >= emu_disk_max_drives(emu)) return false;
    return emu->has_jasmin ? emu->jasmin.write_protect[drv] : emu->microdisc.write_protect[drv];
}
static inline void emu_disk_set_protected(emulator_t* emu, int drv, bool on) {
    if (drv < 0 || drv >= emu_disk_max_drives(emu)) return;
    if (emu->has_jasmin) jasmin_set_write_protect(&emu->jasmin, (uint8_t)drv, on);
    else                 microdisc_set_write_protect(&emu->microdisc, (uint8_t)drv, on);
}

static inline bool emu_has_disk_iface(const emulator_t* emu) {
    return emu->has_microdisc || emu->has_jasmin;
}

/* A floppy goes to the card present: Microdisc or Jasmin, otherwise LOCI
 * (internal model) — they never coexist. With a co-simulated or real LOCI
 * (loci_external), its firmware mounts its images: nothing from the host.
 * Rule shared by the F1 menu and --control / HTTP API. */
static inline bool emu_loci_disks(const emulator_t* emu) {
    return emu->has_loci && !emu->loci_external && !emu_has_disk_iface(emu);
}

#endif /* EMULATOR_H */
