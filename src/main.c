/**
 * @file main.c
 * @brief Phosphoric — ORIC-1 Emulator main entry point - full emulation loop
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-beta.2
 */

/* clock_gettime/CLOCK_MONOTONIC (bench timer) under strict -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "io/via6522.h"
#include "video/video.h"
#include "video/export.h"
#include "storage/tap.h"
#include "storage/disk.h"
#include "storage/sedoric.h"
#include "io/microdisc.h"
#include "io/loci_sdimg.h"
#include "audio/audio.h"
#include "io/keyboard.h"
#include "io/printer.h"
#include "debugger.h"
#include "tui.h"
#include "control.h"
#include "savestate.h"
#include "utils/trace.h"
#include "utils/rominfo.h"
#ifdef HAS_SDL2
#include <SDL2/SDL.h>
#endif
#include "hostfs/hostfs.h"
#include "utils/logging.h"
#ifdef HAS_CAST
#include <arpa/inet.h>
#endif

/* Forward declarations for renderer (in renderer.c) */
bool renderer_init(int scale);
void renderer_cleanup(void);
void renderer_present(video_t* vid);
void renderer_toggle_fullscreen(void);
void renderer_set_scale(int scale);
int renderer_get_scale(void);
void renderer_cycle_scale(void);

static volatile bool g_running = true;

static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

/**
 * @brief Strip padding bytes from TAP block headers in tape buffer.
 *
 * Some TAP files (from real tape captures) have extra $00 padding bytes
 * between the $24 marker and the actual header fields. The ORIC ROM
 * reads bytes sequentially via readbyte, so these extra bytes cause
 * the header to be mis-parsed (wrong start/end addresses).
 *
 * This function scans the buffer for each block header (sync + $24),
 * detects padding by checking that the null separator byte (7th byte
 * after header fields) is $00, and removes extra bytes in-place.
 *
 * @param buf    Tape buffer (modified in place)
 * @param len    Buffer length (updated with new length)
 * @param offset Starting offset (updated if it falls after removed bytes)
 */
static void tap_strip_header_padding(uint8_t* buf, int* len, int* offset) {
    int src = 0, dst = 0;
    int orig_offset = *offset;
    int new_offset = orig_offset;
    int removed_before_offset = 0;

    while (src < *len) {
        /* Look for sync pattern: 3+ consecutive $16 followed by $24 */
        if (buf[src] == 0x16) {
            /* Copy sync bytes and count them */
            int sync_start = src;
            int sync_count = 0;
            while (src < *len && buf[src] == 0x16) {
                buf[dst++] = buf[src++];
                sync_count++;
            }
            if (src >= *len) break;

            if (buf[src] == 0x24 && sync_count >= 3) {
                /* Valid sync pattern (3+ $16 bytes) — copy $24 marker */
                buf[dst++] = buf[src++];

                /* Now check for padding: try skip 0..4, find where
                 * the null separator byte (offset +6 from type) is $00 */
                int best_skip = 0;
                for (int skip = 0; skip <= 4 && src + skip + 7 <= *len; skip++) {
                    uint8_t type_byte = buf[src + skip];
                    uint8_t null_byte = buf[src + skip + 6];
                    bool valid_type = (type_byte == 0x00 || type_byte == 0x80 ||
                                       type_byte == 0xC0);
                    if (null_byte == 0x00 && valid_type) {
                        best_skip = skip;
                        break;
                    }
                }

                if (best_skip > 0) {
                    log_info("TAP: stripped %d padding byte(s) at offset %d",
                             best_skip, src);
                    /* Track bytes removed before the CLOAD offset */
                    if (src <= orig_offset) {
                        removed_before_offset += best_skip;
                    }
                    src += best_skip; /* Skip padding bytes */
                }
            }
        } else {
            buf[dst++] = buf[src++];
        }
    }

    if (dst < *len) {
        new_offset = orig_offset - removed_before_offset;
        if (new_offset < 0) new_offset = 0;
        *offset = new_offset;
        *len = dst;
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  ROM patch tables (version-specific tape loading addresses)         */
/* ═══════════════════════════════════════════════════════════════════ */

static const rom_patches_t rom_patches_basic10 = {
    .name              = "BASIC 1.0 (ORIC-1)",
    .getsync_entry     = 0xE696,
    .getsync_end       = 0xE6B9,
    .getsync_loop      = 0xE681,
    .readbyte_entry    = 0xE630,
    .readbyte_end      = 0xE65B,
    .readbyte_store    = 0x002F,
    .readbyte_storezero= 0,         /* GetTapeByte on Oric-1 does not maintain $02B1 */
    .readbyte_setcarry = false,     /* and exits with C=0 */
    .csave_header_buf  = 0x005E,    /* Sprint 34at — ZP staging buffer $5E..$66
                                      * (read via LDX#9 / LDA $5D,X / DEX, see
                                      * disasm at $E585). Senior-approved. */
    .csave_filename_buf= 0x0035,    /* filename at $0035 (16 bytes, null-term) */
    .writefileheader_entry = 0xE57B,/* Snapshot trap point — captures $5E..$66
                                      * + $0035 BEFORE the data-write loop
                                      * mutates $5F/$60 as a work pointer. */
    .cload_data_rts    = 0xE502,
    .putbyte_entry     = 0xE5C6,
    .putbyte_end       = 0xE5F2,
    .csave_end         = 0xE80A,    /* Sprint 34at (senior-approved Option A):
                                      * $E80A is the JMP $EBD0 that terminates
                                      * the CSAVE outer routine. $E7FE never
                                      * fires on ORIC-1 (verified via PCLOG in
                                      * cpu_step) — the JSR $E804 at $E7F5 calls
                                      * a sub-routine that JMPs to warm-start
                                      * instead of RTSing. The PHP-orphaned
                                      * stack is reset by the warm-start handler. */
    .writeleader_entry = 0xE6BA,
    .writeleader_end   = 0xE6C9,
    .tape_type_addr    = 0x0064     /* CLOAD header read at $E4BC stores the 9
                                      * header bytes reversed (STA $5D,X / DEX),
                                      * so on-tape byte 3 (file type) lands at
                                      * $64 — NOT $66 ($66 = reserved byte 1). */
};

static const rom_patches_t rom_patches_basic11 = {
    .name              = "BASIC 1.1 (ORIC Atmos)",
    .getsync_entry     = 0xE735,
    .getsync_end       = 0xE759,
    .getsync_loop      = 0xE720,
    .readbyte_entry    = 0xE6C9,
    .readbyte_end      = 0xE6FB,
    .readbyte_store    = 0x002F,
    .readbyte_storezero= 0x02B1,    /* Atmos GetTapeByte zeroes the parity accumulator */
    .readbyte_setcarry = true,      /* and exits with C=1 — VERIFY logic relies on both */
    .csave_header_buf  = 0x02A8,    /* Atmos WriteFileHeader staging : $02A8..$02B0 (reversed
                                      * on-tape order, see disasm at $E60F-$E618) */
    .csave_filename_buf= 0x027F,    /* Atmos filename buffer (16 chars, null-terminated) */
    .writefileheader_entry = 0xE607,/* Sprint 34at: snapshot point for Atmos —
                                      * same defensive pattern (cheap, harmless
                                      * if $02A8..$02B0 isn't mutated post-call). */
    .cload_data_rts    = 0xE50A,
    .putbyte_entry     = 0xE65E,
    .putbyte_end       = 0xE68A,
    .csave_end         = 0xE93C,
    .writeleader_entry = 0xE75A,
    .writeleader_end   = 0xE769,
    .tape_type_addr    = 0x02AE    /* CLOAD header read at $E4B9 stores the 9
                                     * header bytes reversed (STA $02A7,X / DEX),
                                     * so on-tape byte 3 (file type) lands at
                                     * $02AE. */
};

/**
 * @brief Auto-detect ROM version from loaded ROM data
 *
 * Checks the JMP target at ROM offset 0 (address $C000):
 * - BASIC 1.0: JMP $EA59 (4C 59 EA)
 * - BASIC 1.1: JMP $ECCC (4C CC EC)
 *
 * @return Detected model, or ORIC_MODEL_ORIC1 as default
 */
static oric_model_t detect_rom_version(const memory_t* mem) {
    /* ROM starts at $C000, which is rom[0] */
    if (mem->rom[0] == 0x4C) {  /* JMP instruction */
        uint16_t target = (uint16_t)mem->rom[1] | ((uint16_t)mem->rom[2] << 8);
        if (target == 0xECCC) {
            return ORIC_MODEL_ATMOS;
        }
    }
    return ORIC_MODEL_ORIC1;
}

static const rom_patches_t* get_rom_patches(oric_model_t model) {
    switch (model) {
        case ORIC_MODEL_ATMOS: return &rom_patches_basic11;
        default:               return &rom_patches_basic10;
    }
}

static void print_usage(const char* program_name) {
    printf("Phosphoric v%s\n", EMU_VERSION);
    printf("Usage: %s [options]\n\n", program_name);
    printf("Options:\n");
    printf("  -t, --tape FILE            Load .TAP tape file\n");
    printf("  -d, --disk FILE            Load .DSK disk file in drive A\n");
    printf("      --disk1 FILE           Load .DSK disk file in drive B\n");
    printf("      --disk2 FILE           Load .DSK disk file in drive C\n");
    printf("      --disk3 FILE           Load .DSK disk file in drive D\n");
    printf("      --disk-rom FILE        Load Microdisc ROM (microdis.rom)\n");
    printf("  -r, --rom FILE             Load custom ROM file\n");
    printf("  -h, --hostfs PATH          Mount host directory\n");
    printf("  -f, --fast-load            Fast tape loading (inject directly, no CLOAD needed)\n");
    printf("  -n, --headless             Run without display (headless mode)\n");
    printf("  -c, --cycles NUM           Run for N cycles then exit\n");
    printf("  -v, --verbose              Verbose logging\n");
    printf("      --screenshot FILE      Take screenshot at exit (.ppm or .bmp)\n");
    printf("      --screenshot-at C:FILE Screenshot after C cycles to FILE\n");
    printf("      --frame-dump DIR       Dump frames to directory\n");
    printf("      --frame-dump-interval N  Dump every Nth frame (default: 50)\n");
    printf("  -m, --model MODEL          Machine model: oric1 or atmos (default: auto-detect)\n");
    printf("  -k, --keyboard LAYOUT      Keyboard layout: qwerty (default) or azerty\n");
    printf("  -j, --joystick MODE        Joystick: keys (arrow keys), gamepad (SDL2 controller)\n");
    printf("  -p, --printer FILE         Capture printer output to FILE (LPRINT/LLIST)\n");
    printf("      --printer-type TYPE    Printer type: text (default) or mcp40 (4-color plotter)\n");
    printf("      --scale N              Display scale factor: 1, 2, 3 (default), or 4\n");
    printf("      --type-keys C:TEXT     Auto-type TEXT after C cycles (\\n=Return, \\pN=pause N sec)\n");
    printf("  -b, --breakpoint ADDR      Break when PC reaches address (hex, e.g. ED8A)\n");
    printf("  -D, --debug                Start in debugger mode (break at first instruction)\n");
    printf("      --break ADDR           Set initial debugger breakpoint (hex)\n");
    printf("      --cast-server[=PORT]   Start MJPEG cast server (default port: 8080)\n");
    printf("      --cast-to[=DEVICE]     Cast to Chromecast (native CASTV2 protocol)\n");
    printf("      --cast-discover        Discover Chromecast devices on network\n");
    printf("      --trace FILE           Log CPU instruction trace to FILE\n");
    printf("      --trace-max N          Max instructions to trace (default: unlimited)\n");
    printf("      --trace-irq FILE       Log every IRQ entry + RTI to FILE (debug IRQ handlers)\n");
    printf("      --profile FILE         Write CPU performance profile to FILE on exit\n");
    printf("      --dump-ram-at C:FILE   Dump 64KB RAM to FILE when cycle >= C\n");
    printf("      --rom-info [FILE]      Analyze ROM and print report (or write to FILE)\n");
    printf("      --symbols FILE         Load symbol table (.sym / .lab / .sym65)\n");
    printf("      --tui                  Use ncurses TUI debugger (requires TUI=1 build)\n");
    printf("      --control              IPC control mode for IDE integration (stdin protocol,\n");
    printf("                             logs to stderr, see docs/control_protocol.md)\n");
    printf("      --bench                Headless throughput bench: prints `BENCH cycles=... mhz_eq=... ...`\n");
    printf("                             on stdout at exit. Use with -c N for fixed-cycle run.\n");
    printf("      --loci                 Enable LOCI MIA at $03A0-$03BF\n");
    printf("      --loci-flash DIR       Sandbox root for LOCI file ops (implies --loci)\n");
    printf("      --loci-sdimg PATH      Raw FAT16/32 SD image (read-only, implies --loci)\n");
    printf("                             Mutually exclusive with --loci-flash\n");
    printf("      --serial TYPE          Serial: loopback, tcp:H:P, pty, modem:H:P, com:B,D,P,S,DEV, digitelec:H:P\n");
    printf("      --serial-v23          V23 mode: 1200/75 baud (Minitel/Prestel/Digitelec)\n");
    printf("                            (auto-enabled with digitelec backend)\n");
    printf("      --serial-buffer N     RX FIFO buffer N bytes (prevents overrun, default: off)\n");
    printf("      --serial-irq-on-rdrf  WDC 65C51 IRQ mode (re-trigger while RDRF set)\n");
    printf("      --serial-trace FILE   Serial debug trace (TX/RX/signals with timestamps)\n");
    printf("      --acia-addr ADDR      ACIA base address in hex (default: 031C)\n");
    printf("      --dtl2000 TRANSPORT   Digitelec DTL 2000 (PIA 6821 + ACIA 6850) at $03F8: loopback, tcp:H:P, pty\n");
    printf("      --dtl2000-addr ADDR   DTL 2000 base address in hex (default: 03F8)\n");
    printf("      --save-state FILE      Save emulator state to FILE on exit\n");
    printf("      --load-state FILE      Load emulator state from FILE at startup\n");
    printf("  -?, --help                 Show this help\n");
    printf("\n");
    printf("Controls:\n");
    printf("  F1  - Help menu\n");
    printf("  F2  - Quick save state\n");
    printf("  F3  - Cycle display scale (x1 → x2 → x3 → x4)\n");
    printf("  F4  - Quick load state\n");
    printf("  F5  - Reset (with --loci : also resets MIA state, keeps mounts)\n");
    printf("  F8  - LOCI Action button (warm short press / release on key up)\n");
    printf("  F9  - Enter debugger\n");
    printf("  F10 - Quit\n");
    printf("  F11 - Fullscreen\n");
    printf("  F12 - Screenshot\n");
    printf("\n");
}

/* emulator_t is defined in include/emulator.h */

/* LOCI ROM-swap callback (Sprint 34ad).
 * Loads a ROM image into Oric memory at base_addr and resets the CPU
 * so the new reset vector is honoured. Only base_addr = $C000 is wired
 * for now (BASIC ROM swap); $A000 (Microdisc overlay) returns true
 * without actually swapping — handled by the existing --disk-rom path. */
/* Sprint 34ao: LOCI tape-mount hook. Loads a TAP into emu.tapebuf so
 * the CLOAD ROM patches find data. Path is the already-extracted
 * /tmp/loci_extract_* file produced by sdimg_extract_to_temp. */
static bool loci_tape_mount_cb(void* ctx, const char* host_tape_path) {
    emulator_t* emu = (emulator_t*)ctx;
    if (!emu || !host_tape_path) return false;
    FILE* f = fopen(host_tape_path, "rb");
    if (!f) {
        log_warning("LOCI tape mount: cannot open %s", host_tape_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }

    if (emu->tapebuf) { free(emu->tapebuf); emu->tapebuf = NULL; }
    emu->tapebuf = (uint8_t*)malloc((size_t)sz);
    if (!emu->tapebuf) { fclose(f); return false; }
    size_t rd = fread(emu->tapebuf, 1, (size_t)sz, f);
    fclose(f);
    if ((long)rd != sz) {
        free(emu->tapebuf); emu->tapebuf = NULL;
        return false;
    }
    emu->tapelen = (int)sz;
    emu->tapeoffs = 0;
    emu->tape_loaded = true;
    emu->tape_syncstack = -1;
    /* Do NOT trigger auto-CLOAD here: when LOCI mounts the tape, the
     * LOCI ROM is still in control. Auto-typed keystrokes would land
     * in the LOCI TUI, not BASIC. The user will type CLOAD"" after
     * MIA_BOOT swaps in BASIC. */
    emu->tape_auto_cload_pending = false;
    log_info("LOCI tape mount: %s buffered (%ld bytes, type CLOAD\"\" in BASIC)",
             host_tape_path, sz);
    return true;
}

static bool loci_rom_swap_cb(void* ctx, const char* rom_path, uint16_t base_addr) {
    emulator_t* emu = (emulator_t*)ctx;
    if (!emu || !rom_path || !*rom_path) return false;

    if (base_addr == 0xA000) {
        /* Sprint 34aw : LOCI MIA_BOOT FDC flag → microdis.rom overlay.
         * Le mapping réel Microdisc place l'overlay à $E000-$FFFF (8 KB).
         * On charge le fichier dans un buffer persistant et on active
         * l'overlay du système mémoire (même mécanisme que Microdisc
         * card avec --disk-rom). */
        FILE* fp = fopen(rom_path, "rb");
        if (!fp) {
            log_error("LOCI ROM swap: cannot open %s", rom_path);
            return false;
        }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0 || sz > 16384) { fclose(fp); return false; }
        /* Sprint 34c hardening : buffer owned by emulator_t now (was a
         * function-scope static with an "acceptable leak at shutdown"
         * comment). Freed by emulator_cleanup. */
        if (emu->loci_overlay_buf) {
            free(emu->loci_overlay_buf);
            emu->loci_overlay_buf = NULL;
        }
        emu->loci_overlay_buf = (uint8_t*)malloc((size_t)sz);
        if (!emu->loci_overlay_buf) { fclose(fp); return false; }
        if (fread(emu->loci_overlay_buf, 1, (size_t)sz, fp) != (size_t)sz) {
            free(emu->loci_overlay_buf); emu->loci_overlay_buf = NULL;
            fclose(fp);
            return false;
        }
        fclose(fp);
        emu->memory.overlay_rom         = emu->loci_overlay_buf;
        emu->memory.overlay_rom_size    = (uint32_t)sz;
        emu->memory.overlay_active      = true;
        emu->memory.basic_rom_disabled  = true;   /* romdis = ROM disable signal */
        log_info("LOCI ROM swap: microdisc overlay activated ($E000+, %ld bytes from %s)",
                 sz, rom_path);
        /* Re-reset CPU so $FFFC reset vector is fetched from Microdisc
         * overlay instead of the BASIC ROM loaded in the prior $C000 call. */
        cpu_reset(&emu->cpu);
        return true;
    }

    if (base_addr != 0xC000) {
        log_info("LOCI ROM swap: ignored base $%04X (only $C000 / $A000 supported)",
                 base_addr);
        return true;
    }
    log_info("LOCI ROM swap: loading %s at $C000", rom_path);
    if (!memory_load_rom(&emu->memory, rom_path, 0)) {
        log_error("LOCI ROM swap: failed to load %s", rom_path);
        return false;
    }
    /* Sprint 34ao: when LOCI swaps to BASIC 1.1 (Atmos) the previous
     * BASIC 1.0 CLOAD patches no longer match — re-detect from the
     * filename so cassette interception keeps working. */
    const char* base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    bool is_b11 = (strstr(base, "11") != NULL) ||
                  (strstr(base, "atmos") != NULL) ||
                  (strstr(base, "ATMOS") != NULL);
    const rom_patches_t* new_patches = get_rom_patches(
        is_b11 ? ORIC_MODEL_ATMOS : ORIC_MODEL_ORIC1);
    if (new_patches != emu->rom_patches) {
        emu->rom_patches = new_patches;
        emu->model = is_b11 ? ORIC_MODEL_ATMOS : ORIC_MODEL_ORIC1;
        log_info("LOCI ROM swap: patches → %s", emu->rom_patches->name);
    }
    /* Reset the 6502 so it re-reads the new $FFFC reset vector. */
    cpu_reset(&emu->cpu);
    return true;
}

/* Sync the LOCI keyboard report from the current SDL keyboard state.
 *
 * SDL_Scancode values map 1:1 to HID Usage IDs from the Keyboard/Keypad
 * usage page (deliberately, per the SDL docs), so the boot keyboard
 * report we hand to LOCI just collects the first six scancodes whose
 * state is "down" and packs the SDL modifier flags into the HID byte.
 *
 * Called on every KEYDOWN/KEYUP — cheap (one SDL state read + up to
 * ~230 iterations bounded by the standard usage page). */
#ifdef HAS_SDL2
static void loci_sync_kbd_from_sdl(emulator_t* emu) {
    if (!emu || !emu->has_loci) return;

    int numkeys = 0;
    const Uint8* state = SDL_GetKeyboardState(&numkeys);
    if (!state) return;

    SDL_Keymod m = SDL_GetModState();
    uint8_t hid_mod = 0;
    if (m & KMOD_LCTRL)  hid_mod |= 0x01;
    if (m & KMOD_LSHIFT) hid_mod |= 0x02;
    if (m & KMOD_LALT)   hid_mod |= 0x04;
    if (m & KMOD_LGUI)   hid_mod |= 0x08;
    if (m & KMOD_RCTRL)  hid_mod |= 0x10;
    if (m & KMOD_RSHIFT) hid_mod |= 0x20;
    if (m & KMOD_RALT)   hid_mod |= 0x40;
    if (m & KMOD_RGUI)   hid_mod |= 0x80;

    uint8_t keys[6] = {0};
    int kn = 0;
    /* HID modifier keys live at 0xE0+ — skip those, they're already in
     * hid_mod. Standard usage page tops out around 0xE7; clamp. */
    int max = numkeys < 0xE0 ? numkeys : 0xE0;
    for (int sc = SDL_SCANCODE_A; sc < max && kn < 6; sc++) {
        if (state[sc]) {
            keys[kn++] = (uint8_t)sc;
        }
    }
    loci_kbd_set_report(&emu->loci, hid_mod, keys);
}
#endif

/* LOCI action-button install hook (Sprint 34ai).
 * Saves the current IRQ vector at $FFFE/F, redirects it to the trap at
 * $03BA, then pulses the CPU IRQ line. The trap bytes themselves were
 * already mirrored into the MIA register file by loci_action_button_short. */
static void loci_action_install_irq_trap(void* ctx) {
    emulator_t* emu = (emulator_t*)ctx;
    if (!emu) return;
    /* Save current vector. The ORIC IRQ vector lives in ROM at $FFFE/F,
     * backed by mem->rom (offset $3FFE/F since rom starts at $C000). */
    uint8_t lo = emu->memory.rom[0x3FFE];
    uint8_t hi = emu->memory.rom[0x3FFF];
    emu->loci.saved_irq_vector = (uint16_t)lo | ((uint16_t)hi << 8);
    /* Redirect to the trap at $03BA. */
    emu->memory.rom[0x3FFE] = 0xBA;
    emu->memory.rom[0x3FFF] = 0x03;
    /* Pulse the IRQ line. Source bit is arbitrary — VIA works because
     * the CPU handler doesn't introspect the source for this trap. */
    cpu_irq_set(&emu->cpu, IRQF_VIA);
}

/* LOCI action-button release hook (Sprint 34ai).
 * Sets the 6502 V flag so the BVC -2 spin falls through and the trap's
 * JMP ($FFFA) executes. Also restores the original IRQ vector so a
 * later non-trap IRQ behaves normally. */
static void loci_action_release_irq_trap(void* ctx) {
    emulator_t* emu = (emulator_t*)ctx;
    if (!emu) return;
    emu->cpu.P |= FLAG_OVERFLOW;
    uint16_t v = emu->loci.saved_irq_vector;
    emu->memory.rom[0x3FFE] = (uint8_t)(v & 0xFF);
    emu->memory.rom[0x3FFF] = (uint8_t)(v >> 8);
    /* Clear the IRQ source so it doesn't re-fire on the next instruction. */
    cpu_irq_clear(&emu->cpu, IRQF_VIA);
}

/* I/O callback: route VIA and Microdisc register access */
static uint8_t io_read_callback(uint16_t address, void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;

    /* LOCI MIA: $03A0-$03BF (checked first — independent of other peripherals) */
    if (emu->has_loci && loci_addr_in_mia(address)) {
        return loci_read(&emu->loci, address);
    }
    /* LOCI TAP $0315-$0317 (Sprint 34af). Overlaps Microdisc $0310-$031F
     * — LOCI claims priority when active since it replaces the cassette
     * interface. */
    if (emu->has_loci && loci_addr_in_tap(address)) {
        return loci_tap_read(&emu->loci, address);
    }
    /* LOCI DSK $0310-$0314 + $0318-$0319 (Sprint 34ae). Only when no real
     * Microdisc is present — otherwise the existing microdisc handler
     * owns the range. */
    if (emu->has_loci && !emu->has_microdisc && loci_addr_in_dsk(address)) {
        return loci_dsk_read(&emu->loci, address);
    }

    /* ACIA 6551 serial: $031C-$031F (checked first — overlaps Microdisc range) */
    if (emu->has_serial && address >= emu->acia_base_addr && address <= (emu->acia_base_addr + 3)) {
        return acia_read(&emu->acia, address);
    }

    /* Microdisc I/O: $0310-$031B (reduced when ACIA present) */
    if (emu->has_microdisc && address >= 0x0310 && address <= 0x031F) {
        /* If serial is active, ACIA owns $031C-$031F */
        if (emu->has_serial && address >= emu->acia_base_addr) {
            return acia_read(&emu->acia, address);
        }
        return microdisc_read(&emu->microdisc, address);
    }

    /* Digitelec DTL 2000 (PIA 6821 + ACIA 6850): $03F8-$03FD.
     * Intercepted ahead of the VIA mirror that otherwise aliases this range. */
    if (emu->has_dtl2000 && dtl2000_addr_in_range(&emu->dtl2000, address)) {
        return dtl2000_read(&emu->dtl2000, address);
    }

    /* VIA 6522: $0300-$030F (mirrored in $0300-$03FF) */
    return via_read(&emu->via, (uint8_t)(address & 0x0F));
}

/**
 * @brief Decode PSG bus state and execute operation
 *
 * ORIC-1 PSG (AY-3-8912) is controlled via VIA (from Oricutron):
 * - VIA Port A (ORA) = PSG data bus
 * - VIA CA2 output = PSG BC1 (PCR bits 1-3: mode 6=low, mode 7=high)
 * - VIA CB2 output = PSG BDIR (PCR bits 5-7: mode 6=low, mode 7=high)
 *
 * PSG operations:
 * - BDIR=1, BC1=1 → Latch Address (ORA → PSG address register)
 * - BDIR=1, BC1=0 → Write Data (ORA → selected PSG register)
 * - BDIR=0, BC1=1 → Read Data (selected PSG register → VIA IRA)
 * - BDIR=0, BC1=0 → Inactive
 *
 * The ROM toggles CA2/CB2 via PCR writes, so this function must
 * be called when PCR, ORA, or ORB change.
 */
static void psg_decode(emulator_t* emu) {
    /* BC1 = CA2 output state (PCR bits 1-3) */
    uint8_t ca2_mode = (emu->via.pcr >> 1) & 0x07;
    bool bc1 = (ca2_mode == 0x07); /* Mode 7 = CA2 high */

    /* BDIR = CB2 output state (PCR bits 5-7) */
    uint8_t cb2_mode = (emu->via.pcr >> 5) & 0x07;
    bool bdir = (cb2_mode == 0x07); /* Mode 7 = CB2 high */

    if (bdir && bc1) {
        /* Latch Address */
        ay_write_address(&emu->psg, emu->via.ora);
    } else if (bdir && !bc1) {
        /* Write Data */
        ay_write_data(&emu->psg, emu->via.ora);
    } else if (!bdir && bc1) {
        /* Read Data - PSG data goes onto VIA input for Port A reads */
        emu->via.ira = ay_read_data(&emu->psg);
    }
}

/**
 * @brief PSG Port A input callback - returns keyboard matrix row data
 *
 * VIA ORB bits 0-2 select the keyboard column (active via 74LS138 decoder).
 * Returns row data: 0xFF = no keys, bit cleared = key pressed (active low).
 *
 * Note: the IJK joystick is NOT blended here any more (v1.16 model,
 * wrong on real hardware) — it lives on the printer port (VIA Port A
 * direct), see ijk_port_a_read() below.
 */
static uint8_t keyboard_matrix_read(void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;
    uint8_t col = emu->via.orb & 0x07;
    return emu->keyboard.matrix[col];
}

/**
 * @brief VIA Port A external pins callback — IJK joystick interface
 *
 * Hardware-accurate model (validated against Oricutron, after an
 * external tester proved the PSG-blend model wrong on a real IJK):
 *   - Enable: VIA PB4 (printer strobe) must be an OUTPUT driven LOW.
 *   - Select: Port A bits 6-7 (driven by the program as outputs) —
 *     bit 6 = 1 selects stick A, bit 7 = 1 selects stick B,
 *     both = 1 selects none. The single emulated stick is stick A.
 *   - Output: bits 0-5 active low (IJK_RIGHT..IJK_UP per joystick.h),
 *     bit 5 (IJK_PRESENCE) pulled low whenever the interface is
 *     enabled — programs use it to detect the interface.
 * Returns 0xFF (pulled-up lines) when disabled or not plugged.
 */
static uint8_t ijk_port_a_read(void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;
    return oric_joystick_port_a_pins(&emu->joystick,
                                     emu->via.ora, emu->via.ddra,
                                     emu->via.orb, emu->via.ddrb);
}

/**
 * @brief VIA Port B read callback - keyboard scan result on PB3
 *
 * On the ORIC, the keyboard scan works as follows (from Oricutron):
 * - ROM writes a mask to PSG register 14 (which rows to test)
 * - ROM selects column via VIA ORB bits 0-2
 * - Hardware checks if any key matches: keystates[col] & (~reg14)
 * - Result appears on VIA PB3 (bit 3): 1 = key pressed, 0 = no key
 *
 * key_matrix[] uses active-low (0 = pressed), so ~key_matrix gives
 * 1 = pressed (matching Oricutron's keystates convention).
 */
static uint8_t portb_read_callback(void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;

    /* AY register 7 bit 6 controls Port A direction:
     * bit 6 = 0 → Port A in output mode → keyboard scan fails (bus conflict)
     * bit 6 = 1 → Port A in input mode → keyboard scan works
     * This matches Oricutron's ay_update_keybits() behavior.
     * Programs that play sound with reg7 bit 6=0 must restore it to
     * enable keyboard scanning (e.g. ay_write(7, $7F)). */
    if (!(emu->psg.registers[7] & 0x40)) {
        /* Port A in output mode → PB3 always 0 (no key detected) */
        return 0xF7;
    }

    uint8_t col = emu->via.orb & 0x07;
    uint8_t reg14 = emu->psg.registers[14];

    /* Check: any pressed key in column matches the inverted mask?
     * ~key_matrix = pressed keys (1=pressed), ~reg14 = rows to test */
    uint8_t pressed = (~emu->keyboard.matrix[col]) & (~reg14) & 0xFF;

    /* PB3 = 1 if any key matches, 0 otherwise.
     * Other input bits default to 1 (no external input). */
    return pressed ? 0xFF : 0xF7;
}

static void io_write_callback(uint16_t address, uint8_t value, void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;

    /* LOCI MIA: $03A0-$03BF */
    if (emu->has_loci && loci_addr_in_mia(address)) {
        loci_write(&emu->loci, address, value);
        return;
    }
    /* LOCI TAP $0315-$0317 (Sprint 34af). */
    if (emu->has_loci && loci_addr_in_tap(address)) {
        loci_tap_write(&emu->loci, address, value);
        return;
    }
    /* LOCI DSK $0310-$0314 + $0318-$0319 (Sprint 34ae). */
    if (emu->has_loci && !emu->has_microdisc && loci_addr_in_dsk(address)) {
        loci_dsk_write(&emu->loci, address, value);
        return;
    }

    /* ACIA 6551 serial: $031C-$031F */
    if (emu->has_serial && address >= emu->acia_base_addr && address <= (emu->acia_base_addr + 3)) {
        acia_write(&emu->acia, address, value);
        return;
    }

    /* Microdisc I/O: $0310-$031F */
    if (emu->has_microdisc && address >= 0x0310 && address <= 0x031F) {
        /* If serial is active, ACIA owns $031C-$031F */
        if (emu->has_serial && address >= emu->acia_base_addr) {
            acia_write(&emu->acia, address, value);
            return;
        }
        if (fdc_trace_enabled()) {
            fprintf(stderr, "[FDC] PC=%04X cyc=%llu write $%04X = %02X\n",
                    emu->cpu.PC, (unsigned long long)emu->cpu.cycles,
                    address, value);
        }
        microdisc_write(&emu->microdisc, address, value);
        /* Sync overlay flags to memory system */
        emu->memory.basic_rom_disabled = emu->microdisc.romdis;
        emu->memory.overlay_active = emu->microdisc.diskrom;
        return;
    }

    /* Digitelec DTL 2000 (PIA 6821 + ACIA 6850): $03F8-$03FD.
     * Intercepted ahead of the VIA mirror that otherwise aliases this range. */
    if (emu->has_dtl2000 && dtl2000_addr_in_range(&emu->dtl2000, address)) {
        dtl2000_write(&emu->dtl2000, address, value);
        return;
    }

    uint8_t reg = (uint8_t)(address & 0x0F);

    /* Intercept VIA Port A writes to forward to PSG data bus */
    if (reg == VIA_ORA || reg == 0x0F) {
        /* ORA write: data goes to PSG bus. The actual PSG operation
         * depends on BDIR/BC1 which are set via ORB. */
    }

    /* LOCI snoops VIA ORB writes ($0300) for the cassette motor line
     * (PB6), like the firmware tap_act() hook (Sprint 36f). */
    if (emu->has_loci && address == 0x0300) {
        loci_tap_motor(&emu->loci, (value & 0x40) != 0);
    }

    /* Capture old PCR before VIA write (for printer strobe edge detection) */
    uint8_t old_pcr = emu->via.pcr;

    via_write(&emu->via, reg, value);

    /* Decode PSG bus state ONLY when control lines change.
     * BC1 = CA2, BDIR = CB2, both controlled by PCR bits.
     * Matching Oricutron: PSG bus decode is triggered only on PCR writes,
     * NOT on ORB writes (which select keyboard columns) or ORA writes
     * (which just change data bus). The ROM sequence is:
     *   1. Write ORA with address/data value
     *   2. Write PCR to set BDIR/BC1 → PSG operation happens HERE
     *   3. Write PCR to clear BDIR/BC1 */
    if (reg == VIA_PCR) {
        psg_decode(emu);
        /* Check for Centronics printer STROBE (CA2 forced low → high) */
        oric_printer_check_strobe(&emu->printer, old_pcr, value, emu->via.ora);
    }
}

/* VIA IRQ callback - level-triggered: set/clear VIA IRQ source bit */
static void irq_callback(bool state, void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;
    if (state) {
        cpu_irq_set(&emu->cpu, IRQF_VIA);
    } else {
        cpu_irq_clear(&emu->cpu, IRQF_VIA);
    }
}

/* Microdisc CPU IRQ callbacks - level-triggered: set/clear DISK IRQ source bit */
static void microdisc_cpu_irq_set(emulator_t* emu) {
    cpu_irq_set(&emu->cpu, IRQF_DISK);
}

static void microdisc_cpu_irq_clr(emulator_t* emu) {
    cpu_irq_clear(&emu->cpu, IRQF_DISK);
}

/* Sprint 34ax : LOCI DSK bus callbacks — réutilise IRQF_DISK level-triggered
 * et synchronise overlay/ROMDIS dans le sous-système mémoire à chaque
 * CTRL write. Sans ça le Microdisc ROM (sous LOCI MIA_BOOT FDC) reste
 * bloqué après le RESTORE command — il attend l'IRQ et la commutation. */
static void loci_dsk_cpu_irq_set(void* ctx) {
    emulator_t* emu = (emulator_t*)ctx;
    cpu_irq_set(&emu->cpu, IRQF_DISK);
}
static void loci_dsk_cpu_irq_clr(void* ctx) {
    emulator_t* emu = (emulator_t*)ctx;
    cpu_irq_clear(&emu->cpu, IRQF_DISK);
}
static void loci_dsk_sync_overlay(void* ctx, bool basic_disabled, bool overlay_active) {
    emulator_t* emu = (emulator_t*)ctx;
    emu->memory.basic_rom_disabled = basic_disabled;
    emu->memory.overlay_active     = overlay_active;
}

/* ACIA 6551 serial IRQ callbacks */
static void acia_cpu_irq_set(emulator_t* emu) {
    cpu_irq_set(&emu->cpu, IRQF_SERIAL);
}

static void acia_cpu_irq_clr(emulator_t* emu) {
    cpu_irq_clear(&emu->cpu, IRQF_SERIAL);
}

/* Digitelec DTL 2000 (ACIA 6850) IRQ callbacks */
static void dtl2000_cpu_irq_set(emulator_t* emu) {
    cpu_irq_set(&emu->cpu, IRQF_DTL2000);
}

static void dtl2000_cpu_irq_clr(emulator_t* emu) {
    cpu_irq_clear(&emu->cpu, IRQF_DTL2000);
}

static bool emulator_init(emulator_t* emu) {
    log_info("Initializing Phosphoric v%s", EMU_VERSION);

    if (!memory_init(&emu->memory)) {
        log_error("Failed to initialize memory");
        return false;
    }

    cpu_init(&emu->cpu, &emu->memory);

    via_init(&emu->via);
    via_reset(&emu->via);

    /* Initialize keyboard */
    oric_keyboard_init(&emu->keyboard);

    /* Initialize joystick (disabled by default) */
    oric_joystick_init(&emu->joystick);

    /* Initialize printer (disabled by default) */
    oric_printer_init(&emu->printer);

    /* Initialize ACIA 6551 serial interface (disabled by default) */
    acia_init(&emu->acia);
    emu->acia.irq_set = acia_cpu_irq_set;
    emu->acia.irq_clr = acia_cpu_irq_clr;
    emu->acia.irq_userdata = emu;

    /* Initialize Digitelec DTL 2000 modem card (disabled by default) */
    dtl2000_init(&emu->dtl2000, DTL2000_DEFAULT_BASE);
    emu->dtl2000.irq_set = dtl2000_cpu_irq_set;
    emu->dtl2000.irq_clr = dtl2000_cpu_irq_clr;
    emu->dtl2000.irq_userdata = emu;

    /* Initialize PSG (AY-3-8912) with keyboard input callback */
    ay_init(&emu->psg, ORIC_CLOCK_HZ);
    emu->psg.porta_input = keyboard_matrix_read;
    emu->psg.userdata = emu;

    /* Wire up I/O callbacks */
    memory_set_io_callbacks(&emu->memory, io_read_callback, io_write_callback, emu);
    via_set_irq_callback(&emu->via, irq_callback, emu);

    /* VIA Port A is driven by PSG in READ mode: psg_decode() updates via.ira
     * (IRA init = 0xFF, no phantom keys). porta_read models the EXTERNAL
     * devices on the printer port pins — the IJK joystick interface
     * (wired-AND with IRA in via_read, cf. ijk_port_a_read). */
    emu->via.porta_read = ijk_port_a_read;
    emu->via.portb_read = portb_read_callback;
    emu->via.userdata = emu;

    /* Initialize video - charset is read from RAM at $B400 by the renderer.
     * vid->charset is left NULL so the renderer uses the RAM copy
     * which the ROM populates during boot. */
    video_init(&emu->video);

    /* Initialize renderer if not headless */
    if (!emu->headless) {
        renderer_init(emu->scale_factor > 0 ? emu->scale_factor : 3);
#ifdef HAS_SDL2
        SDL_StartTextInput();  /* Enable TEXTINPUT events for symbolic keyboard */
#endif
    }

    /* Initialize audio output (connects PSG to SDL2 audio callback) */
    if (!emu->headless) {
        if (!audio_init(&emu->psg)) {
            log_warning("Failed to initialize audio output");
        }
    }

    if (!hostfs_init(&emu->hostfs)) {
        log_error("Failed to initialize host filesystem");
        return false;
    }

    /* Initialize debugger */
    debugger_init(&emu->debugger);

    emu->running = true;
    /* Note: fast_load, headless, max_cycles are set by caller before init */
    emu->screenshot_file = NULL;
    emu->screenshot_at_cycles = -1;
    emu->screenshot_at_file = NULL;
    emu->frame_dump_dir = NULL;
    emu->frame_dump_interval = 50;
    emu->dump_ram_at_cycles = -1;
    emu->dump_ram_at_file = NULL;
    emu->dump_ram_at_done = true;
    emu->irq_trace_fp = NULL;
    emu->irq_trace_active = false;
    emu->irq_trace_depth = 0;
    emu->cpu.irq_trace_fp = NULL;
    emu->cpu.irq_trace_count = 0;

    log_info("Emulator initialized successfully");
    return true;
}

static void emulator_cleanup(emulator_t* emu) {
    if (emu->has_loci) {
        loci_cleanup(&emu->loci);
    }
    if (emu->loci_overlay_buf) {
        free(emu->loci_overlay_buf);
        emu->loci_overlay_buf = NULL;
    }
    if (emu->tui_mode) {
        tui_cleanup();
        emu->tui_mode = false;
    }
    log_info("Shutting down emulator");
    if (emu->irq_trace_fp) {
        log_info("IRQ trace: %llu interrupts logged",
                 (unsigned long long)emu->cpu.irq_trace_count);
        fclose((FILE*)emu->irq_trace_fp);
        emu->irq_trace_fp = NULL;
        emu->cpu.irq_trace_fp = NULL;
    }
    if (!emu->headless) {
        audio_cleanup();
        renderer_cleanup();
    }
    video_cleanup(&emu->video);
    hostfs_cleanup(&emu->hostfs);
    memory_cleanup(&emu->memory);
    if (emu->tapebuf) {
        free(emu->tapebuf);
        emu->tapebuf = NULL;
    }
    if (emu->fastload_buf) {
        free(emu->fastload_buf);
        emu->fastload_buf = NULL;
    }
    if (emu->has_castv2) {
        castv2_disconnect(&emu->castv2_client);
    }
    if (emu->has_cast_server) {
        cast_server_stop(&emu->cast_server);
    }
    oric_printer_close(&emu->printer);
    if (emu->serial_backend) {
        serial_backend_destroy(emu->serial_backend);
        emu->serial_backend = NULL;
        emu->has_serial = false;
    }
    if (emu->dtl2000_backend) {
        serial_backend_destroy(emu->dtl2000_backend);
        emu->dtl2000_backend = NULL;
        emu->has_dtl2000 = false;
    }
    /* Close ACIA trace and free RX FIFO */
    acia_set_trace(&emu->acia, NULL);
    acia_set_rx_fifo(&emu->acia, 0);
#ifdef HAS_SDL2
    oric_joystick_close_sdl(&emu->joystick);
#endif
    if (emu->has_microdisc) {
        microdisc_cleanup(&emu->microdisc);
    }
    for (int i = 0; i < MICRODISC_MAX_DRIVES; i++) {
        if (emu->disks[i]) {
            sedoric_destroy(emu->disks[i]);
            emu->disks[i] = NULL;
        }
    }
    log_info("Emulator cleanup complete");
}

/**
 * @brief Rechain BASIC line pointers after CLOAD
 *
 * Reproduces the ROM's rechain routine at $C56F (BASIC 1.0) / equivalent
 * (Atmos). Walks the BASIC program from TXTTAB ($9A/$9B), finds each line's
 * null terminator, computes the actual next-line address, and updates the
 * 2-byte pointer at the start of each line.
 *
 * TAP files may have stale next-line pointers (saved from different memory
 * addresses). The ORIC ROM does NOT rechain after CLOAD — only when lines
 * are edited. Multi-block programs like TYRANN need rechaining after each
 * block loads.
 *
 * @param mem  Memory subsystem
 */
static void basic_rechain(memory_t* mem) {
    /* TXTTAB = start of BASIC text ($9A/$9B), typically $0501 */
    uint16_t ptr = (uint16_t)(mem->ram[0x9A] | (mem->ram[0x9B] << 8));

    int lines_fixed = 0;
    while (ptr + 1 < 0xC000) {
        /* Check next-line pointer high byte — $00 means end of program.
         * The ROM $C56F checks ONLY the high byte (ptr+1): LDA ($91),Y
         * with Y=1, then BEQ to exit. Valid next-line pointers always
         * have hi >= $05 (BASIC text starts at $0501). A hi byte of $00
         * is the end-of-program marker, even if the low byte is non-zero
         * (e.g. TYRANN block 1 ends with $49 $00 at $132B). */
        uint8_t next_hi = mem->ram[ptr + 1];
        if (next_hi == 0)
            break;

        /* Find the null terminator: scan from offset 4 (after pointer + line num) */
        uint16_t scan = ptr + 4;
        while (scan < 0xC000 && mem->ram[scan] != 0x00)
            scan++;
        scan++;  /* Skip the null terminator */

        /* Update the next-line pointer to the computed address */
        uint16_t old_next = (uint16_t)(mem->ram[ptr] | (mem->ram[ptr + 1] << 8));
        if (old_next != scan) {
            mem->ram[ptr]     = (uint8_t)(scan & 0xFF);
            mem->ram[ptr + 1] = (uint8_t)(scan >> 8);
            lines_fixed++;
        }

        ptr = scan;
    }

    if (lines_fixed > 0) {
        log_info("BASIC rechain: fixed %d line pointer(s)", lines_fixed);
    }
}

/**
 * @brief ROM patching for CLOAD support
 *
 * Intercepts ROM cassette routines by checking CPU PC after each instruction.
 * When PC hits known ROM entry points (getsync, readbyte), we inject tape
 * data directly into CPU registers and skip to the routine's RTS.
 * This is the same approach used by Oricutron.
 *
 * Addresses are ROM-version-specific, loaded from emu->rom_patches:
 *   BASIC 1.0 (ORIC-1):  getsync=$E696, readbyte=$E630, loop=$E681
 *   BASIC 1.1 (Atmos):   getsync=$E735, readbyte=$E6C9, loop=$E720
 */
static void tape_patches(emulator_t* emu) {
    if (!emu->rom_patches)
        return;

    const rom_patches_t* p = emu->rom_patches;
    uint16_t pc = emu->cpu.PC;

    /* CSAVE patches work even without a tape loaded */
    if (pc == p->writeleader_entry || pc == p->putbyte_entry || pc == p->csave_end ||
        (p->writefileheader_entry && pc == p->writefileheader_entry)) {
        goto do_patch;  /* Skip tape_loaded check for CSAVE */
    }

    if (!emu->tape_loaded)
        return;

do_patch:
    if (pc == p->getsync_entry) {
        /* getsync: scan forward to first 0x16 sync byte.
         * Leave tapeoffs pointing AT the 0x16 so readbyte will
         * read the sync bytes (ROM confirmation loop needs them).
         * The ORIC ROM reads 9 header bytes after $24, which
         * correctly parses start/end addresses from the raw TAP. */
        if (emu->tapebuf[emu->tapeoffs] != 0x16) {
            while (emu->tapeoffs < emu->tapelen &&
                   emu->tapebuf[emu->tapeoffs] != 0x16) {
                emu->tapeoffs++;
            }
            if (emu->tapeoffs >= emu->tapelen)
                return;
        }
        log_info("TAPE: getsync at tapeoffs=%d/%d", emu->tapeoffs, emu->tapelen);
        /* Save stack pointer for sync loop recovery */
        emu->tape_syncstack = emu->cpu.SP;
        /* Jump to end of getsync */
        emu->cpu.PC = p->getsync_end;
    } else if (pc == p->readbyte_entry) {
        /* readbyte: feed next byte from tape buffer to ROM. Sprint 34ar
         * (senior-review fix): mirror what the real GetTapeByte does
         * version-by-version — on Atmos, $02B1 is the parity accumulator
         * and the routine exits with C=1 ; on Oric-1, neither applies.
         * Without these two effects, BASIC 1.1's VERIFY logic accumulates
         * a phantom error count and prints "Errors found" cosmetically
         * even though the data loaded correctly. Reference: Oricutron's
         * .pch tape patch + Atmos GetTapeByte disassembly. */
        if (emu->tapeoffs < emu->tapelen) {
            uint8_t byte = emu->tapebuf[emu->tapeoffs++];
            emu->cpu.A = byte;
            if (byte == 0) emu->cpu.P |= FLAG_ZERO;
            else           emu->cpu.P &= ~FLAG_ZERO;
            if (p->readbyte_setcarry) emu->cpu.P |=  FLAG_CARRY;
            else                      emu->cpu.P &= ~FLAG_CARRY;
            memory_write(&emu->memory, p->readbyte_store, byte);
            if (p->readbyte_storezero) {
                memory_write(&emu->memory, p->readbyte_storezero, 0x00);
            }
            emu->cpu.PC = p->readbyte_end;
            emu->tape_readbyte_active = true;
        }
        /* Tape exhausted: don't intercept — let the ROM bit-decoder time
         * out naturally. With $02B1/carry now correct, the silence handler
         * is no longer needed to avoid spurious "Errors found", and not
         * patching here means BASIC's CLOAD termination signals the end
         * of tape via its own logic instead of running on synthesised $00. */
    } else if (pc == p->getsync_loop) {
        /* Sync loop recovery */
        if (emu->tape_syncstack >= 0) {
            emu->cpu.SP = (uint8_t)emu->tape_syncstack;
            emu->tape_syncstack = -1;
            if (emu->tapebuf[emu->tapeoffs] != 0x16) {
                while (emu->tapeoffs < emu->tapelen &&
                       emu->tapebuf[emu->tapeoffs] != 0x16)
                    emu->tapeoffs++;
                if (emu->tapeoffs >= emu->tapelen) {
                    emu->tape_loaded = false;
                    return;
                }
            }
            emu->cpu.PC = p->getsync_end;
        }
    } else if (p->writefileheader_entry && pc == p->writefileheader_entry) {
        /* Sprint 34at : snapshot the header staging buffer and the
         * filename buffer at WriteFileHeader entry — before any data-write
         * loop reuses the staging ZP as a work pointer. On ORIC-1, $5F/$60
         * holds TXTTAB right now but will be advanced to VARTAB during the
         * data write, so reading it at csave_end would give the WRONG
         * start address. */
        if (p->csave_header_buf) {
            for (int i = 0; i < 9; i++) {
                emu->csave_header_snap[i] = emu->memory.ram[p->csave_header_buf + i];
            }
        }
        if (p->csave_filename_buf) {
            for (int i = 0; i < 16; i++) {
                emu->csave_fname_snap[i] = (char)emu->memory.ram[p->csave_filename_buf + i];
            }
            emu->csave_fname_snap[16] = 0;
        }
        emu->csave_snap_valid = true;
        /* Do NOT modify PC — let the ROM execute WriteFileHeader normally. */
    } else if (pc == p->writeleader_entry) {
        /* CSAVE: write tape leader — open output file if needed */
        if (!emu->csave_file) {
            /* Read filename from $0035 keeping only [A-Z0-9_-.] up to 11
             * chars. BASIC stores the name with surrounding quotes and
             * sometimes a length-prefix byte; the raw bytes are not
             * filesystem-safe. */
            char csave_name[16] = {0};
            int nlen = 0;
            for (int i = 0; i < 16 && nlen < 11; i++) {
                unsigned char ch = emu->memory.ram[0x0035 + i];
                if (ch == 0) break;
                if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
                if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                    ch == '_' || ch == '-' || ch == '.') {
                    csave_name[nlen++] = (char)ch;
                }
            }

            /* Build filename: name.tap (or csave_output.tap if empty) */
            char csave_path[64];
            if (nlen > 0) {
                snprintf(csave_path, sizeof(csave_path), "%s.tap", csave_name);
            } else {
                snprintf(csave_path, sizeof(csave_path), "csave_output.tap");
            }

            emu->csave_file = fopen(csave_path, "wb");
            if (emu->csave_file) {
                uint8_t leader[] = { 0x16, 0x16, 0x16 };
                fwrite(leader, 1, 3, emu->csave_file);
                emu->csave_byte_count = 0;
                emu->csave_snap_valid = false;  /* 34at : reset between CSAVEs */
                emu->csave_in_progress = true;  /* 34at : guard against shared-path re-entry */
                strncpy(emu->csave_last_path, csave_path,
                        sizeof(emu->csave_last_path) - 1);
                emu->csave_last_path[sizeof(emu->csave_last_path) - 1] = 0;
                log_info("CSAVE: saving to %s", csave_path);
            }
        } else {
            /* Subsequent leader (between header and data) */
            uint8_t leader[] = { 0x16, 0x16, 0x16 };
            fwrite(leader, 1, 3, emu->csave_file);
        }
        emu->cpu.PC = p->writeleader_end;
    } else if (pc == p->putbyte_entry) {
        /* CSAVE: putbyte is intercepted but we ignore the byte. The TAP
         * is rebuilt from RAM at csave_end (which produces a properly
         * structured Oric TAP, unlike the byte-stream produced here by
         * the BASIC ROM, which proved unreliable). */
        emu->csave_byte_count++;
        emu->cpu.PC = p->putbyte_end;
    } else if (pc == p->csave_end) {
        /* 34at : guard against re-entry on shared code paths (ORIC-1
         * $E80A is reached from CLOAD's exit too). */
        if (!emu->csave_in_progress) return;
        emu->csave_in_progress = false;
        /* CSAVE complete — rebuild the TAP (Sprint 34as).
         *
         * Sourcing priority :
         *  1. If p->csave_header_buf is set (Atmos), read the 9-byte
         *     header staging buffer the ROM populated before WriteFileHeader.
         *     This buffer is CSAVE-variant-agnostic : works for BASIC
         *     programs AND machine-code (`,A start,E end`) without any
         *     special-casing.
         *  2. Fallback (BASIC 1.0 for now) : TXTTAB/VARTAB pointers in
         *     zero-page. Works for BASIC programs only.
         *
         * The header buffer layout (Atmos, memory address → tape byte) :
         *   $02A8 → byte 9 (null sep)
         *   $02A9 → byte 8 (start_lo)
         *   $02AA → byte 7 (start_hi)
         *   $02AB → byte 6 (end_lo)
         *   $02AC → byte 5 (end_hi)
         *   $02AD → byte 4 (auto-flag, $C7)
         *   $02AE → byte 3 (type, $00=BASIC)
         *   $02AF → byte 2 (padding)
         *   $02B0 → byte 1 (padding)
         * Tape order is the buffer read in reverse (X=9 down to X=1).
         */
        if (emu->csave_file) {
            fclose(emu->csave_file);
            emu->csave_file = NULL;
            emu->csave_byte_count = 0;
        }

        uint16_t start_addr, end_addr;
        uint8_t  header_type, header_auto;
        /* Sprint 34at : prefer the snapshot captured at writefileheader_entry,
         * because data-write loops on ORIC-1 reuse $5F/$60 as a work pointer
         * and the live RAM no longer holds the original start address. */
        if (emu->csave_snap_valid) {
            /* Snapshot layout matches the live buffer indexing. */
            start_addr  = (uint16_t)(emu->csave_header_snap[1] |
                                     (emu->csave_header_snap[2] << 8));
            end_addr    = (uint16_t)(emu->csave_header_snap[3] |
                                     (emu->csave_header_snap[4] << 8));
            header_auto = emu->csave_header_snap[5];
            header_type = emu->csave_header_snap[6];
        } else if (p->csave_header_buf) {
            uint16_t b = p->csave_header_buf;
            start_addr  = (uint16_t)(emu->memory.ram[b + 1] |
                                     (emu->memory.ram[b + 2] << 8));
            end_addr    = (uint16_t)(emu->memory.ram[b + 3] |
                                     (emu->memory.ram[b + 4] << 8));
            header_auto = emu->memory.ram[b + 5];
            header_type = emu->memory.ram[b + 6];
        } else {
            /* Legacy fallback : TXTTAB / VARTAB. */
            start_addr =
                (uint16_t)(emu->memory.ram[0x9A] | (emu->memory.ram[0x9B] << 8));
            end_addr =
                (uint16_t)(emu->memory.ram[0x9C] | (emu->memory.ram[0x9D] << 8));
            /* VARTAB points to first byte AFTER program → subtract 1. */
            if (end_addr > start_addr) end_addr--;
            header_auto = 0xC7;
            header_type = 0x00;
        }
        int prog_len = (int)end_addr - (int)start_addr + 1;
        if (prog_len < 0) prog_len = 0;

        /* Sanitize the filename. Prefer snapshot if valid (consistent with
         * the header source). */
        char clean_name[12] = {0};
        int ci = 0;
        for (int i = 0; i < 16 && ci < 11; i++) {
            unsigned char c;
            if (emu->csave_snap_valid) {
                c = (unsigned char)emu->csave_fname_snap[i];
            } else {
                uint16_t fn_addr =
                    p->csave_filename_buf ? p->csave_filename_buf : 0x0035;
                c = emu->memory.ram[fn_addr + i];
            }
            if (c == 0) break;
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-' || c == '.') {
                clean_name[ci++] = (char)c;
            }
        }
        if (ci == 0) snprintf(clean_name, sizeof(clean_name), "CSAVE");

        /* Build the canonical TAP. Single data block — Atmos BASIC's
         * CLOAD verify still prints "Errors found" cosmetically even
         * when data loads correctly (parity counters set by tape
         * input mock), but the program is fully loaded and auto-runs. */
        int tap_cap = 4 /*leader+sync*/ + 8 /*header*/ +
                      (int)strlen(clean_name) + 1 + prog_len;
        uint8_t* tap = (uint8_t*)malloc((size_t)tap_cap);
        if (tap) {
            int t = 0;
            tap[t++] = 0x16; tap[t++] = 0x16; tap[t++] = 0x16;
            tap[t++] = 0x24;
            tap[t++] = 0x00; tap[t++] = 0x00;                    /* 2 padding */
            tap[t++] = header_type;                              /* $00=BASIC, $80=array, $C0=string, ... */
            tap[t++] = header_auto;                              /* $C7 auto-run usually */
            tap[t++] = (uint8_t)(end_addr >> 8);
            tap[t++] = (uint8_t)(end_addr & 0xFF);
            tap[t++] = (uint8_t)(start_addr >> 8);
            tap[t++] = (uint8_t)(start_addr & 0xFF);
            tap[t++] = 0x00;                                     /* reserved */
            size_t nlen = strlen(clean_name);
            memcpy(tap + t, clean_name, nlen); t += (int)nlen;
            tap[t++] = 0x00;                                     /* name null */
            for (int i = 0; i < prog_len; i++) {
                tap[t++] = emu->memory.ram[start_addr + i];
            }

            /* Overwrite the host file with the proper TAP. */
            FILE* fw = fopen(emu->csave_last_path, "wb");
            if (fw) {
                fwrite(tap, 1, (size_t)t, fw);
                fclose(fw);
                log_info("CSAVE: built TAP %s (%d bytes, prog $%04X-$%04X)",
                         emu->csave_last_path, t, start_addr, end_addr);
            }

            /* Re-buffer for in-session CLOAD. */
            if (emu->tapebuf) free(emu->tapebuf);
            emu->tapebuf = (uint8_t*)malloc((size_t)t);
            if (emu->tapebuf) {
                memcpy(emu->tapebuf, tap, (size_t)t);
                emu->tapelen = t;
                emu->tapeoffs = 0;
                emu->tape_loaded = true;
                emu->tape_syncstack = -1;
                log_info("CSAVE: re-buffered %d bytes for CLOAD", t);
            }

            /* Persist to SDIMG so the file survives a restart. */
            if (emu->has_loci && emu->loci.sdimg && t > 0) {
                char sd_name[16] = {0};
                int sci = 0;
                for (int i = 0; clean_name[i] && sci < 8 && clean_name[i] != '.'; i++) {
                    sd_name[sci++] = clean_name[i];
                }
                if (sci == 0) {
                    snprintf(sd_name, sizeof(sd_name), "CSAVE.TAP");
                } else {
                    sd_name[sci] = 0;
                    snprintf(sd_name + sci, sizeof(sd_name) - sci, ".TAP");
                }
                int fd = loci_sdimg_fopen_ex(
                    (loci_sdimg_t*)emu->loci.sdimg, sd_name, 1);
                if (fd >= 0) {
                    int written = 0;
                    while (written < t) {
                        int chunk = t - written;
                        if (chunk > 256) chunk = 256;
                        int bw = loci_sdimg_fwrite(
                            (loci_sdimg_t*)emu->loci.sdimg, fd,
                            tap + written, (uint16_t)chunk);
                        if (bw <= 0) break;
                        written += bw;
                    }
                    loci_sdimg_fclose((loci_sdimg_t*)emu->loci.sdimg, fd);
                    loci_sdimg_sync((loci_sdimg_t*)emu->loci.sdimg);
                    log_info("CSAVE: persisted %d bytes to SDIMG as %s",
                             written, sd_name);
                } else {
                    log_warning("CSAVE: SDIMG persist failed (errno=%d)", -fd);
                }
            }
            free(tap);
            /* 34at : invalidate snapshot so a subsequent csave_end hit that
             * shares the same PC (ORIC-1 $E80A is reached from CLOAD's exit
             * path too) does not re-rebuild from stale state. */
            emu->csave_snap_valid = false;
        } else {
            log_warning("CSAVE: OOM rebuilding TAP");
        }
    }
}

static void emulator_run(emulator_t* emu) {
    cpu_reset(&emu->cpu);

    log_info("Starting emulation at PC=$%04X", emu->cpu.PC);

    /* Sprint 35a — emit the IPC ready banner once everything is wired up
     * so the client knows the channel is live. */
    if (emu->control_mode) {
        control_emit_ready(emu);
    }

    /* Sprint 36a — start wall clock for --bench. CLOCK_MONOTONIC is what
     * we want : insensitive to NTP / RTC adjustments. */
    struct timespec bench_t0 = {0};
    if (emu->bench_mode) {
        clock_gettime(CLOCK_MONOTONIC, &bench_t0);
    }

    uint64_t total_executed = 0;
    uint64_t frame_count = 0;
    bool screenshot_at_done = false;

#ifdef HAS_SDL2
    uint32_t frame_start_ticks = SDL_GetTicks();
#endif

    while (emu->running && g_running) {
#ifdef HAS_SDL2
        frame_start_ticks = SDL_GetTicks();
#endif
        /* Execute one frame worth of CPU cycles */
        int frame_cycles = 0;
        int rendered_scanlines = 0;
        bool vsync_triggered = false;
        while (frame_cycles < CYCLES_PER_FRAME && !emu->cpu.halted) {
            /* Legacy single breakpoint (--breakpoint / -b) */
            if (emu->breakpoint >= 0 && emu->cpu.PC == (uint16_t)emu->breakpoint) {
                /* Promote to interactive debugger if available */
                emu->debugger.active = true;
            }

            /* Interactive debugger check */
            if (emu->debugger.active || debugger_should_break(&emu->debugger, emu)) {
                if (emu->control_mode) {
                    /* Pick the reason for the EVT stopped: async pause wins
                     * over CPU-side break causes; otherwise use the explicit
                     * last_break_reason populated by debugger_should_break
                     * (sprint 35b). Fallback "break" for the first entry
                     * when active was set pre-loop. */
                    const char* reason =
                        emu->control_async_pause_pending ? "user" :
                        emu->debugger.last_break_reason[0]
                            ? emu->debugger.last_break_reason
                            : "break";
                    emu->control_async_pause_pending = false;
                    /* Reset step_mode so the next `continue` doesn't fire
                     * stepping; control_repl will set it again on a `step`
                     * command. */
                    emu->debugger.step_mode = false;
                    control_emit_stopped(emu, reason);
                    /* Clear last_break_reason after emitting so the next
                     * stop starts fresh. */
                    emu->debugger.last_break_reason[0] = '\0';
                    control_repl(emu);
                } else if (emu->tui_mode) {
                    tui_repl(emu);
                } else {
                    debugger_repl(&emu->debugger, emu);
                }
                if (!emu->running) break;
            }

            /* CPU trace logging (before step, captures pre-execution state) */
            trace_log_instruction(&emu->trace, &emu->cpu);

            /* CPU profiler (record address and opcode before step) */
            profiler_record_instruction(&emu->profiler, &emu->cpu);
            uint16_t prof_pc = emu->cpu.PC;

            tape_patches(emu);

            int step = cpu_step(&emu->cpu);
            frame_cycles += step;
            emu->frame_cycles = frame_cycles;   /* expose for raster bps */

            /* Post-CLOAD BASIC rechain: the ORIC ROM does NOT rechain
             * line pointers after CLOAD. TAP files may have stale pointers
             * (e.g. TYRANN.TAP). Detect when the CLOAD data loop completes
             * (PC hits cload_data_rts after readbyte was active) and fix
             * all next-line pointers so GOTO/GOSUB can traverse the chain. */
            if (emu->tape_readbyte_active && emu->rom_patches &&
                emu->cpu.PC == emu->rom_patches->cload_data_rts) {
                /* Only rechain BASIC programs (header file-type byte $00).
                 * Machine code loads ($80, $C0) must not be rechained —
                 * rechaining would overwrite program bytes with bogus
                 * next-line pointers (seen with Asteroids at $0500: 12
                 * "fixed" pointers corrupted the code, crashing the ROM
                 * 1.0 autorun JMP ($5F)). The type byte address is
                 * ROM-specific: $64 on ORIC-1, $02AE on Atmos. */
                if (emu->memory.ram[emu->rom_patches->tape_type_addr] == 0x00) {
                    basic_rechain(&emu->memory);
                }
                emu->tape_readbyte_active = false;
            }

            /* CPU profiler (record cycle cost after step) */
            profiler_record_cycles(&emu->profiler, prof_pc, step);

            /* Update VIA timers */
            via_update(&emu->via, step);

            /* Microdisc FDC: process delayed DRQ/INTRQ timers */
            if (emu->has_microdisc) {
                fdc_ticktock(&emu->microdisc.fdc, step);
            }

            /* LOCI WD1793 (Sprint 34aw) : tick the FDC backing LOCI's
             * 4 virtual DSK drives so DRQ/INTRQ timing is cycle-accurate. */
            if (emu->has_loci) {
                fdc_ticktock(&emu->loci.dsk_fdc, step);
            }

            /* ACIA 6551: serial TX/RX timing (aggregated, one call per instruction) */
            if (emu->has_serial) {
                acia_set_trace_cycle(&emu->acia, emu->cpu.cycles);
                acia_tick(&emu->acia, step);
            }

            /* Digitelec DTL 2000: PIA+ACIA TX/RX timing */
            if (emu->has_dtl2000) {
                dtl2000_tick(&emu->dtl2000, step);
            }

            /* NOTE: real Oric hardware does NOT expose VSync via VIA CB1.
             * VSync detection on a real Oric is done by polling memory
             * (ULA-driven counters), or by programming VIA Timer 1 in
             * continuous mode at the frame period (20 ms PAL). No VIA
             * signal is toggled here on purpose — Phosphoric stays faithful
             * to the hardware. */
            (void)vsync_triggered;

            /* Scanline-accurate ULA rendering: emit one scanline every
             * PAL_CYCLES_PER_LINE (64) CPU cycles. The visible area is
             * 224 lines (200 HIRES/TEXT + 24 bottom text rows). Lines
             * 224-311 are vertical blanking (not rendered). Mimics real
             * Oric ULA behavior where the electron beam paints in real
             * time as the CPU runs, so each scanline samples the memory
             * state at its precise emission cycle (e.g. mid-fill state
             * during HIRES init). */
            int target_scanline = frame_cycles / PAL_CYCLES_PER_LINE;
            while (rendered_scanlines < target_scanline && rendered_scanlines < 224) {
                video_render_scanline(&emu->video, emu->memory.ram, rendered_scanlines);
                rendered_scanlines++;
            }
        }

        /* Flush any remaining scanlines (e.g. if CPU halted mid-frame) */
        while (rendered_scanlines < 224) {
            video_render_scanline(&emu->video, emu->memory.ram, rendered_scanlines);
            rendered_scanlines++;
        }

        total_executed += (uint64_t)frame_cycles;

        /* Sprint 35a freeze — async pause: once per frame, peek at stdin.
         * If the IDE sent `pause`, hand control back to the REPL right
         * after this frame ends. Latency = at most one frame (~20 ms). */
        if (emu->control_mode && control_poll_pause(emu)) {
            emu->debugger.active = true;
        }

        /* Fast-load phase 1: inject TAP data into RAM as soon as the ROM
         * RAM test is done (~3M cycles). Injecting early ensures the binary
         * is in place when the BASIC READY prompt appears (~3.6M cycles) so
         * a user typing CALL/USR manually finds valid opcodes at start_addr.
         * The ROM init writes to its own zero-page/system area, not to
         * $0500+ where our binary goes, so no overwrite race. */
        if (emu->fastload_pending && total_executed > 3000000) {
            for (int i = 0; i < emu->fastload_size; i++) {
                memory_write(&emu->memory, (uint16_t)(emu->fastload_addr + i),
                             emu->fastload_buf[i]);
            }
            log_info("Deferred fast-load: injected %d bytes at $%04X-$%04X (after %llu cycles)",
                     emu->fastload_size, emu->fastload_addr,
                     emu->fastload_addr + emu->fastload_size - 1,
                     (unsigned long long)total_executed);

            if (emu->fastload_type == 0x00) {
                /* BASIC: rechain + VARTAB now (binary fully in RAM) */
                basic_rechain(&emu->memory);
                uint16_t vartab = emu->fastload_end + 1;
                memory_write(&emu->memory, 0x9C, (uint8_t)(vartab & 0xFF));
                memory_write(&emu->memory, 0x9D, (uint8_t)(vartab >> 8));
                memory_write(&emu->memory, 0x9E, (uint8_t)(vartab & 0xFF));
                memory_write(&emu->memory, 0x9F, (uint8_t)(vartab >> 8));
                memory_write(&emu->memory, 0xA0, (uint8_t)(vartab & 0xFF));
                memory_write(&emu->memory, 0xA1, (uint8_t)(vartab >> 8));
                log_info("BASIC: VARTAB=$%04X", vartab);
            }

            /* Phase 2 (auto-exec / auto-RUN) is fired later from a separate
             * block, once the ROM has finished its full init and reached the
             * READY idle loop — at that point VIA PCR/IER/IFR and ULA are
             * fully configured, so machine-code binaries don't inherit a
             * half-initialized I/O state. */
            emu->fastload_autoexec_pending = true;
            free(emu->fastload_buf);
            emu->fastload_buf = NULL;
            emu->fastload_pending = false;
        }

        /* Fast-load phase 2: fire auto-exec / auto-RUN once VIA + ULA are
         * stable (~5M cycles, ROM in READY idle loop). Cf. rapport
         * docs/phosphoric-autorun-timing.md de l'équipe Asteroids. */
        if (emu->fastload_autoexec_pending && total_executed > 5000000) {
            if (emu->fastload_type == 0x00 && !emu->type_keys_text) {
                emu->type_keys_text = "RUN\\n";
                emu->type_keys_at = (int64_t)total_executed + CYCLES_PER_FRAME * 10;
                emu->type_keys_idx = 0;
                emu->type_keys_next_cycle = emu->type_keys_at;
                emu->type_keys_done = false;
                emu->type_keys_last_char = 0;
                log_info("Auto-typing RUN after fast-load (phase 2)");
            } else if (emu->fastload_type == 0x80 &&
                       (emu->fastload_auto_run & 0x80)) {
                emu->cpu.PC = emu->fastload_addr;
                log_info("Auto-exec machine code at $%04X (auto-run flag=$%02X, phase 2)",
                         emu->fastload_addr, emu->fastload_auto_run);
            }
            emu->fastload_autoexec_pending = false;
        }

        /* Auto-CLOAD: when a tape was provided without -f, the BASIC prompt
         * is now ready (RAM test done) — auto-type CLOAD"" so the ROM CLOAD
         * routine runs and triggers the on-tape auto-run flag normally.
         * Only fires once; user can override by setting --type-keys. */
        if (emu->tape_auto_cload_pending && total_executed > 5000000 &&
            !emu->type_keys_text) {
            emu->type_keys_text = "CLOAD\"\"\\n";
            emu->type_keys_at = (int64_t)total_executed + CYCLES_PER_FRAME * 10;
            emu->type_keys_idx = 0;
            emu->type_keys_next_cycle = emu->type_keys_at;
            emu->type_keys_done = false;
            emu->type_keys_last_char = 0;
            emu->tape_auto_cload_pending = false;
            log_info("Auto-typing CLOAD\"\" for inserted tape");
        }

        /* Auto-type: inject keystrokes at specified cycle count.
         * Each key is pressed for ~2 frames (40ms) then released for ~2 frames.
         * This simulates realistic typing speed for the ROM keyboard scanner. */
        if (emu->type_keys_text && !emu->type_keys_done &&
            (int64_t)total_executed >= emu->type_keys_at) {
            if ((int64_t)total_executed >= emu->type_keys_next_cycle) {
                int idx = emu->type_keys_idx;
                char c = emu->type_keys_text[idx];
                /* Sprint 34av : LOCI HID injection path. Each char/escape
                 * yields a HID usage code that's pushed into the LOCI kbd
                 * bitmap for ~2 frames, then released. */
                if (emu->type_keys_loci_hid && emu->has_loci) {
                    if (c == '\0') {
                        loci_kbd_clear(&emu->loci);
                        emu->type_keys_done = true;
                    } else if (emu->type_keys_debounce > 0) {
                        loci_kbd_clear(&emu->loci);
                        emu->type_keys_debounce--;
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME;
                    } else if (c == '\\') {
                        /* Escape : \n \e \u \d \l \r \pN */
                        char esc = emu->type_keys_text[idx+1];
                        uint8_t hid = 0;
                        switch (esc) {
                            case 'n': hid = 0x28; break;  /* Enter */
                            case 'e': hid = 0x29; break;  /* Escape */
                            case 'u': hid = 0x52; break;  /* Up */
                            case 'd': hid = 0x51; break;  /* Down */
                            case 'l': hid = 0x50; break;  /* Left */
                            case 'r': hid = 0x4F; break;  /* Right */
                            case 'p': {
                                int secs = emu->type_keys_text[idx+2] - '0';
                                if (secs < 1) secs = 1;
                                if (secs > 9) secs = 9;
                                loci_kbd_clear(&emu->loci);
                                emu->type_keys_idx += 3;
                                emu->type_keys_next_cycle = (int64_t)total_executed + ORIC_CLOCK_HZ * secs;
                                goto type_keys_done_frame;
                            }
                            default: hid = 0; break;
                        }
                        if (hid) {
                            uint8_t keys[6] = { hid, 0, 0, 0, 0, 0 };
                            loci_kbd_set_report(&emu->loci, 0, keys);
                            emu->type_keys_last_char = esc;
                            emu->type_keys_idx += 2;
                            emu->type_keys_debounce = 2;  /* release for 2 frames after */
                            emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME * 2;
                        } else {
                            emu->type_keys_idx += 2;  /* unknown escape — skip */
                        }
                    } else {
                        /* Regular char → HID code */
                        uint8_t hid = 0;
                        char lc = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
                        if (lc >= 'a' && lc <= 'z') hid = (uint8_t)(0x04 + (lc - 'a'));
                        else if (lc == '0') hid = 0x27;
                        else if (lc >= '1' && lc <= '9') hid = (uint8_t)(0x1E + (lc - '1'));
                        else if (lc == ' ') hid = 0x2C;
                        if (hid) {
                            if (c == emu->type_keys_last_char) {
                                /* Same char twice : release first */
                                loci_kbd_clear(&emu->loci);
                                emu->type_keys_debounce = 1;
                                emu->type_keys_last_char = 0;
                                emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME;
                            } else {
                                uint8_t mod = (c >= 'A' && c <= 'Z') ? 0x02 : 0;  /* L-Shift */
                                uint8_t keys[6] = { hid, 0, 0, 0, 0, 0 };
                                loci_kbd_set_report(&emu->loci, mod, keys);
                                emu->type_keys_last_char = c;
                                emu->type_keys_idx++;
                                emu->type_keys_debounce = 2;
                                emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME * 2;
                            }
                        } else {
                            emu->type_keys_idx++;  /* unknown char — skip */
                        }
                    }
                    type_keys_done_frame:;
                } else
                if (c == '\0') {
                    /* Done typing */
                    oric_keyboard_release_all(&emu->keyboard);
                    emu->type_keys_done = true;
                } else if (c == '\\' && emu->type_keys_text[idx+1] == 'n') {
                    /* \n = RETURN. Si deux \n consécutifs, insert un frame de
                     * relâche entre les deux : le scanner ROM voit sinon une
                     * pression longue unique au lieu de deux RETURN distincts.
                     * On réutilise last_char sans toucher à type_keys_debounce
                     * (qui est réservé au branch caractère ordinaire). */
                    if (emu->type_keys_last_char == '\n') {
                        oric_keyboard_release_all(&emu->keyboard);
                        emu->type_keys_last_char = 0;
                        /* idx non avancé : on re-traitera ce \n au prochain frame */
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME;
                    } else {
                        oric_keyboard_release_all(&emu->keyboard);
                        oric_keyboard_press_char(&emu->keyboard, '\n');
                        emu->type_keys_last_char = '\n';
                        emu->type_keys_idx += 2;
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME * 4;
                    }
                } else if (c == '\\' && emu->type_keys_text[idx+1] == 'e') {
                    /* Sprint 34av : \e = ESC. Touche utile pour le TUI LOCI. */
                    if (emu->type_keys_last_char == 0x1B) {
                        oric_keyboard_release_all(&emu->keyboard);
                        emu->type_keys_last_char = 0;
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME;
                    } else {
                        oric_keyboard_release_all(&emu->keyboard);
                        oric_keyboard_press_char(&emu->keyboard, 0x1B);
                        emu->type_keys_last_char = 0x1B;
                        emu->type_keys_idx += 2;
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME * 4;
                    }
                } else if (c == '\\' && (emu->type_keys_text[idx+1] == 'u' ||
                                          emu->type_keys_text[idx+1] == 'd' ||
                                          emu->type_keys_text[idx+1] == 'l' ||
                                          emu->type_keys_text[idx+1] == 'r')) {
                    /* Sprint 34av : flèches pour navigation TUI LOCI. */
                    char dir = emu->type_keys_text[idx+1];
                    char arrow = (dir == 'u') ? (char)0x80
                              : (dir == 'd') ? (char)0x81
                              : (dir == 'l') ? (char)0x82
                              : (char)0x83;  /* r */
                    if (emu->type_keys_last_char == arrow) {
                        oric_keyboard_release_all(&emu->keyboard);
                        emu->type_keys_last_char = 0;
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME;
                    } else {
                        oric_keyboard_release_all(&emu->keyboard);
                        oric_keyboard_press_char(&emu->keyboard, arrow);
                        emu->type_keys_last_char = arrow;
                        emu->type_keys_idx += 2;
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME * 4;
                    }
                } else if (c == '\\' && emu->type_keys_text[idx+1] == 'p') {
                    /* \pN = pause N seconds (N = single digit) */
                    int secs = emu->type_keys_text[idx+2] - '0';
                    if (secs < 1) secs = 1;
                    if (secs > 9) secs = 9;
                    oric_keyboard_release_all(&emu->keyboard);
                    emu->type_keys_idx += 3;
                    emu->type_keys_next_cycle = (int64_t)total_executed + ORIC_CLOCK_HZ * secs;
                } else {
                    /* Regular character */
                    if (emu->type_keys_debounce > 0) {
                        /* Debounce phase: release all keys and wait */
                        oric_keyboard_release_all(&emu->keyboard);
                        emu->type_keys_debounce--;
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME;
                    } else if (c == emu->type_keys_last_char) {
                        /* Same char as previous: insert release phase */
                        oric_keyboard_release_all(&emu->keyboard);
                        emu->type_keys_debounce = 1; /* 1 more frame of release */
                        emu->type_keys_last_char = 0;
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME;
                    } else {
                        /* New character: press immediately */
                        oric_keyboard_release_all(&emu->keyboard);
                        oric_keyboard_press_char(&emu->keyboard, c);
                        emu->type_keys_last_char = c;
                        emu->type_keys_idx++;
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME * 4;
                    }
                }
            }
        }

        /* Flush serial trace once per frame (not per byte) */
        if (emu->has_serial) {
            acia_trace_flush(&emu->acia);
        }

        /* Video frame already rendered scanline-by-scanline above
         * (interleaved with CPU cycles, ULA-accurate timing). No final
         * pass needed; the framebuffer reflects the per-scanline memory
         * snapshots. */

        /* Push frame to cast server if active */
        if (emu->has_cast_server) {
            cast_server_push_frame(&emu->cast_server, emu->video.framebuffer,
                                   ORIC_SCREEN_W, ORIC_SCREEN_H);
        }

        /* Present to screen and handle events if not headless */
        if (!emu->headless) {
            renderer_present(&emu->video);
#ifdef HAS_SDL2
            /* Poll SDL events (keyboard, window close, etc.) */
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_QUIT:
                    emu->running = false;
                    break;
                case SDL_KEYDOWN:
                    /* F5 = Reset, F10 = Quit, F11 = Fullscreen, F12 = Screenshot */
                    switch (event.key.keysym.sym) {
                    case SDLK_F2:
                        if (savestate_save(emu, "oric1_quicksave.ost")) {
                            log_info("Quick save state saved (F2)");
                        } else {
                            log_error("Quick save state failed (F2)");
                        }
                        break;
                    case SDLK_F3:
                        renderer_cycle_scale();
                        log_info("Display scale: x%d", renderer_get_scale());
                        break;
                    case SDLK_F4:
                        if (savestate_load(emu, "oric1_quicksave.ost")) {
                            log_info("Quick save state loaded (F4)");
                        } else {
                            log_error("Quick save state load failed (F4)");
                        }
                        break;
                    case SDLK_F5:
                        cpu_reset(&emu->cpu);
                        if (emu->has_loci) {
                            /* Sprint 34aj: LOCI reset button — clears MIA
                             * state (regs/xstack/active_op) but keeps the
                             * mount table and open file handles so the
                             * user's drives stay attached. Equivalent to
                             * the Pi Pico reset on real LOCI hardware. */
                            loci_reset(&emu->loci);
                            log_info("LOCI: MIA state reset (mounts preserved)");
                        }
                        break;
                    case SDLK_F8:
                        /* Sprint 34ai: LOCI Action button (warm short press).
                         * Installs the IRQ trap and triggers an interrupt so
                         * the LOCI ROM can take over (save-state menu, etc.).
                         * Release on KEYUP below. */
                        if (emu->has_loci) {
                            loci_action_button_short(&emu->loci);
                            log_info("LOCI: Action button pressed (F8)");
                        }
                        break;
                    case SDLK_F7: {
                        /* Memory dump: save 64KB RAM to timestamped file */
                        time_t now = time(NULL);
                        struct tm* tm = localtime(&now);
                        char dumpname[64];
                        snprintf(dumpname, sizeof(dumpname),
                                 "memdump_%04d%02d%02d_%02d%02d%02d.bin",
                                 tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
                                 tm->tm_hour, tm->tm_min, tm->tm_sec);
                        FILE* df = fopen(dumpname, "wb");
                        if (df) {
                            fwrite(emu->memory.ram, 1, sizeof(emu->memory.ram), df);
                            /* $C000-$FFFF : vue CPU bankée (même contrat 64 Ko
                             * que --dump-ram-at, cf. sprint 38) */
                            for (uint32_t a = 0xC000; a <= 0xFFFF; a++) {
                                uint8_t b = memory_read(&emu->memory, (uint16_t)a);
                                fwrite(&b, 1, 1, df);
                            }
                            fclose(df);
                            log_info("Memory dump: %s (64KB, $C000-$FFFF = CPU view, PC=$%04X, cycle=%llu)",
                                     dumpname, emu->cpu.PC,
                                     (unsigned long long)total_executed);
                        }
                        break;
                    }
                    case SDLK_F9:
                        /* Enter interactive debugger */
                        emu->debugger.active = true;
                        break;
                    case SDLK_F10:
                        emu->running = false;
                        break;
                    case SDLK_F11:
                        renderer_toggle_fullscreen();
                        break;
                    case SDLK_F12:
                        video_export_ppm(&emu->video, "screenshot.ppm");
                        log_info("Screenshot saved to screenshot.ppm");
                        break;
                    default:
                        break;
                    }
                    /* Fall through to keyboard/joystick handler */
                    if (!oric_joystick_handle_sdl_event(&emu->joystick, &event)) {
                        oric_keyboard_handle_sdl_event(&emu->keyboard, &event);
                    }
                    /* Sprint 34ak: mirror SDL keyboard state into the
                     * LOCI kbd bitmap so the LOCI ROM TUI can navigate. */
                    loci_sync_kbd_from_sdl(emu);
                    break;
                case SDL_KEYUP:
                    if (event.key.keysym.sym == SDLK_F8 && emu->has_loci) {
                        /* Sprint 34ai: Action button release sets V flag
                         * so the BVC spin exits and JMP ($FFFA) runs the
                         * save-state handler. */
                        loci_action_button_release(&emu->loci);
                        log_info("LOCI: Action button released (F8)");
                    }
                    if (!oric_joystick_handle_sdl_event(&emu->joystick, &event)) {
                        oric_keyboard_handle_sdl_event(&emu->keyboard, &event);
                    }
                    /* Sprint 34ak: sync after KEYUP so released keys
                     * disappear from the LOCI bitmap. */
                    loci_sync_kbd_from_sdl(emu);
                    break;
                case SDL_TEXTINPUT:
                    /* Symbolic mode: character -> ORIC key mapping */
                    oric_keyboard_handle_sdl_event(&emu->keyboard, &event);
                    break;
                /* Sprint 34al: bridge SDL mouse → LOCI mou_xram. */
                case SDL_MOUSEMOTION:
                    if (emu->has_loci) {
                        uint32_t bs = SDL_GetMouseState(NULL, NULL);
                        uint8_t btn = 0;
                        if (bs & SDL_BUTTON(SDL_BUTTON_LEFT))   btn |= 0x01;
                        if (bs & SDL_BUTTON(SDL_BUTTON_RIGHT))  btn |= 0x02;
                        if (bs & SDL_BUTTON(SDL_BUTTON_MIDDLE)) btn |= 0x04;
                        loci_mou_report(&emu->loci, btn,
                                        (int8_t)event.motion.xrel,
                                        (int8_t)event.motion.yrel,
                                        0, 0);
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                    if (emu->has_loci) {
                        uint32_t bs = SDL_GetMouseState(NULL, NULL);
                        uint8_t btn = 0;
                        if (bs & SDL_BUTTON(SDL_BUTTON_LEFT))   btn |= 0x01;
                        if (bs & SDL_BUTTON(SDL_BUTTON_RIGHT))  btn |= 0x02;
                        if (bs & SDL_BUTTON(SDL_BUTTON_MIDDLE)) btn |= 0x04;
                        loci_mou_report(&emu->loci, btn, 0, 0, 0, 0);
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    if (emu->has_loci) {
                        loci_mou_report(&emu->loci, 0, 0, 0,
                                        (int8_t)event.wheel.y,
                                        (int8_t)event.wheel.x);
                    }
                    break;
                /* SDL game controller / joystick events */
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP:
                case SDL_CONTROLLERAXISMOTION:
                case SDL_JOYHATMOTION:
                case SDL_JOYBUTTONDOWN:
                case SDL_JOYBUTTONUP:
                    oric_joystick_handle_sdl_event(&emu->joystick, &event);
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    if (emu->joystick.mode == ORIC_JOY_SDL_GAMEPAD &&
                        emu->joystick.controller == NULL &&
                        emu->joystick.joystick == NULL) {
                        oric_joystick_open_sdl(&emu->joystick, event.cdevice.which);
                    }
                    break;
                default:
                    break;
                }
            }
#endif
        }

        /* Screenshot at specific cycle count */
        if (!screenshot_at_done && emu->screenshot_at_cycles >= 0 &&
            (int64_t)total_executed >= emu->screenshot_at_cycles) {
            log_info("Taking screenshot at %llu cycles -> %s",
                     (unsigned long long)total_executed, emu->screenshot_at_file);
            video_export_auto(&emu->video, emu->screenshot_at_file);
            screenshot_at_done = true;
        }

        /* Frame dump */
        if (emu->frame_dump_dir && (frame_count % (uint64_t)emu->frame_dump_interval == 0)) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%06llu.ppm",
                     emu->frame_dump_dir, (unsigned long long)frame_count);
            video_export_ppm(&emu->video, path);
        }

        frame_count++;

        /* RAM dump at cycle: write 64KB once when threshold reached */
        if (!emu->dump_ram_at_done && emu->dump_ram_at_cycles >= 0 &&
            (int64_t)total_executed >= emu->dump_ram_at_cycles) {
            FILE* rf = fopen(emu->dump_ram_at_file, "wb");
            if (rf) {
                fwrite(emu->memory.ram, 1, sizeof(emu->memory.ram), rf);
                /* $C000-$FFFF : vue CPU (banking BASIC ROM / overlay /
                 * upper RAM). memory_read est sans effet de bord hors
                 * page I/O ($0300-$03FF), jamais atteinte ici. */
                for (uint32_t a = 0xC000; a <= 0xFFFF; a++) {
                    uint8_t b = memory_read(&emu->memory, (uint16_t)a);
                    fwrite(&b, 1, 1, rf);
                }
                fclose(rf);
                log_info("RAM dump (64KB, $C000-$FFFF = CPU view) at %llu cycles → %s",
                         (unsigned long long)total_executed, emu->dump_ram_at_file);
            } else {
                log_error("Cannot open RAM dump file: %s", emu->dump_ram_at_file);
            }
            emu->dump_ram_at_done = true;
        }

#ifdef HAS_SDL2
        /* Frame limiter: 50 Hz PAL = 20ms per frame.
         * Without this, the emulator runs at monitor refresh rate (60 Hz+)
         * which is 20% faster than real ORIC hardware.
         * SDL_Delay has ~1ms resolution, good enough for frame pacing. */
        if (!emu->headless) {
            uint32_t frame_elapsed = SDL_GetTicks() - frame_start_ticks;
            if (frame_elapsed < 20) {
                SDL_Delay(20 - frame_elapsed);
            }
        }
#endif

        /* Check cycle limit for headless/test mode */
        if (emu->max_cycles >= 0 && (int64_t)total_executed >= emu->max_cycles) {
            log_info("Cycle limit reached (%lld cycles)", (long long)emu->max_cycles);
            if (emu->control_mode) control_emit_halt(emu, "cycle_limit");
            break;
        }

        if (emu->cpu.halted) {
            log_info("CPU halted after %llu cycles", (unsigned long long)total_executed);
            if (emu->control_mode) control_emit_halt(emu, "jam");
            break;
        }
    }

    /* End-of-run screenshot */
    if (emu->screenshot_file) {
        log_info("Taking exit screenshot -> %s", emu->screenshot_file);
        video_render_frame(&emu->video, emu->memory.ram);
        video_export_auto(&emu->video, emu->screenshot_file);
    }

    log_info("Emulation stopped. Total cycles: %llu, frames: %llu",
             (unsigned long long)total_executed, (unsigned long long)frame_count);

    char state[128];
    cpu_get_state_string(&emu->cpu, state, sizeof(state));
    log_info("Final CPU state: %s", state);

    /* Sprint 36a — single-line bench report on stdout. Easy to grep from
     * scripts. ORIC clock is 1 MHz, so `mhz_eq` of 1.0 = real-time,
     * 50.0 = 50x real-time. */
    if (emu->bench_mode) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double wall_s = (double)(t1.tv_sec - bench_t0.tv_sec)
                      + (double)(t1.tv_nsec - bench_t0.tv_nsec) * 1e-9;
        if (wall_s <= 0.0) wall_s = 1e-9;
        double mhz_eq = (double)total_executed / (wall_s * 1e6);
        double speed_ratio = mhz_eq / 1.0;   /* ORIC is 1 MHz */
        double frame_us = wall_s * 1e6 / (frame_count > 0 ? (double)frame_count : 1.0);
        printf("BENCH cycles=%llu frames=%llu wall_ms=%.3f mhz_eq=%.2f "
               "speed_ratio=%.1fx frame_us=%.1f\n",
               (unsigned long long)total_executed,
               (unsigned long long)frame_count,
               wall_s * 1000.0, mhz_eq, speed_ratio, frame_us);
        fflush(stdout);
    }
}

int main(int argc, char* argv[]) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    emu.breakpoint = -1;

    const char* tape_file = NULL;
    const char* disk_files[MICRODISC_MAX_DRIVES] = {NULL, NULL, NULL, NULL};
    const char* rom_file = NULL;
    const char* hostfs_path = NULL;
    bool fast_load = false;
    bool verbose = false;
    bool headless = false;
    int64_t max_cycles = -1;
    const char* screenshot_file = NULL;
    const char* screenshot_at_arg = NULL;
    const char* frame_dump_dir = NULL;
    int frame_dump_interval = 50;
    const char* keyboard_layout = NULL;

    const char* type_keys_arg = NULL;
    const char* disk_rom_file = NULL;
    bool debug_mode = false;
    const char* debug_break_addr = NULL;
    bool cast_server_enabled = false;
    uint16_t cast_server_port = 0;
    bool cast_discover = false;
    bool cast_to_enabled = false;
    const char* cast_to_device = NULL;
    const char* save_state_file = NULL;
    const char* load_state_file = NULL;
    const char* model_arg = NULL;
    const char* joystick_mode = NULL;
    const char* printer_file = NULL;
    const char* printer_type_arg = NULL;
    int scale_factor = 3;
    const char* trace_file = NULL;
    const char* dump_ram_at_arg = NULL;
    const char* trace_irq_file = NULL;
    const char* symbols_file = NULL;
    bool tui_mode = false;
    bool control_mode = false;
    bool bench_mode = false;
    bool loci_enabled = false;
    const char* loci_flash_root = NULL;
    const char* loci_sdimg_path = NULL;
    int64_t trace_max = 0;
    const char* profile_file = NULL;
    const char* rom_info_file = NULL;
    bool rom_info_enabled = false;
    const char* serial_arg = NULL;
    const char* acia_addr_arg = NULL;
    const char* dtl2000_arg = NULL;
    const char* dtl2000_addr_arg = NULL;
    bool serial_v23 = false;
    int serial_buffer_size = 0;
    bool serial_irq_on_rdrf = false;
    const char* serial_trace_file = NULL;
    /* Long option codes for options without short equivalents */
    enum { OPT_SCREENSHOT = 256, OPT_SCREENSHOT_AT, OPT_FRAME_DUMP, OPT_FRAME_DUMP_INTERVAL, OPT_TYPE_KEYS, OPT_DISK_ROM, OPT_DISK1, OPT_DISK2, OPT_DISK3, OPT_BREAKPOINT, OPT_DEBUG_BREAK, OPT_CAST_SERVER, OPT_CAST_DISCOVER, OPT_CAST_TO, OPT_SAVE_STATE, OPT_LOAD_STATE, OPT_MODEL, OPT_JOYSTICK, OPT_PRINTER, OPT_PRINTER_TYPE, OPT_SCALE, OPT_TRACE, OPT_TRACE_MAX, OPT_PROFILE, OPT_ROM_INFO, OPT_SERIAL, OPT_SERIAL_V23, OPT_ACIA_ADDR, OPT_SERIAL_BUFFER, OPT_SERIAL_IRQ_RDRF, OPT_SERIAL_TRACE, OPT_DTL2000, OPT_DTL2000_ADDR, OPT_DUMP_RAM_AT, OPT_TRACE_IRQ, OPT_SYMBOLS, OPT_TUI, OPT_LOCI, OPT_LOCI_FLASH, OPT_LOCI_SDIMG, OPT_CONTROL, OPT_BENCH };

    static struct option long_options[] = {
        {"tape",                required_argument, 0, 't'},
        {"disk",                required_argument, 0, 'd'},
        {"disk1",               required_argument, 0, OPT_DISK1},
        {"disk2",               required_argument, 0, OPT_DISK2},
        {"disk3",               required_argument, 0, OPT_DISK3},
        {"rom",                 required_argument, 0, 'r'},
        {"hostfs",              required_argument, 0, 'h'},
        {"fast-load",           no_argument,       0, 'f'},
        {"headless",            no_argument,       0, 'n'},
        {"cycles",              required_argument, 0, 'c'},
        {"verbose",             no_argument,       0, 'v'},
        {"screenshot",          required_argument, 0, OPT_SCREENSHOT},
        {"screenshot-at",       required_argument, 0, OPT_SCREENSHOT_AT},
        {"frame-dump",          required_argument, 0, OPT_FRAME_DUMP},
        {"frame-dump-interval", required_argument, 0, OPT_FRAME_DUMP_INTERVAL},
        {"keyboard",            required_argument, 0, 'k'},
        {"type-keys",           required_argument, 0, OPT_TYPE_KEYS},
        {"disk-rom",            required_argument, 0, OPT_DISK_ROM},
        {"breakpoint",          required_argument, 0, 'b'},
        {"debug",               no_argument,       0, 'D'},
        {"break",               required_argument, 0, OPT_DEBUG_BREAK},
        {"cast-server",         optional_argument, 0, OPT_CAST_SERVER},
        {"cast-to",             optional_argument, 0, OPT_CAST_TO},
        {"cast-discover",       no_argument,       0, OPT_CAST_DISCOVER},
        {"save-state",          required_argument, 0, OPT_SAVE_STATE},
        {"load-state",          required_argument, 0, OPT_LOAD_STATE},
        {"model",               required_argument, 0, 'm'},
        {"joystick",            required_argument, 0, 'j'},
        {"printer",             required_argument, 0, 'p'},
        {"printer-type",        required_argument, 0, OPT_PRINTER_TYPE},
        {"scale",               required_argument, 0, OPT_SCALE},
        {"trace",               required_argument, 0, OPT_TRACE},
        {"trace-max",           required_argument, 0, OPT_TRACE_MAX},
        {"profile",             required_argument, 0, OPT_PROFILE},
        {"rom-info",            optional_argument, 0, OPT_ROM_INFO},
        {"serial",              required_argument, 0, OPT_SERIAL},
        {"serial-v23",          no_argument,       0, OPT_SERIAL_V23},
        {"serial-buffer",       required_argument, 0, OPT_SERIAL_BUFFER},
        {"serial-irq-on-rdrf",  no_argument,       0, OPT_SERIAL_IRQ_RDRF},
        {"serial-trace",        required_argument, 0, OPT_SERIAL_TRACE},
        {"acia-addr",           required_argument, 0, OPT_ACIA_ADDR},
        {"dtl2000",             required_argument, 0, OPT_DTL2000},
        {"dtl2000-addr",        required_argument, 0, OPT_DTL2000_ADDR},
        {"dump-ram-at",         required_argument, 0, OPT_DUMP_RAM_AT},
        {"trace-irq",           required_argument, 0, OPT_TRACE_IRQ},
        {"symbols",             required_argument, 0, OPT_SYMBOLS},
        {"tui",                 no_argument,       0, OPT_TUI},
        {"loci",                no_argument,       0, OPT_LOCI},
        {"loci-flash",          required_argument, 0, OPT_LOCI_FLASH},
        {"loci-sdimg",          required_argument, 0, OPT_LOCI_SDIMG},
        {"control",             no_argument,       0, OPT_CONTROL},
        {"bench",               no_argument,       0, OPT_BENCH},
        {"help",                no_argument,       0, '?'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "t:d:r:h:fnc:vm:k:j:p:b:D?", long_options, &option_index)) != -1) {
        switch (opt) {
            case 't': tape_file = optarg; break;
            case 'd': disk_files[0] = optarg; break;
            case OPT_DISK1: disk_files[1] = optarg; break;
            case OPT_DISK2: disk_files[2] = optarg; break;
            case OPT_DISK3: disk_files[3] = optarg; break;
            case 'r': rom_file = optarg; break;
            case 'h': hostfs_path = optarg; break;
            case 'f': fast_load = true; break;
            case 'n': headless = true; break;
            case 'c': max_cycles = atoll(optarg); break;
            case 'v': verbose = true; break;
            case OPT_SCREENSHOT: screenshot_file = optarg; break;
            case OPT_SCREENSHOT_AT: screenshot_at_arg = optarg; break;
            case OPT_FRAME_DUMP: frame_dump_dir = optarg; break;
            case OPT_FRAME_DUMP_INTERVAL: frame_dump_interval = atoi(optarg); break;
            case 'k': keyboard_layout = optarg; break;
            case OPT_TYPE_KEYS: type_keys_arg = optarg; break;
            case OPT_DISK_ROM: disk_rom_file = optarg; break;
            case 'b': emu.breakpoint = (int32_t)strtol(optarg, NULL, 16); break;
            case 'D': debug_mode = true; break;
            case OPT_DEBUG_BREAK: debug_break_addr = optarg; break;
            case OPT_CAST_SERVER:
                cast_server_enabled = true;
                if (optarg) cast_server_port = (uint16_t)atoi(optarg);
                break;
            case OPT_CAST_TO:
                cast_to_enabled = true;
                if (optarg) cast_to_device = optarg;
                break;
            case OPT_CAST_DISCOVER: cast_discover = true; break;
            case OPT_SAVE_STATE: save_state_file = optarg; break;
            case OPT_LOAD_STATE: load_state_file = optarg; break;
            case 'm': model_arg = optarg; break;
            case 'j': joystick_mode = optarg; break;
            case 'p': printer_file = optarg; break;
            case OPT_PRINTER_TYPE: printer_type_arg = optarg; break;
            case OPT_SCALE:
                scale_factor = atoi(optarg);
                if (scale_factor < 1 || scale_factor > 4) {
                    fprintf(stderr, "Invalid scale factor: %s (must be 1-4)\n", optarg);
                    return 1;
                }
                break;
            case OPT_TRACE: trace_file = optarg; break;
            case OPT_TRACE_MAX: trace_max = atoll(optarg); break;
            case OPT_PROFILE: profile_file = optarg; break;
            case OPT_ROM_INFO:
                rom_info_enabled = true;
                if (optarg) rom_info_file = optarg;
                break;
            case OPT_SERIAL:
                serial_arg = optarg;
                break;
            case OPT_SERIAL_V23:
                serial_v23 = true;
                break;
            case OPT_SERIAL_BUFFER:
                serial_buffer_size = atoi(optarg);
                break;
            case OPT_SERIAL_IRQ_RDRF:
                serial_irq_on_rdrf = true;
                break;
            case OPT_SERIAL_TRACE:
                serial_trace_file = optarg;
                break;
            case OPT_DUMP_RAM_AT: dump_ram_at_arg = optarg; break;
            case OPT_TRACE_IRQ: trace_irq_file = optarg; break;
            case OPT_SYMBOLS: symbols_file = optarg; break;
            case OPT_TUI: tui_mode = true; debug_mode = true; break;
            case OPT_CONTROL:
                control_mode = true;
                debug_mode = true;
                headless = true;
                /* Redirect logs to stderr as early as possible so the
                 * init banner doesn't pollute the protocol channel. */
                log_set_stream(stderr);
                break;
            case OPT_BENCH:
                bench_mode = true;
                headless = true;
                /* Logs to stderr so the single-line BENCH report on
                 * stdout is easy to grep / pipe / parse. */
                log_set_stream(stderr);
                break;
            case OPT_LOCI: loci_enabled = true; break;
            case OPT_LOCI_FLASH: loci_flash_root = optarg; loci_enabled = true; break;
            case OPT_LOCI_SDIMG: loci_sdimg_path = optarg; loci_enabled = true; break;
            case OPT_ACIA_ADDR:
                acia_addr_arg = optarg;
                break;
            case OPT_DTL2000:
                dtl2000_arg = optarg;
                break;
            case OPT_DTL2000_ADDR:
                dtl2000_addr_arg = optarg;
                break;
            case '?':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }

    log_init(verbose ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Cast discover: standalone mode, list devices and exit */
    if (cast_discover) {
#ifdef HAS_CAST
        cast_server_discover_devices(3000);
#else
        fprintf(stderr, "Cast support not compiled in. Build with CAST=1.\n");
#endif
        return 0;
    }

    /* Set headless and scale before init so renderer is configured correctly */
    emu.headless = headless;
    emu.scale_factor = scale_factor;

    if (!emulator_init(&emu)) {
        log_error("Failed to initialize emulator");
        return 1;
    }

    emu.fast_load = fast_load;

    emu.max_cycles = max_cycles;
    emu.screenshot_file = screenshot_file;

    /* Set keyboard layout */
    if (keyboard_layout && strcasecmp(keyboard_layout, "azerty") == 0) {
        oric_keyboard_set_layout(&emu.keyboard, ORIC_KB_AZERTY);
        log_info("Keyboard layout: AZERTY");
    } else {
        log_info("Keyboard layout: QWERTY");
    }
    /* Set joystick mode */
    if (joystick_mode) {
        if (strcasecmp(joystick_mode, "keys") == 0 || strcasecmp(joystick_mode, "keyboard") == 0) {
            oric_joystick_set_mode(&emu.joystick, ORIC_JOY_KEYBOARD);
        } else if (strcasecmp(joystick_mode, "gamepad") == 0 || strcasecmp(joystick_mode, "sdl") == 0) {
            oric_joystick_set_mode(&emu.joystick, ORIC_JOY_SDL_GAMEPAD);
#ifdef HAS_SDL2
            if (SDL_NumJoysticks() > 0) {
                oric_joystick_open_sdl(&emu.joystick, 0);
            } else {
                log_info("Joystick: no SDL game controller found, waiting for hot-plug");
            }
#endif
        } else {
            log_error("Unknown joystick mode '%s'. Use: keys, gamepad", joystick_mode);
        }
    }

    /* Set printer type and open output */
    if (printer_file) {
        if (printer_type_arg && strcasecmp(printer_type_arg, "mcp40") == 0) {
            emu.printer.type = PRINTER_MCP40;
            log_info("Printer type: MCP-40 plotter");
        } else {
            emu.printer.type = PRINTER_TEXT;
            log_info("Printer type: text");
        }
        if (!oric_printer_open(&emu.printer, printer_file)) {
            log_error("Failed to open printer output: %s", printer_file);
        }
    }

    /* Serial interface (ACIA 6551) */
    if (acia_addr_arg) {
        emu.acia_base_addr = (uint16_t)strtol(acia_addr_arg, NULL, 16);
        log_info("ACIA base address: $%04X", emu.acia_base_addr);
    } else if (loci_enabled && serial_arg) {
        /* LOCI firmware exposes its ACIA at $0380-$0383 (acia.c). Under
         * --loci, default there so LOCI client software finds it. */
        emu.acia_base_addr = 0x0380;
        log_info("ACIA base address: $0380 (LOCI default — override with --acia-addr)");
    } else {
        emu.acia_base_addr = ACIA_DEFAULT_BASE;
    }
    if (serial_arg) {
        serial_backend_t* sb = NULL;
        if (strcmp(serial_arg, "loopback") == 0) {
            sb = serial_backend_loopback_create();
        } else if (strncmp(serial_arg, "tcp:", 4) == 0) {
            /* Parse tcp:host:port */
            char host[256] = {0};
            uint16_t port = 23;  /* Default: telnet */
            const char* hp = serial_arg + 4;
            const char* colon = strrchr(hp, ':');
            if (colon && colon != hp) {
                size_t hlen = (size_t)(colon - hp);
                if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
                memcpy(host, hp, hlen);
                host[hlen] = '\0';
                port = (uint16_t)atoi(colon + 1);
            } else {
                strncpy(host, hp, sizeof(host) - 1);
            }
            sb = serial_backend_tcp_create(host, port);
        } else if (strcmp(serial_arg, "pty") == 0) {
            sb = serial_backend_pty_create();
        } else if (strcmp(serial_arg, "modem") == 0 ||
                   strncmp(serial_arg, "modem:", 6) == 0) {
            /* Hayes AT modem. Modes:
             *   --serial modem              Pure command mode (use ATD to dial)
             *   --serial modem:host:port    Preset host (ATD without args connects here)
             *   --serial modem:listen:port  Server mode (ATA to accept) */
            const char* hp = (serial_arg[5] == ':') ? serial_arg + 6 : "";
            bool listen_mode = false;
            char host[256] = {0};
            uint16_t port = 23;
            if (strncmp(hp, "listen:", 7) == 0) {
                listen_mode = true;
                port = (uint16_t)atoi(hp + 7);
            } else {
                const char* colon = strrchr(hp, ':');
                if (colon && colon != hp) {
                    size_t hlen = (size_t)(colon - hp);
                    if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
                    memcpy(host, hp, hlen);
                    host[hlen] = '\0';
                    port = (uint16_t)atoi(colon + 1);
                } else {
                    strncpy(host, hp, sizeof(host) - 1);
                }
            }
            sb = serial_backend_modem_create(host, port, listen_mode);
        } else if (strncmp(serial_arg, "com:", 4) == 0) {
            /* Parse com:baud,bits,parity,stop,device */
            sb = serial_backend_com_create(serial_arg + 4);
        } else if (strncmp(serial_arg, "digitelec:", 10) == 0) {
            /* Parse digitelec:host:port — Digitelec DTL 2000 V23 modem */
            char host[256] = {0};
            uint16_t port = 23;
            const char* hp = serial_arg + 10;
            const char* colon = strrchr(hp, ':');
            if (colon && colon != hp) {
                size_t hlen = (size_t)(colon - hp);
                if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
                memcpy(host, hp, hlen);
                host[hlen] = '\0';
                port = (uint16_t)atoi(colon + 1);
            } else {
                strncpy(host, hp, sizeof(host) - 1);
            }
            sb = serial_backend_digitelec_create(host, port, &emu.acia);
        } else {
            log_error("Unknown serial backend: %s", serial_arg);
            log_error("  loopback, tcp:host:port, pty, modem:host:port,");
            log_error("  modem:listen:port, com:baud,bits,P,stop,device, digitelec:host:port");
            emulator_cleanup(&emu);
            return 1;
        }

        if (sb) {
            if (sb->open(sb)) {
                acia_set_backend(&emu.acia, sb);
                emu.serial_backend = sb;
                emu.has_serial = true;
                if (serial_v23 || sb->type == SERIAL_BACKEND_DIGITELEC) {
                    acia_set_v23_mode(&emu.acia, true);
                }
                if (serial_buffer_size > 0) {
                    acia_set_rx_fifo(&emu.acia, serial_buffer_size);
                }
                if (serial_irq_on_rdrf) {
                    acia_set_irq_on_rdrf(&emu.acia, true);
                }
                if (serial_trace_file) {
                    acia_set_trace(&emu.acia, serial_trace_file);
                }
                log_info("Serial interface enabled: %s", serial_arg);
            } else {
                log_error("Failed to open serial backend: %s", serial_arg);
                serial_backend_destroy(sb);
            }
        }
    }

    /* Digitelec DTL 2000 — faithful PIA 6821 + ACIA 6850 modem card.
     * The transport backend reuses the generic serial backends. */
    if (dtl2000_arg) {
        uint16_t base = DTL2000_DEFAULT_BASE;
        if (dtl2000_addr_arg) {
            base = (uint16_t)strtol(dtl2000_addr_arg, NULL, 16);
        }
        if (emu.has_microdisc) {
            log_warning("DTL 2000 at $%04X shares page 3 with the disc electronics "
                        "(Jasmin) — not faithful to coexist on real hardware", base);
        }
        serial_backend_t* db = NULL;
        if (strcmp(dtl2000_arg, "loopback") == 0) {
            db = serial_backend_loopback_create();
        } else if (strncmp(dtl2000_arg, "tcp:", 4) == 0) {
            char host[256] = {0};
            uint16_t port = 23;
            const char* hp = dtl2000_arg + 4;
            const char* colon = strrchr(hp, ':');
            if (colon && colon != hp) {
                size_t hlen = (size_t)(colon - hp);
                if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
                memcpy(host, hp, hlen);
                host[hlen] = '\0';
                port = (uint16_t)atoi(colon + 1);
            } else {
                strncpy(host, hp, sizeof(host) - 1);
            }
            db = serial_backend_tcp_create(host, port);
        } else if (strcmp(dtl2000_arg, "pty") == 0) {
            db = serial_backend_pty_create();
        } else {
            log_error("Unknown DTL 2000 transport: %s (loopback, tcp:host:port, pty)",
                      dtl2000_arg);
            emulator_cleanup(&emu);
            return 1;
        }

        if (db) {
            if (db->open(db)) {
                dtl2000_init(&emu.dtl2000, base);
                /* dtl2000_init() zeroes the struct — re-wire the CPU IRQ hooks */
                emu.dtl2000.irq_set = dtl2000_cpu_irq_set;
                emu.dtl2000.irq_clr = dtl2000_cpu_irq_clr;
                emu.dtl2000.irq_userdata = &emu;
                dtl2000_set_backend(&emu.dtl2000, db);
                emu.dtl2000_backend = db;
                emu.has_dtl2000 = true;
                log_info("Digitelec DTL 2000 enabled at $%04X (transport: %s)",
                         base, dtl2000_arg);
            } else {
                log_error("Failed to open DTL 2000 transport: %s", dtl2000_arg);
                serial_backend_destroy(db);
            }
        }
    }

    emu.frame_dump_dir = frame_dump_dir;
    emu.frame_dump_interval = (frame_dump_interval > 0) ? frame_dump_interval : 50;

    /* Parse --dump-ram-at CYCLES:FILE */
    if (dump_ram_at_arg) {
        const char* colon = strchr(dump_ram_at_arg, ':');
        if (colon) {
            emu.dump_ram_at_cycles = atoll(dump_ram_at_arg);
            emu.dump_ram_at_file = colon + 1;
            emu.dump_ram_at_done = false;
            log_info("RAM dump scheduled at %lld cycles → %s",
                     (long long)emu.dump_ram_at_cycles, emu.dump_ram_at_file);
        } else {
            log_error("Invalid --dump-ram-at format. Use CYCLES:FILE");
            emulator_cleanup(&emu);
            return 1;
        }
    } else {
        emu.dump_ram_at_cycles = -1;
        emu.dump_ram_at_file = NULL;
        emu.dump_ram_at_done = true;
    }

    /* Open --trace-irq FILE */
    if (trace_irq_file) {
        FILE* fp = fopen(trace_irq_file, "w");
        if (!fp) {
            log_error("Cannot open --trace-irq file: %s", trace_irq_file);
            emulator_cleanup(&emu);
            return 1;
        }
        fprintf(fp, "# Phosphoric IRQ trace — Oric-1/Atmos\n");
        fprintf(fp, "# Format: <cycle> <event> <details>\n");
        fprintf(fp, "# IRQ-ENTRY: PC before, target (= vector at $FFFE/F), IFR/IER snapshot, srcmask\n");
        fprintf(fp, "# RTI: PC after RTI, P flags, SP\n");
        emu.irq_trace_fp = fp;
        emu.irq_trace_active = true;
        emu.cpu.irq_trace_fp = fp;
        log_info("IRQ trace → %s", trace_irq_file);
    }

    /* Enable LOCI peripheral (--loci) */
    if (loci_enabled) {
        loci_init(&emu.loci);
        emu.loci.enabled = true;
        emu.has_loci = true;
        /* ROM-swap callback used by op 0xA0 MIA_BOOT (Sprint 34ad). */
        loci_set_rom_swap_callback(&emu.loci, loci_rom_swap_cb, &emu);
        loci_set_dsk_bus_callbacks(&emu.loci, loci_dsk_cpu_irq_set,
                                   loci_dsk_cpu_irq_clr,
                                   loci_dsk_sync_overlay, &emu);
        /* Tape-mount callback used by op_mount on LOCI_MNT_TAP (Sprint 34ao). */
        loci_set_tape_mount_callback(&emu.loci, loci_tape_mount_cb, &emu);
        /* Action-button hooks (Sprint 34ai). */
        loci_set_action_callbacks(&emu.loci,
            loci_action_install_irq_trap,
            loci_action_release_irq_trap,
            &emu);
        /* Sprint 34am fix: the real LOCI hardware's Pi Pico firmware
         * pre-initialises the AY-3-8910 R7 (mixer) to enable Port A as
         * output for keyboard scanning. The LOCI ROM relies on that
         * state and never writes R7 itself. Without this seed, the
         * keyboard scan callback's R7-bit-6 check always rejects, and
         * no key reaches the LOCI TUI. Mirror the firmware setup so
         * the ROM's ReadKeyboard sees a working PSG. */
        emu.psg.registers[7] = 0x7F;
        log_info("LOCI: pre-seeded PSG R7=$7F (firmware AY init for keyboard)");
        if (loci_flash_root && loci_sdimg_path) {
            log_error("--loci-flash and --loci-sdimg are mutually exclusive");
            return 1;
        }
        if (loci_sdimg_path) {
            if (!loci_attach_sdimg(&emu.loci, loci_sdimg_path)) {
                log_error("Failed to attach LOCI SD image: %s", loci_sdimg_path);
                return 1;
            }
            log_info("LOCI MIA enabled at $%04X-$%04X (SD image: %s)",
                     LOCI_MIA_BASE, LOCI_MIA_END, loci_sdimg_path);
        } else if (loci_flash_root) {
            loci_set_flash_root(&emu.loci, loci_flash_root);
            log_info("LOCI MIA enabled at $%04X-$%04X (flash root: %s)",
                     LOCI_MIA_BASE, LOCI_MIA_END, loci_flash_root);
        } else {
            log_info("LOCI MIA enabled at $%04X-$%04X (flash root: CWD)",
                     LOCI_MIA_BASE, LOCI_MIA_END);
        }
    }

    /* Load symbol table (--symbols FILE) */
    symbol_table_init(&emu.symbols);

    /* Sprint 35a — IPC control mode for OricForge. Logs go to stderr so
     * stdout stays a clean protocol channel. Forces headless so SDL output
     * never collides with stdout traffic. */
    emu.control_mode = control_mode;
    emu.bench_mode = bench_mode;
    if (control_mode) {
        log_set_stream(stderr);
        emu.headless = true;
        emu.debugger.active = true;   /* wait for first client command */
    }

    /* Route debugger break into ncurses TUI when --tui is set
     * (requires build with TUI=1). Init done lazily on first break. */
    emu.tui_mode = tui_mode;
    if (tui_mode) {
#ifdef HAS_TUI
        if (!tui_init()) {
            log_error("Failed to initialise ncurses TUI");
            emu.tui_mode = false;
        }
#else
        log_error("--tui requires a build with TUI=1 (ncurses)");
        emu.tui_mode = false;
#endif
    }
    if (symbols_file) {
        if (symbol_table_load(&emu.symbols, symbols_file) < 0) {
            emulator_cleanup(&emu);
            return 1;
        }
    }

    /* Parse --screenshot-at CYCLES:FILE */
    if (screenshot_at_arg) {
        const char* colon = strchr(screenshot_at_arg, ':');
        if (colon) {
            emu.screenshot_at_cycles = atoll(screenshot_at_arg);
            emu.screenshot_at_file = colon + 1;
        } else {
            log_error("Invalid --screenshot-at format. Use CYCLES:FILE");
            emulator_cleanup(&emu);
            return 1;
        }
    }

    /* Parse --type-keys CYCLES:TEXT (Sprint 34av: TEXT may start with
     * "loci-hid:" to route keys via the LOCI HID bitmap instead of the
     * ORIC keyboard matrix — useful for automating the LOCI TUI). */
    if (type_keys_arg) {
        const char* colon = strchr(type_keys_arg, ':');
        if (colon) {
            emu.type_keys_at = atoll(type_keys_arg);
            const char* text = colon + 1;
            if (strncmp(text, "loci-hid:", 9) == 0) {
                emu.type_keys_loci_hid = true;
                text += 9;
            }
            emu.type_keys_text = text;
            emu.type_keys_idx = 0;
            emu.type_keys_next_cycle = emu.type_keys_at;
            emu.type_keys_done = false;
            log_info("Auto-type at %lld cycles (%s): \"%s\"",
                     (long long)emu.type_keys_at,
                     emu.type_keys_loci_hid ? "LOCI HID" : "ORIC matrix",
                     emu.type_keys_text);
        } else {
            log_error("Invalid --type-keys format. Use CYCLES:TEXT (e.g. 3000000:CLOAD\"\"\\n)");
            emulator_cleanup(&emu);
            return 1;
        }
    }

    /* Create frame dump directory if specified */
    if (frame_dump_dir) {
        mkdir(frame_dump_dir, 0755);
    }

    /* Store file paths for save state metadata */
    emu.rom_path = rom_file;
    emu.disk_path = disk_files[0];
    emu.diskrom_path = disk_rom_file;
    emu.tape_path = tape_file;

    /* Load ROM if specified */
    if (rom_file) {
        log_info("Loading ROM: %s", rom_file);
        if (!memory_load_rom(&emu.memory, rom_file, 0)) {
            log_error("Failed to load ROM: %s", rom_file);
            emulator_cleanup(&emu);
            return 1;
        }
    }

    /* ROM analysis (if requested) */
    if (rom_info_enabled && rom_file) {
        rom_analysis_t rom_analysis;
        rominfo_analyze(&rom_analysis, emu.memory.rom, ROM_SIZE);
        if (rom_info_file) {
            rominfo_report_to_file(&rom_analysis, emu.memory.rom, ROM_SIZE, rom_info_file);
        } else {
            rominfo_report(&rom_analysis, emu.memory.rom, ROM_SIZE, stdout);
        }
    } else if (rom_info_enabled && !rom_file) {
        log_error("--rom-info requires a ROM file (-r ROM)");
    }

    /* Detect or set machine model */
    if (model_arg) {
        if (strcasecmp(model_arg, "atmos") == 0 || strcmp(model_arg, "1.1") == 0) {
            emu.model = ORIC_MODEL_ATMOS;
        } else if (strcasecmp(model_arg, "oric1") == 0 || strcmp(model_arg, "1.0") == 0) {
            emu.model = ORIC_MODEL_ORIC1;
        } else {
            log_error("Unknown model '%s'. Use: oric1, atmos, 1.0, or 1.1", model_arg);
            emulator_cleanup(&emu);
            return 1;
        }
        log_info("Machine model: %s (user-specified)",
                 emu.model == ORIC_MODEL_ATMOS ? "ORIC Atmos" : "ORIC-1");
    } else if (rom_file) {
        emu.model = detect_rom_version(&emu.memory);
        log_info("Machine model: %s (auto-detected from ROM)",
                 emu.model == ORIC_MODEL_ATMOS ? "ORIC Atmos" : "ORIC-1");
    } else {
        emu.model = ORIC_MODEL_ORIC1;
    }
    emu.rom_patches = get_rom_patches(emu.model);
    log_info("ROM patches: %s", emu.rom_patches->name);

    /* Mount host filesystem */
    if (hostfs_path) {
        log_info("Mounting host filesystem: %s", hostfs_path);
        if (!hostfs_mount(&emu.hostfs, hostfs_path, false)) {
            log_error("Failed to mount host filesystem: %s", hostfs_path);
            emulator_cleanup(&emu);
            return 1;
        }
    }

    /* Load tape */
    if (tape_file) {
        log_info("Loading tape: %s", tape_file);
        if (fast_load) {
            /* Fast load: buffer TAP data for deferred injection after RAM test */
            tap_file_t* tap = tap_open_read(tape_file, true);
            if (tap) {
                tap_header_t header;
                if (tap_read_header(tap, &header)) {
                    log_info("Fast load (deferred): '%s' type=%02X start=$%04X end=$%04X",
                             header.name, header.type, header.start_addr, header.end_addr);
                    uint16_t size = header.end_addr - header.start_addr + 1;
                    uint8_t* buf = (uint8_t*)malloc(size);
                    if (buf) {
                        int rd = tap_read_data(tap, buf, size);
                        if (rd > 0) {
                            emu.fastload_buf = buf;
                            emu.fastload_addr = header.start_addr;
                            emu.fastload_end = header.end_addr;
                            emu.fastload_size = (uint16_t)rd;
                            emu.fastload_type = header.type;
                            emu.fastload_auto_run = header.auto_run;
                            emu.fastload_pending = true;
                            log_info("Buffered %d bytes for deferred injection to $%04X-$%04X",
                                     rd, header.start_addr, header.start_addr + rd - 1);
                        } else {
                            free(buf);
                        }
                    }
                }

                /* Also buffer the full tape for subsequent CLOADs via ROM
                 * patching. Multi-block TAP files (like TYRANN) have a BASIC
                 * loader as block 1 that CLOADs additional blocks at runtime.
                 * Set tape position past the first block's data, and strip
                 * any padding bytes so the ROM parses headers correctly. */
                uint32_t remaining_pos = tap_tell(tap);
                if (remaining_pos < tap_size(tap) && tap->data) {
                    emu.tapelen = (int)tap_size(tap);
                    emu.tapebuf = (uint8_t*)malloc((size_t)emu.tapelen);
                    if (emu.tapebuf) {
                        memcpy(emu.tapebuf, tap->data, (size_t)emu.tapelen);
                        emu.tapeoffs = (int)remaining_pos;
                        emu.tape_loaded = true;
                        emu.tape_syncstack = -1;
                        log_info("Tape buffered for CLOAD: %d bytes, offset=%d",
                                 emu.tapelen, emu.tapeoffs);
                    }
                }

                tap_close(tap);
            } else {
                log_warning("Failed to open tape: %s", tape_file);
            }
        } else {
            /* Normal load: buffer TAP for CLOAD via ROM patching */
            FILE* f = fopen(tape_file, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                emu.tapelen = ftell(f);
                fseek(f, 0, SEEK_SET);
                emu.tapebuf = (uint8_t*)malloc(emu.tapelen);
                if (emu.tapebuf) {
                    size_t rd = fread(emu.tapebuf, 1, emu.tapelen, f);
                    if ((int)rd == emu.tapelen) {
                        emu.tapeoffs = 0;
                        emu.tape_loaded = true;
                        emu.tape_syncstack = -1;
                        emu.tape_auto_cload_pending = true;
                        log_info("Tape buffered for CLOAD: %d bytes", emu.tapelen);
                    } else {
                        log_warning("Tape read incomplete: %zu/%d bytes", rd, emu.tapelen);
                        free(emu.tapebuf);
                        emu.tapebuf = NULL;
                    }
                }
                fclose(f);
            } else {
                log_warning("Failed to open tape: %s", tape_file);
            }
        }
    }

    /* Load disks with Microdisc controller */
    bool any_disk = false;
    for (int i = 0; i < MICRODISC_MAX_DRIVES; i++) {
        if (disk_files[i]) { any_disk = true; break; }
    }

    if (any_disk) {
        /* Initialize Microdisc controller */
        microdisc_init(&emu.microdisc);
        emu.microdisc.cpu_irq_set = microdisc_cpu_irq_set;
        emu.microdisc.cpu_irq_clr = microdisc_cpu_irq_clr;
        emu.microdisc.cpu_userdata = &emu;
        emu.has_microdisc = true;

        /* Load Microdisc ROM if specified */
        if (disk_rom_file) {
            log_info("Loading Microdisc ROM: %s", disk_rom_file);
            if (!microdisc_load_rom(&emu.microdisc, disk_rom_file)) {
                log_error("Failed to load Microdisc ROM: %s", disk_rom_file);
                emulator_cleanup(&emu);
                return 1;
            }
            /* Set overlay ROM in memory system */
            emu.memory.overlay_rom = emu.microdisc.diskrom_data;
            emu.memory.overlay_rom_size = emu.microdisc.diskrom_size;
            emu.memory.overlay_active = true;
            emu.memory.basic_rom_disabled = true;
            log_info("Microdisc ROM loaded (%u bytes), overlay active", emu.microdisc.diskrom_size);
        }

        /* Load disk images into drives A-D */
        for (int i = 0; i < MICRODISC_MAX_DRIVES; i++) {
            if (!disk_files[i]) continue;

            log_info("Loading disk drive %c: %s", 'A' + i, disk_files[i]);
            emu.disks[i] = sedoric_load(disk_files[i]);
            if (!emu.disks[i]) {
                log_error("Failed to load disk image: %s", disk_files[i]);
                emulator_cleanup(&emu);
                return 1;
            }

            /* Connect disk data to Microdisc drive slot */
            microdisc_set_disk(&emu.microdisc, (uint8_t)i,
                               emu.disks[i]->data, emu.disks[i]->size,
                               emu.disks[i]->tracks, emu.disks[i]->sectors);
            log_info("Drive %c: %u bytes, %d sides x %d tracks x %d sectors",
                     'A' + i, emu.disks[i]->size, emu.disks[i]->sides,
                     emu.disks[i]->tracks, emu.disks[i]->sectors);
        }
    }

    /* Setup debugger if requested */
    if (debug_mode) {
        emu.debugger.active = true;
        log_info("Debugger mode enabled (will break at first instruction)");
    }
    if (debug_break_addr) {
        uint16_t addr = (uint16_t)strtol(debug_break_addr, NULL, 16);
        debugger_add_breakpoint(&emu.debugger, addr);
        log_info("Debugger breakpoint set at $%04X", addr);
    }

    /* --cast-to implicitly enables --cast-server */
    if (cast_to_enabled && !cast_server_enabled) {
        cast_server_enabled = true;
    }

    /* Initialize cast server if requested */
    if (cast_server_enabled) {
#ifdef HAS_CAST
        if (cast_server_init(&emu.cast_server, cast_server_port)) {
            emu.has_cast_server = true;
            /* Connect audio output to cast server for WAV streaming */
            audio_set_cast_server(&emu.cast_server);
        } else {
            log_error("Failed to start cast server");
        }
#else
        fprintf(stderr, "Cast support not compiled in. Build with CAST=1.\n");
#endif
    }

    /* Initialize CASTV2 client: discover device and cast */
    if (cast_to_enabled && emu.has_cast_server) {
#ifdef HAS_CAST
        char device_ip[64] = "";
        bool discovered = false;

        if (cast_to_device && cast_to_device[0]) {
            /* Try to parse as IP address first */
            struct in_addr test_addr;
            if (inet_pton(AF_INET, cast_to_device, &test_addr) == 1) {
                strncpy(device_ip, cast_to_device, sizeof(device_ip) - 1);
                discovered = true;
            }
        }

        if (!discovered) {
            discovered = castv2_discover_device(device_ip, cast_to_device, 5000);
        }

        if (discovered) {
            /* Build stream URL */
            char local_ip[64] = "";
            if (!castv2_get_local_ip(local_ip)) {
                strncpy(local_ip, "127.0.0.1", sizeof(local_ip));
            }
            char stream_url[256];
            snprintf(stream_url, sizeof(stream_url), "http://%s:%d/",
                     local_ip, emu.cast_server.port);

            log_info("Casting to %s, stream URL: %s", device_ip, stream_url);

            if (castv2_connect_and_cast(&emu.castv2_client, device_ip, stream_url)) {
                emu.has_castv2 = true;
            } else {
                log_error("Failed to connect CASTV2 to %s", device_ip);
            }
        } else {
            log_error("No Chromecast device found%s%s",
                      cast_to_device ? " matching '" : "",
                      cast_to_device ? cast_to_device : "");
            if (cast_to_device) log_error("'");
        }
#else
        fprintf(stderr, "Cast support not compiled in. Build with CAST=1.\n");
#endif
    }

    /* Load save state if specified */
    if (load_state_file) {
        log_info("Loading save state: %s", load_state_file);
        if (!savestate_load(&emu, load_state_file)) {
            log_error("Failed to load save state: %s", load_state_file);
        }
    }

    if (!headless) {
        printf("\n");
        printf("Phosphoric v%s\n", EMU_VERSION);
        printf("Press Ctrl+C to quit\n\n");
    }

    /* CPU trace logging */
    trace_init(&emu.trace);
    if (trace_file) {
        if (trace_max > 0) {
            trace_set_max(&emu.trace, (uint64_t)trace_max);
        }
        if (!trace_open(&emu.trace, trace_file)) {
            log_error("Failed to open trace file: %s", trace_file);
        }
    }

    /* CPU performance profiler */
    profiler_init(&emu.profiler);
    if (profile_file) {
        profiler_start(&emu.profiler);
        log_info("CPU profiling enabled, report will be written to %s", profile_file);
    }

    /* Run emulation */
    emulator_run(&emu);

    /* Save state on exit if specified */
    if (save_state_file) {
        log_info("Saving state on exit: %s", save_state_file);
        savestate_save(&emu, save_state_file);
    }

    /* Write profiler report if enabled */
    if (profile_file) {
        profiler_stop(&emu.profiler);
        profiler_report_to_file(&emu.profiler, profile_file);
    }

    trace_close(&emu.trace);
    emulator_cleanup(&emu);
    log_cleanup();

    return 0;
}
