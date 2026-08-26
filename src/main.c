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
#ifndef _WIN32
#include <strings.h>
#endif
#include <signal.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include "utils/oscompat.h"   /* statvfs/mkdir/SIGPIPE/monotonic portables */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "emulator.h"
#include "rom_patches.h"
#include "io/keyboard.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "io/via6522.h"
#include "video/video.h"
#include "video/export.h"
#include "storage/tap.h"
#include "storage/disk.h"
#include "storage/sedoric.h"
#include "storage/disk_http.h"   /* loci-webdisk archi B : disque servi par HTTP */
#include "io/microdisc.h"
#include "io/loci_sdimg.h"
#include "io/io_device.h"
#include "io/io_bus.h"        /* table de bus + dispatch (Epic 7/US2) */
#include "io/autotype.h"      /* pacing scan-driven de --type-keys */
#include "io/tape_patches.h" /* ROM CLOAD/CSAVE PC patches (ex-main.c) */
#include "io/loci_glue.h"    /* LOCI adapter callbacks (ex-main.c, Epic 9) */
#include "io/loci_internal.h"  /* loci_dsk_open_web (loci-webdisk archi B) */
#include "cli/cli_options.h"  /* enum OPT_* + long_options[] (Epic 7/US3) */
#include "cli/cli_usage.h"    /* cli_print_usage (Epic 7/US3) */
#include "cli/cli_parse.h"    /* cli_* parse helpers (Epic 7/US3) */
#include "audio/audio.h"
#include "io/keyboard.h"
#include "io/printer.h"
#include "debugger.h"
#include "tui.h"
#include "control.h"
#include "control_queue.h"
#include "network/http_api.h"
#include "network/gdbstub.h"
#include "savestate.h"
#include "utils/trace.h"
#include "utils/rominfo.h"
#ifdef HAS_SDL2
#include <SDL2/SDL.h>
#endif
#include "hostfs/hostfs.h"
#include "utils/logging.h"
#include "utils/netutil.h"     /* parse_host_port (Epic 7/US1) */
#include "utils/appsignal.h"   /* app_should_run / app_install_signal_handlers */
#ifdef HAS_CAST
#include <arpa/inet.h>
#endif

#ifdef __EMSCRIPTEN__
/* ─── Web glue: virtual keyboard bridge (called from JS via ccall) ─────────
 * The browser steals some real Ctrl chords (Ctrl+T = new tab) before they ever
 * reach the canvas; the on-screen keyboard routes through these exports instead,
 * writing the ORIC matrix directly — so Ctrl/Funct combos always work. */
static emulator_t* g_web_emu = NULL;

/* Press (down=1) or release (down=0) a key. `c` is an ASCII char or one of the
 * press_char sentinels (0x0D return, 0x1B esc, 0x80-0x83 arrows). `ctrl`/`funct`
 * apply the modifier for this keystroke. Release clears the whole matrix. */
EMSCRIPTEN_KEEPALIVE void web_key(int c, int ctrl, int funct, int shift, int down) {
    if (!g_web_emu) return;
    oric_keyboard_t* kb = &g_web_emu->keyboard;
    oric_keyboard_release_all(kb);
    if (!down) return;
    if (ctrl)  oric_keyboard_press_ctrl(kb);
    if (funct) oric_keyboard_press_funct(kb);
    if (shift) oric_keyboard_press_lshift(kb);
    oric_keyboard_press_char(kb, (char)c);
}

/* Release every key (matrix → all-released). */
EMSCRIPTEN_KEEPALIVE void web_key_release_all(void) {
    if (g_web_emu) oric_keyboard_release_all(&g_web_emu->keyboard);
}

/* I/O activity bitmap for the on-screen LEDs: bit0 = tape (CLOAD in progress),
 * bit1 = disk (WD1793 BUSY). Polled by the web UI. */
EMSCRIPTEN_KEEPALIVE int web_io_activity(void) {
    if (!g_web_emu) return 0;
    int bits = 0;
    if (g_web_emu->tape_readbyte_active) bits |= 1;
    if (g_web_emu->microdisc.fdc.status & FDC_ST_BUSY) bits |= 2;
    return bits;
}

/* Save a snapshot to /state.ost in the virtual FS (JS downloads it). 1 = ok. */
EMSCRIPTEN_KEEPALIVE int web_save_state(void) {
    return (g_web_emu && savestate_save(g_web_emu, "/state.ost")) ? 1 : 0;
}

/* Restore a snapshot from /state.ost (JS writes the uploaded bytes first).
 * Applied live to the running machine — no reload. 1 = ok. */
EMSCRIPTEN_KEEPALIVE int web_load_state(void) {
    return (g_web_emu && savestate_load(g_web_emu, "/state.ost")) ? 1 : 0;
}

/* Hot-insert a cassette from a VFS path the JS side just wrote (FS.writeFile).
 * Fast-loads it: the first block is parsed and queued for deferred RAM injection
 * (fires next frame, since the machine is well past boot), so the browser does
 * NOT wait for real cassette speed (~minute). The full file is also kept in
 * tapebuf for any subsequent CLOAD. 1 = ok. */
EMSCRIPTEN_KEEPALIVE int web_insert_tap(const char* path) {
    if (!g_web_emu || !path) return 0;

    /* Parse the TAP header + first data block for the instant fast-load. */
    tap_file_t* tap = tap_open_read(path, true);
    if (!tap) return 0;
    tap_header_t header;
    if (!tap_read_header(tap, &header)) { tap_close(tap); return 0; }
    uint16_t size = (uint16_t)(header.end_addr - header.start_addr + 1);
    uint8_t* fbuf = (uint8_t*)malloc(size ? size : 1);
    int rd = fbuf ? tap_read_data(tap, fbuf, size) : 0;
    tap_close(tap);
    if (rd <= 0) { free(fbuf); return 0; }

    /* Keep the whole file in tapebuf too (subsequent CLOADs / multi-block). */
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz <= (1 << 20)) {
            uint8_t* tb = (uint8_t*)malloc((size_t)sz);
            if (tb && fread(tb, 1, (size_t)sz, f) == (size_t)sz) {
                if (g_web_emu->tapebuf) free(g_web_emu->tapebuf);
                g_web_emu->tapebuf = tb;
                g_web_emu->tapelen = (int)sz;
                g_web_emu->tapeoffs = 0;
                g_web_emu->tape_loaded = true;
                g_web_emu->tape_syncstack = -1;
            } else {
                free(tb);
            }
        }
        fclose(f);
    }

    /* Arm the deferred fast-load (injected next frame: total_executed >> 3M). */
    if (g_web_emu->fastload_buf) free(g_web_emu->fastload_buf);
    g_web_emu->fastload_buf       = fbuf;
    g_web_emu->fastload_addr      = header.start_addr;
    g_web_emu->fastload_end       = header.end_addr;
    g_web_emu->fastload_size      = (uint16_t)rd;
    g_web_emu->fastload_type      = header.type;
    g_web_emu->fastload_auto_run  = header.auto_run;
    g_web_emu->fastload_pending   = true;
    return 1;
}

/* Hot-insert a .dsk into drive (0..3) from a VFS path. Requires the Microdisc
 * to be active (the machine was started with --disk-rom). Returns 0 if there is
 * no Microdisc, so the JS side can fall back to a reload that brings it up.
 * No write-back: the WASM build does not enable --disk-writeback. */
EMSCRIPTEN_KEEPALIVE int web_insert_disk(int drive, const char* path) {
    if (!g_web_emu || !path) return 0;
    if (!g_web_emu->has_microdisc) return 0;
    if (drive < 0 || drive >= MICRODISC_MAX_DRIVES) return 0;
    sedoric_disk_t* nd = sedoric_load(path);
    if (!nd) return 0;
    if (g_web_emu->disks[drive]) sedoric_destroy(g_web_emu->disks[drive]);
    g_web_emu->disks[drive] = nd;
    g_web_emu->microdisc.disk_dirty[drive] = false;
    microdisc_set_disk(&g_web_emu->microdisc, (uint8_t)drive, nd->data, nd->size,
                       nd->tracks, nd->sectors);
    g_web_emu->disk_paths[drive] = strdup(path);
    return 1;
}
#endif /* __EMSCRIPTEN__ */

/* Forward declarations for renderer (in renderer.c) */
bool renderer_init(int scale, bool prefer_software);
void renderer_cleanup(void);
void renderer_present(video_t* vid);
void renderer_set_border(bool on);
bool renderer_get_border(void);
void renderer_toggle_fullscreen(void);
void renderer_set_scale(int scale);
int renderer_get_scale(void);
void renderer_cycle_scale(void);




/* emulator_t is defined in include/emulator.h */

/* LOCI ROM-swap callback (Sprint 34ad).
 * Loads a ROM image into Oric memory at base_addr and resets the CPU
 * so the new reset vector is honoured. Only base_addr = $C000 is wired
 * for now (BASIC ROM swap); $A000 (Microdisc overlay) returns true
 * without actually swapping — handled by the existing --disk-rom path. */



/* Live ROM byte poke (ADJ_SCAN progress: the menu ROM polls its TIMINGS
 * byte at $FFF0 while the firmware sweeps tior). */
/* I/O callback: route VIA and Microdisc register access */
static uint8_t io_read_callback(uint16_t address, void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;

    /* Bus I/O : périphériques enregistrés (LOCI en tête, puis ACIA, Mageco,
     * Microdisc, DTL2000, ULA-NG). Repli sur le VIA.
     * ULA-NG en lecture : ne réclame que déverrouillée ; verrouillée, la fenêtre
     * retombe sur le miroir VIA (indiscernable). */
    const io_device_t* dev = io_bus_find(emu, address);
    if (dev) return dev->read(emu, address);

    /* VIA 6522: $0300-$030F (mirrored in $0300-$03FF) */
    return via_read(&emu->via, (uint8_t)(address & 0x0F));
}

/* Side-effect-free I/O read for observers (memory_peek): debugger, control API,
 * memory dumps, remote display. Uses the device's peek() when available (ACIA →
 * no RDRF clear), else falls back to read() — identical to io_read_callback for
 * devices without destructive reads. NB: the VIA fallback still uses via_read
 * (no via_peek yet); reading most VIA regs is harmless, and this is no worse
 * than the historical behavior where observers called memory_read directly. */
static uint8_t io_peek_callback(uint16_t address, void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;
    const io_device_t* dev = io_bus_find(emu, address);
    if (dev) return dev->peek ? dev->peek(emu, address) : dev->read(emu, address);
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
/* --audio-wav : write/patch a 44-byte canonical WAV header (PCM, 16-bit, stereo,
 * 44.1 kHz — matches ay_generate's interleaved output). Called with data_bytes=0
 * at open (placeholder) and with the real payload size at close. */
static void wav_put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static void wav_put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);         p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void wav_write_header(FILE* f, uint32_t data_bytes) {
    const uint32_t rate = AUDIO_SAMPLE_RATE, chans = 2, bits = 16;
    const uint32_t byte_rate = rate * chans * bits / 8;
    uint8_t h[44];
    memcpy(h + 0, "RIFF", 4);
    wav_put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    wav_put_le32(h + 16, 16);                 /* subchunk1 size = 16 (PCM) */
    wav_put_le16(h + 20, 1);                  /* audio format = PCM */
    wav_put_le16(h + 22, (uint16_t)chans);
    wav_put_le32(h + 24, rate);
    wav_put_le32(h + 28, byte_rate);
    wav_put_le16(h + 32, (uint16_t)(chans * bits / 8));   /* block align */
    wav_put_le16(h + 34, (uint16_t)bits);
    memcpy(h + 36, "data", 4);
    wav_put_le32(h + 40, data_bytes);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

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
        /* Write Data — timestamped with the current CPU cycle so audio-rate
         * register hammering (digidrums) is reproduced sample-accurately. */
        ay_write_data_timed(&emu->psg, emu->via.ora, emu->cpu.cycles);
        /* --psg-trace : log the sound-register writes (0..13). Ports 14/15 are
         * the keyboard matrix, not audio, so they are skipped. NB : reg 7
         * (mixer) is hammered to $7F by the keyboard scan — that is real bus
         * activity, kept as-is (measured, not filtered). */
        if (emu->psg_trace_fp) {
            uint8_t reg = emu->psg.selected_reg;
            if (reg < 14)
                fprintf(emu->psg_trace_fp, "%llu R%u=%02X\n",
                        (unsigned long long)emu->cpu.cycles, reg, emu->via.ora);
        }
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
    uint8_t col = emu->via.orb & 0x07;
    uint8_t reg7 = emu->psg.registers[7];
    uint8_t reg14 = emu->psg.registers[14];
    uint8_t mcol = emu->keyboard.matrix[col];
    uint8_t result;

    if (!(reg7 & 0x40)) {
        /* Port A in output mode → PB3 always 0 (no key detected) */
        result = 0xF7;
    } else {
        /* Scan-driven --type-keys pacing: count a completed keyboard scan pass
         * every time the descending column sweep (7->0) restarts. This is the
         * real hardware handshake the auto-typer synchronises on so it never
         * changes the matrix faster than the program reads it. */
        if (autotype_is_new_pass(emu->kbd_scan_prev_col, col))
            emu->kbd_scan_passes++;
        emu->kbd_scan_prev_col = col;

        /* Check: any pressed key in column matches the inverted mask?
         * ~key_matrix = pressed keys (1=pressed), ~reg14 = rows to test */
        uint8_t pressed = (~mcol) & (~reg14) & 0xFF;

        /* PB3 = 1 if any key matches, 0 otherwise.
         * Other input bits default to 1 (no external input). */
        result = pressed ? 0xFF : 0xF7;
    }

    /* --kbd-scan-trace : one line per Port B read, so a custom (non-ROM)
     * keyboard scanner can see exactly what the emulator returns — column,
     * the two AY gate registers (reg7 direction, reg14 row mask), the matrix
     * byte for that column, and the rendered PB3. reg7 bit6=0 or matrix[col]
     * broadly ≠ 0xFF are the two classic "scan reads garbage" causes. */
    if (emu->kbd_trace_fp) {
        fprintf(emu->kbd_trace_fp,
                "%llu col=%u reg7=%02X reg14=%02X matrix=%02X PB3=%u\n",
                (unsigned long long)emu->cpu.cycles, col, reg7, reg14, mcol,
                (result & 0x08) ? 1u : 0u);
    }
    return result;
}

static void io_write_callback(uint16_t address, uint8_t value, void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;

    /* Bus I/O : périphériques enregistrés (LOCI en tête, puis ACIA, Mageco,
     * Microdisc, DTL2000, ULA-NG). Le device peut **décliner** l'écriture
     * (write → false) : c'est le cas de l'ULA-NG verrouillée, qui guette la
     * séquence 'N','G' en fenêtre mais laisse retomber les octets neutres sur le
     * VIA (bit-à-bit, indiscernable). Sinon repli VIA.
     *
     * NB : le snoop LOCI sur l'écriture VIA ORB $0300 (ligne moteur cassette,
     * cf. loci_tap_motor plus bas) n'est PAS un claim — l'écriture va au VIA ;
     * il reste donc hors bus, dans le chemin VIA. */
    const io_device_t* dev = io_bus_find_write(emu, address);
    if (dev && dev->write(emu, address, value)) return;

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
    /* --tape-signal-free : gate le moteur cassette signal-level sur ORB PB6 (le
     * moteur piloté par la ROM), pour les ROM clean-room dont le layout n'atteint
     * pas la plage PC tape-read de la 1.1 (cf. tape_patches). */
    if (emu->cassette.free_gate && address == 0x0300) {
        bool on = (value & 0x40) != 0;
        if (on && !emu->cassette.started) {
            cassette_rewind(&emu->cassette);
            emu->cassette.started = true;
        }
        cassette_set_motor(&emu->cassette, on);
    }
    /* Signal-level cassette motor is gated on the ROM tape-read routine PC in
     * tape_patches() (Sprint 90), not on ORB PB6 — the keyboard column scan
     * clobbers PB6 identically at the READY prompt and during CLOAD. */

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

/* Export a screenshot honouring --export-border: with the flag, the
 * overscan border is composited around the active area (larger image). */
static bool emu_export_image(emulator_t* emu, const char* path) {
    return emu->export_border ? video_export_auto_bordered(&emu->video, path)
                              : video_export_auto(&emu->video, path);
}

/* Compose un nom de fichier de capture qui n'écrase pas un fichier existant.
 * Base = "screenshot", extension = ".ppm". Si "screenshot.ppm" est libre, il
 * est utilisé ; sinon on insère un horodatage local "screenshot-YYYYMMDD-HHMMSS"
 * (déterministe et parlant), et en cas de collision improbable dans la même
 * seconde on suffixe un index "-NN". Le nom retenu est écrit dans out. */
static void screenshot_unique_name(char* out, size_t out_sz) {
    const char* base = "screenshot";
    const char* ext  = ".png";

    snprintf(out, out_sz, "%s%s", base, ext);
    if (access(out, F_OK) != 0)
        return; /* nom par défaut libre */

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);

    snprintf(out, out_sz, "%s-%s%s", base, stamp, ext);
    for (int i = 1; access(out, F_OK) == 0 && i < 100; i++)
        snprintf(out, out_sz, "%s-%s-%02d%s", base, stamp, i, ext);
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

/* Per-cycle clock callback (registered on the CPU). The CPU invokes this for
 * every bus cycle it consumes, so PHI2-clocked peripherals advance in step with
 * the CPU's memory accesses instead of in one post-instruction batch. The total
 * cycles delivered per instruction equals the instruction's cycle count. */
static void cpu_cycle_tick(void* ctx, int cycles) {
    emulator_t* emu = (emulator_t*)ctx;
    via_update(&emu->via, cycles);
    if (emu->cassette.signal_mode)
        cassette_tick(&emu->cassette, &emu->via, cycles);
    /* Tape-OUT capture : échantillonne PB7 (Timer1) pour reconstruire le .TAP.
     * Résolution = 1 bus-tick (~1-7 cy) << période bit (416/624), sans effet
     * sur le seuil 512. */
    if (emu->tape_capture.active)
        tape_capture_sample(&emu->tape_capture, &emu->via, emu->cpu.cycles);
    /* Périphériques de bus temporisés (FDC/ACIA/DTL/Mageco), ordre historique
     * préservé dans io_bus_tick (Epic 7/US5). */
    io_bus_tick(emu, cycles);
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

/* Mageco MIDI (ACIA 6850) IRQ callbacks */
static void mageco_cpu_irq_set(emulator_t* emu) {
    cpu_irq_set(&emu->cpu, IRQF_MAGECO);
}

static void mageco_cpu_irq_clr(emulator_t* emu) {
    cpu_irq_clear(&emu->cpu, IRQF_MAGECO);
}

/* parse_host_port → src/utils/netutil.c (Epic 7/US1, Sprint 125). */

/* Réécrit le .dsk du lecteur @p drv sur disque s'il a été modifié par le jeu
 * et que --disk-writeback est actif. Appelé avant tout swap/éjection pour ne
 * pas perdre les écritures. Retourne true si une sauvegarde a eu lieu. */
static bool osd_writeback_drive(emulator_t* emu, int drv) {
    if (!emu->disk_writeback || drv < 0 || drv >= MICRODISC_MAX_DRIVES) return false;
    if (!emu->microdisc.disk_dirty[drv] || !emu->disks[drv] || !emu->disk_paths[drv])
        return false;
    bool ok = sedoric_save(emu->disks[drv], emu->disk_paths[drv]);
    log_info("OSD: write-back lecteur %c -> %s (%s)", 'A' + drv, emu->disk_paths[drv],
             ok ? "OK" : "ECHEC");
    emu->microdisc.disk_dirty[drv] = false;
    return ok;
}

/* OSD : éjecte la disquette du lecteur cible (write-back préalable si activé). */
static void osd_do_eject(emulator_t* emu) {
    if (!emu->has_microdisc) {
        snprintf(emu->osd.status, sizeof(emu->osd.status),
                 "Pas de Microdisc (--disk-rom requis)");
        return;
    }
    int drv = emu->osd.disk_drive;
    if (drv < 0 || drv >= MICRODISC_MAX_DRIVES) drv = 0;
    if (!emu->disks[drv]) {
        snprintf(emu->osd.status, sizeof(emu->osd.status), "Lecteur %c deja vide", 'A' + drv);
        return;
    }
    osd_writeback_drive(emu, drv);
    sedoric_destroy(emu->disks[drv]);
    emu->disks[drv] = NULL;
    emu->disk_paths[drv] = NULL;
    microdisc_set_disk(&emu->microdisc, (uint8_t)drv, NULL, 0, 0, 0);
    if (drv == 0) emu->disk_path = NULL;
    snprintf(emu->osd.status, sizeof(emu->osd.status), "Lecteur %c ejecte", 'A' + drv);
    log_info("OSD: lecteur %c ejecte", 'A' + drv);
    osd_close(&emu->osd);
}

/* OSD : éjecte la cassette (libère le tampon TAP, vide le pont de lecture). */
static void osd_do_eject_tape(emulator_t* emu) {
    if (!emu->tape_loaded && !emu->tapebuf) {
        snprintf(emu->osd.status, sizeof(emu->osd.status), "Aucune cassette");
        return;
    }
    if (emu->tapebuf) { free(emu->tapebuf); emu->tapebuf = NULL; }
    emu->tapelen = 0;
    emu->tapeoffs = 0;
    emu->tape_loaded = false;
    emu->tape_path = NULL;
    snprintf(emu->osd.status, sizeof(emu->osd.status), "Cassette ejectee");
    log_info("OSD: cassette ejectee");
    osd_close(&emu->osd);
}

/* OSD hot-swap : charge le média sélectionné dans l'overlay (cassette ou
 * disquette lecteur A) sans quitter l'émulateur. */
static void osd_do_load(emulator_t* emu, const osd_entry_t* e) {
    if (e->is_disk) {
        if (!emu->has_microdisc) {
            snprintf(emu->osd.status, sizeof(emu->osd.status),
                     "Pas de Microdisc (--disk-rom requis)");
            return;
        }
        int drv = emu->osd.disk_drive;
        if (drv < 0 || drv >= MICRODISC_MAX_DRIVES) drv = 0;
        sedoric_disk_t* nd = sedoric_load(e->path);
        if (!nd) {
            snprintf(emu->osd.status, sizeof(emu->osd.status), "Echec: %.40s", e->name);
            return;
        }
        /* Sauve l'ancien disque s'il a été modifié, avant de l'écraser. */
        osd_writeback_drive(emu, drv);
        if (emu->disks[drv]) sedoric_destroy(emu->disks[drv]);
        emu->disks[drv] = nd;
        emu->microdisc.disk_dirty[drv] = false;
        microdisc_set_disk(&emu->microdisc, (uint8_t)drv, nd->data, nd->size,
                           nd->tracks, nd->sectors);
        /* Suivi du chemin par lecteur (write-back/éjection ultérieurs). Les
         * pointeurs initiaux viennent d'argv (non libérables) → on réaffecte. */
        emu->disk_paths[drv] = strdup(e->path);
        if (drv == 0)
            emu->disk_path = emu->disk_paths[drv];
        snprintf(emu->osd.status, sizeof(emu->osd.status),
                 "Disque %c: %.28s (reboot/DIR)", 'A' + drv, e->name);
        log_info("OSD: disque %c <- %s", 'A' + drv, e->path);
    } else {
        FILE* f = fopen(e->path, "rb");
        if (!f) {
            snprintf(emu->osd.status, sizeof(emu->osd.status), "Echec: %.40s", e->name);
            return;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > (1 << 20)) { fclose(f); return; }
        uint8_t* buf = (uint8_t*)malloc((size_t)sz);
        if (!buf) { fclose(f); return; }
        if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return; }
        fclose(f);
        if (emu->tapebuf) free(emu->tapebuf);
        emu->tapebuf = buf;
        emu->tapelen = (int)sz;
        emu->tapeoffs = 0;
        emu->tape_loaded = true;
        emu->tape_path = strdup(e->path);
        snprintf(emu->osd.status, sizeof(emu->osd.status),
                 "Cassette: %.28s (CLOAD\"\")", e->name);
        log_info("OSD: cassette <- %s", e->path);
    }
    osd_close(&emu->osd);
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

    ula_ng_init(&emu->ula_ng);   /* ULA-NG verrouillée au démarrage (état HCS10017) */

    cassette_init(&emu->cassette);

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

    /* Initialize Mageco MIDI interface (disabled by default) */
    mageco_init(&emu->mageco, MAGECO_DEFAULT_BASE);
    emu->mageco.irq_set = mageco_cpu_irq_set;
    emu->mageco.irq_clr = mageco_cpu_irq_clr;
    emu->mageco.irq_userdata = emu;

    /* Initialize PSG (AY-3-8912) with keyboard input callback */
    ay_init(&emu->psg, ORIC_CLOCK_HZ);
    emu->psg.porta_input = keyboard_matrix_read;
    emu->psg.userdata = emu;

    /* Wire up I/O callbacks */
    memory_set_io_callbacks(&emu->memory, io_read_callback, io_write_callback, emu);
    memory_set_io_peek(&emu->memory, io_peek_callback);  /* observateurs non destructifs */
    via_set_irq_callback(&emu->via, irq_callback, emu);

    /* Expose la table de bus à la sérialisation : les devices qui fournissent un
     * hook save/load (ex. ULA-NG → section "UNG") persistent leur état .ost. */
    int io_bus_n = 0;
    const io_device_t* io_bus_tbl = io_bus_devices(&io_bus_n);
    savestate_set_io_devices(io_bus_tbl, io_bus_n);

    /* Drive PHI2-clocked peripherals from the CPU's per-cycle clock (replaces
     * the post-instruction batch ticking in the frame loop). */
    cpu_set_cycle_callback(&emu->cpu, cpu_cycle_tick, emu);

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

    /* ULA-NG palette-indirection (§5.1) : wire video to the NG LUT. Inert until
     * unlocked + NG_MODE.b0 (active), and the LUT is identity at reset. */
    emu->video.ng_pal        = emu->ula_ng.pal;
    emu->video.ng_active     = &emu->ula_ng.active;
    emu->video.ng_scrstart   = &emu->ula_ng.scrstart;   /* start-address (§5.3) */
    emu->video.ng_scrollx    = &emu->ula_ng.scrollx;    /* scroll fin X (§5.5) */
    emu->video.ng_scrolly    = &emu->ula_ng.scrolly;    /* scroll fin Y (§5.5) */
    emu->video.ng_attr       = emu->ula_ng.attr;        /* attributs // (§5.6) */
    emu->video.ng_attr_active = &emu->ula_ng.attr_active;
    emu->video.ng_dev        = &emu->ula_ng;            /* sprites (§5.7) */
    emu->video.ng_chunky_active = &emu->ula_ng.chunky_active;  /* chunky 4bpp (§5.8) */
    emu->video.ng_text80_active = &emu->ula_ng.text80_active;  /* texte 80col (§5.8) */
    emu->video.ng_vram        = emu->ula_ng.vram;             /* VRAM chunky VDU (v0.2) */
    emu->video.ng_vram_active = &emu->ula_ng.vram_active;

    /* Initialize renderer if not headless */
    if (!emu->headless) {
        renderer_init(emu->scale_factor > 0 ? emu->scale_factor : 3, emu->render_software);
        renderer_set_border(!emu->no_border);
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
    emu->screenshot_text_file = NULL;
    emu->screenshot_ansi_file = NULL;
    emu->timed_capture_count = 0;   /* captures -at répétables (voir timed_capture_t) */
    emu->frame_dump_dir = NULL;
    emu->frame_dump_interval = 50;
    emu->kbd_scan_prev_col = 0xFF;   /* sentinel: first read starts no pass */
    emu->type_keys_when_addr = -1;
    emu->type_keys_when_text = NULL;
    emu->type_keys_when_done = false;
    emu->screenshot_when_addr = -1;
    emu->screenshot_when_file = NULL;
    emu->screenshot_when_done = false;
    emu->dump_ram_when_addr = -1;
    emu->dump_ram_when_file = NULL;
    emu->dump_ram_when_done = false;
    emu->screenshot_text_when_addr = -1;
    emu->screenshot_text_when_file = NULL;
    emu->screenshot_text_when_done = false;
    emu->when_condition_unmet = false;
    emu->poke_count = 0;
    emu->irq_trace_fp = NULL;
    emu->irq_trace_active = false;
    emu->irq_trace_depth = 0;
    emu->cpu.irq_trace_fp = NULL;
    emu->cpu.irq_trace_count = 0;
    emu->psg_trace_fp = NULL;
    emu->kbd_trace_fp = NULL;
    emu->audio_wav_fp = NULL;
    emu->audio_wav_data_bytes = 0;

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
    if (emu->psg_trace_fp) {
        fclose(emu->psg_trace_fp);
        emu->psg_trace_fp = NULL;
    }
    if (emu->kbd_trace_fp) {
        fclose(emu->kbd_trace_fp);
        emu->kbd_trace_fp = NULL;
    }
    if (emu->audio_wav_fp) {
        /* Backpatch the RIFF/data sizes now that the payload length is known. */
        wav_write_header(emu->audio_wav_fp, emu->audio_wav_data_bytes);
        log_info("Audio WAV: %u bytes PCM (%.2f s @ %d Hz stereo)",
                 emu->audio_wav_data_bytes,
                 (double)emu->audio_wav_data_bytes / (AUDIO_SAMPLE_RATE * 2 * 2),
                 AUDIO_SAMPLE_RATE);
        fclose(emu->audio_wav_fp);
        emu->audio_wav_fp = NULL;
    }
    if (!emu->headless) {
        audio_avi_tap_disable();   /* idempotent safety net (freed at AVI close) */
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
    /* Stop the HTTP server (joins its thread → no more producers) BEFORE
     * destroying the queue, so no submit() can touch freed memory. */
    if (emu->has_http_api) {
        http_api_stop(emu->http_api);
        emu->http_api = NULL;
        emu->has_http_api = false;
    }
    if (emu->control_queue) {
        control_queue_destroy(emu->control_queue);
        emu->control_queue = NULL;
    }
    if (emu->kbd_inject_buf) {
        free(emu->kbd_inject_buf);
        emu->kbd_inject_buf = NULL;
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
    if (emu->mageco_backend) {
        mageco_set_trace(&emu->mageco, NULL);
        serial_backend_destroy(emu->mageco_backend);
        emu->mageco_backend = NULL;
        emu->has_mageco = false;
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


/* Sprint 95 (API REST Epic 4) — feed queued keystrokes (from the `keys`
 * control command / HTTP POST /keys) into the keyboard matrix, one key every
 * few frames: press+hold, then release+gap, so the ROM's per-frame scan sees
 * each key distinctly (and repeated keys are separated). Called once per frame.
 * Runs on the emulator thread, same as the queue drain that fills the buffer. */
static void feed_kbd_inject(emulator_t* emu) {
    if (!emu->kbd_inject_buf) return;
    if (emu->kbd_inject_pos >= emu->kbd_inject_len) {
        if (emu->kbd_inject_len) {          /* just finished a batch → reset */
            oric_keyboard_release_all(&emu->keyboard);
            emu->kbd_inject_len = 0;
            emu->kbd_inject_pos = 0;
            emu->kbd_inject_pressed = false;
            emu->kbd_inject_delay = 0;
        }
        return;
    }
    if (emu->kbd_inject_delay > 0) { emu->kbd_inject_delay--; return; }

    if (!emu->kbd_inject_pressed) {
        oric_keyboard_release_all(&emu->keyboard);
        oric_keyboard_press_char(&emu->keyboard,
                                 emu->kbd_inject_buf[emu->kbd_inject_pos]);
        emu->kbd_inject_pressed = true;
        emu->kbd_inject_delay = 3;          /* hold ~3 frames ≈ 60 ms */
    } else {
        oric_keyboard_release_all(&emu->keyboard);
        emu->kbd_inject_pressed = false;
        emu->kbd_inject_pos++;
        emu->kbd_inject_delay = 2;          /* gap before the next key */
    }
}

/* Read a byte for the state-triggered captures. Below the ROM/overlay window
 * ($<C000) we read raw RAM directly — no banking, no I/O side effects (the I/O
 * page $0300-$03FF is never a sensible game-state trigger, so it reads the
 * shadow RAM there, matching --dump-ram-at). At $C000+ we take the CPU view
 * (memory_read) to honour the active BASIC-ROM / overlay banking. */
static uint8_t when_read(const emulator_t* emu, uint16_t addr) {
    if (addr < 0xC000)
        return emu->memory.ram[addr];
    return memory_peek((memory_t*)&emu->memory, addr);
}

/* Symétrique de when_read pour --poke-* : écrit RAM[addr]. En dessous de $C000
 * l'écriture est directe (RAM brute, sans effet de bord) ; au-delà elle passe
 * par memory_write (overlay/banking, page I/O respectée). */
static void poke_write(emulator_t* emu, uint16_t addr, uint8_t val) {
    if (addr < 0xC000)
        emu->memory.ram[addr] = val;
    else
        memory_write(&emu->memory, addr, val);
}

static void emulator_run(emulator_t* emu) {
    /* Skip the power-on reset when a save state was restored at startup —
     * otherwise the loaded PC/cycles are wiped back to the reset vector. */
    if (!emu->startup_state_loaded)
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

#ifdef HAS_SDL2
    uint32_t frame_start_ticks = SDL_GetTicks();
#endif

#ifndef __EMSCRIPTEN__
    /* Real-time pacing deadline (--realtime, headless/no-SDL). Absolute
     * CLOCK_MONOTONIC target advanced by one PAL frame each iteration so the
     * pacing never drifts. */
    struct timespec rt_next;
    if (emu->realtime) clock_gettime(CLOCK_MONOTONIC, &rt_next);
#endif

    while (emu->running && app_should_run()) {
#ifdef HAS_SDL2
        frame_start_ticks = SDL_GetTicks();
#endif
        /* Movie record/replay: the keyboard matrix is the only deterministic
         * input. Apply this frame's state BEFORE the CPU runs so the VIA scan
         * sees it. Replay overwrites live input; record samples it. */
        if (emu->movie.mode == MOVIE_REPLAY) {
            movie_replay_frame(&emu->movie, frame_count, emu->keyboard.matrix);
        } else if (emu->movie.mode == MOVIE_RECORD) {
            movie_record_frame(&emu->movie, frame_count, emu->keyboard.matrix);
        }

        /* Execute one frame worth of CPU cycles */
        int frame_cycles = 0;
        int rendered_scanlines = 0;
        int ng_line = 0;            /* ULA-NG raster tick, full PAL frame (0-311) */
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
                } else if (emu->gdb_mode) {
                    gdb_stub_stopped((gdb_stub_t*)emu->gdb_stub, emu);
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

            /* PHI2-clocked peripherals (VIA timers, Microdisc/LOCI FDC, ACIA,
             * DTL 2000, Mageco MIDI) are now advanced per-cycle by the CPU's
             * cpu_cycle_tick() callback, in step with the bus accesses, rather
             * than in a single post-instruction batch here. */

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
            /* ULA-NG raster tick over the FULL PAL frame (0-311), decoupled from
             * the visible render (0-223). Asserts the raster IRQ line when the
             * programmed NG_RASTERLINE is crossed (level, cleared by NG_STATUS
             * ack). No-op unless unlocked + NG_MODE.b0 + raster enable. */
            while (ng_line < target_scanline && ng_line < ULA_NG_FRAME_LINES) {
                ula_ng_scanline(&emu->ula_ng, ng_line);
                if (ula_ng_irq(&emu->ula_ng)) cpu_irq_set(&emu->cpu, IRQF_ULANG);
                ng_line++;
            }
        }

        /* Flush any remaining scanlines (e.g. if CPU halted mid-frame) */
        while (rendered_scanlines < 224) {
            video_render_scanline(&emu->video, emu->memory.ram, rendered_scanlines);
            rendered_scanlines++;
        }

        total_executed += (uint64_t)frame_cycles;

        /* Headless audio sinks : render THIS frame's PSG audio ONCE via
         * ay_generate (the same routine the SDL callback uses) and feed every
         * active sink — the --audio-wav file and/or the AVI's PCM stream.
         * Generating once is essential : ay_generate consumes the PSG event
         * queue, so two calls per frame would double-drain it. Armed only in
         * headless (in GUI the SDL audio device is the generator's owner). */
        bool avi_audio = emu->headless && emu->video_avi_active && emu->video_avi_rec.has_audio;
        if (emu->audio_wav_fp || avi_audio) {
            enum { WAV_FRAME_SAMPLES = AUDIO_SAMPLE_RATE / ORIC_FRAME_RATE };
            int16_t wav_buf[WAV_FRAME_SAMPLES * 2];  /* interleaved L/R */
            ay_generate(&emu->psg, wav_buf, WAV_FRAME_SAMPLES);
            if (emu->audio_wav_fp) {
                fwrite(wav_buf, sizeof(int16_t) * 2, WAV_FRAME_SAMPLES, emu->audio_wav_fp);
                emu->audio_wav_data_bytes +=
                    (uint32_t)(WAV_FRAME_SAMPLES * 2 * (int)sizeof(int16_t));
            }
            if (avi_audio)
                avi_recorder_add_audio(&emu->video_avi_rec, wav_buf, WAV_FRAME_SAMPLES);
        }

        /* Sprint 35a freeze — async pause: once per frame, peek at stdin.
         * If the IDE sent `pause`, hand control back to the REPL right
         * after this frame ends. Latency = at most one frame (~20 ms). */
        if (emu->control_mode && control_poll_pause(emu)) {
            emu->debugger.active = true;
        }

        /* GDB stub: once per frame, check for a Ctrl-C interrupt or a client
         * disconnect; either forces a stop into gdb_stub_stopped() next loop. */
        if (emu->gdb_mode && gdb_stub_poll_interrupt((gdb_stub_t*)emu->gdb_stub)) {
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

        /* Séquençage multi --type-keys : dès que l'entrée active est terminée
         * et que le cycle d'armement de la suivante est atteint, on la charge
         * dans les champs type_keys_* actifs. Garantit un vrai relâchement
         * (release_all + reset des compteurs de debounce) entre deux entrées,
         * même à touches identiques — ce que wait_release des TUI exige. */
        if (emu->type_keys_done &&
            emu->type_keys_seq_idx < emu->type_keys_seq_count &&
            (int64_t)total_executed >= emu->type_keys_seq[emu->type_keys_seq_idx].at) {
            int s = emu->type_keys_seq_idx++;
            oric_keyboard_release_all(&emu->keyboard);
            if (emu->has_loci) loci_kbd_clear(&emu->loci);
            emu->type_keys_at = emu->type_keys_seq[s].at;
            emu->type_keys_text = emu->type_keys_seq[s].text;
            emu->type_keys_loci_hid = emu->type_keys_seq[s].loci_hid;
            emu->type_keys_idx = 0;
            emu->type_keys_next_cycle = emu->type_keys_seq[s].at;
            emu->type_keys_done = false;
            emu->type_keys_last_char = 0;
            emu->type_keys_debounce = 0;
            emu->type_keys_last_pass = emu->kbd_scan_passes;
        }

        /* --type-keys-when : arm the auto-typer the moment RAM[addr]==val,
         * instead of a guessed cycle. Fires once, and only when no other
         * auto-type text is currently in flight. Removes the need to hand-tune
         * a boot delay (cf. include/io/autotype.h rationale). */
        if (emu->type_keys_when_text && !emu->type_keys_when_done &&
            emu->type_keys_when_addr >= 0 &&
            (!emu->type_keys_text || emu->type_keys_done) &&
            when_read(emu, (uint16_t)emu->type_keys_when_addr) ==
                emu->type_keys_when_val) {
            oric_keyboard_release_all(&emu->keyboard);
            if (emu->has_loci) loci_kbd_clear(&emu->loci);
            emu->type_keys_text = emu->type_keys_when_text;
            emu->type_keys_loci_hid = emu->type_keys_when_loci_hid;
            emu->type_keys_at = (int64_t)total_executed;
            emu->type_keys_next_cycle = (int64_t)total_executed;
            emu->type_keys_idx = 0;
            emu->type_keys_done = false;
            emu->type_keys_last_char = 0;
            emu->type_keys_debounce = 0;
            emu->type_keys_last_pass = emu->kbd_scan_passes;
            emu->type_keys_when_done = true;
            log_info("Auto-type armed: RAM[$%04X]==$%02X reached at %lld cycles",
                     (unsigned)emu->type_keys_when_addr, emu->type_keys_when_val,
                     (long long)total_executed);
        }

        /* Auto-type: inject keystrokes at specified cycle count.
         * Each key is pressed for ~2 frames (40ms) then released for ~2 frames.
         * This simulates realistic typing speed for the ROM keyboard scanner. */
        if (emu->type_keys_text && !emu->type_keys_done &&
            (int64_t)total_executed >= emu->type_keys_at) {
            if (autotype_should_fire(emu->type_keys_loci_hid,
                                     (int64_t)total_executed,
                                     emu->type_keys_next_cycle,
                                     emu->kbd_scan_passes,
                                     emu->type_keys_last_pass)) {
                /* Record the scan-pass baseline for the *next* transition, so
                 * the scanner is guaranteed to observe this matrix state
                 * before it changes again (cf. include/io/autotype.h). */
                emu->type_keys_last_pass = emu->kbd_scan_passes;
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
                            case 'C': case 'F': case 'L': case 'R': {
                                /* \Cx=CTRL+x, \Lx=LEFT-shift+x, \Rx=RIGHT-shift+x,
                                 * \Fx=FUNCT+x. The LOCI MIA firmware (loci-firmware
                                 * kbd.c) exposes a raw USB HID keyboard bitmap in
                                 * XRAM: the modifiers are the standard HID bits
                                 * (LEFTCTRL=0x01, LEFTSHIFT=0x02, RIGHTSHIFT=0x20)
                                 * — firmware-exact. FUNCT has NO HID usage code and
                                 * no concept in the firmware, so it is sent as a
                                 * USB Tab (0x2B) chord by convention. */
                                char keyc = emu->type_keys_text[idx+2];
                                char lc = (keyc >= 'A' && keyc <= 'Z')
                                            ? (char)(keyc - 'A' + 'a') : keyc;
                                uint8_t khid = 0;
                                if (lc >= 'a' && lc <= 'z') khid = (uint8_t)(0x04 + (lc - 'a'));
                                else if (lc == '0') khid = 0x27;
                                else if (lc >= '1' && lc <= '9') khid = (uint8_t)(0x1E + (lc - '1'));
                                else if (lc == ' ') khid = 0x2C;
                                if (keyc == '\0' || !khid) {
                                    emu->type_keys_idx += 2;  /* dangling/unknown — skip */
                                    goto type_keys_done_frame;
                                }
                                if (esc == 'F') {
                                    uint8_t keys[6] = { 0x2B, khid, 0, 0, 0, 0 };
                                    loci_kbd_set_report(&emu->loci, 0, keys);
                                } else {
                                    uint8_t hmod = (esc == 'C') ? 0x01
                                                 : (esc == 'L') ? 0x02 : 0x20;
                                    uint8_t keys[6] = { khid, 0, 0, 0, 0, 0 };
                                    loci_kbd_set_report(&emu->loci, hmod, keys);
                                }
                                emu->type_keys_last_char = esc;
                                emu->type_keys_idx += 3;
                                emu->type_keys_debounce = 2;
                                emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME * 2;
                                goto type_keys_done_frame;
                            }
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
                } else if (c == '\\' && emu->type_keys_text[idx+1] == 'b') {
                    /* \b = DEL (backspace) — édition de ligne (readline). Presse
                     * la touche DEL (matrix 5,5) via le sentinel 0x84 de press_char. */
                    if (emu->type_keys_last_char == (char)0x84) {
                        oric_keyboard_release_all(&emu->keyboard);
                        emu->type_keys_last_char = 0;
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME;
                    } else {
                        oric_keyboard_release_all(&emu->keyboard);
                        oric_keyboard_press_char(&emu->keyboard, (char)0x84);
                        emu->type_keys_last_char = (char)0x84;
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
                } else if (c == '\\' && (emu->type_keys_text[idx+1] == 'C' ||
                                          emu->type_keys_text[idx+1] == 'F' ||
                                          emu->type_keys_text[idx+1] == 'L' ||
                                          emu->type_keys_text[idx+1] == 'R')) {
                    /* \Cx=CTRL+x, \Fx=FUNCT+x, \Lx=LEFT-shift+x, \Rx=RIGHT-shift+x.
                     * The modifier is held while the companion key x is pressed
                     * (3 chars consumed). A distinct sentinel per modifier forces
                     * a release frame between two consecutive combos so the ROM
                     * scanner sees separate keystrokes. */
                    char mod = emu->type_keys_text[idx+1];
                    char keyc = emu->type_keys_text[idx+2];
                    /* Single shared sentinel (0x90) for ALL modifier combos so
                     * that two consecutive combos — even with the same base key
                     * (e.g. \L1\R1) — are always separated by a release frame,
                     * which the ROM/app keyboard scanner needs to see as two
                     * distinct keystrokes. */
                    char sentinel = (char)0x90;
                    if (emu->type_keys_last_char == sentinel) {
                        oric_keyboard_release_all(&emu->keyboard);
                        emu->type_keys_last_char = 0;
                        emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME;
                    } else if (keyc == '\0') {
                        emu->type_keys_idx += 2;  /* dangling modifier — skip */
                    } else {
                        oric_keyboard_release_all(&emu->keyboard);
                        if (mod == 'C')      oric_keyboard_press_ctrl(&emu->keyboard);
                        else if (mod == 'F') oric_keyboard_press_funct(&emu->keyboard);
                        else if (mod == 'L') oric_keyboard_press_lshift(&emu->keyboard);
                        else                 oric_keyboard_press_rshift(&emu->keyboard);
                        oric_keyboard_press_char(&emu->keyboard, keyc);
                        emu->type_keys_last_char = sentinel;
                        emu->type_keys_idx += 3;
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
                                   (unsigned int)emu->video.native_w,
                                   (unsigned int)emu->video.native_h);
        }

        /* Drain any HTTP-API commands at this frame boundary (sprint 94). The
         * server thread parked in control_queue_submit() is unblocked here, so
         * state-mutating commands run on the emulator thread. No-op when the
         * API is disabled (control_queue is NULL). */
        control_queue_drain(emu->control_queue, emu);

        /* Inject any keystrokes queued by the `keys` command (sprint 95). */
        feed_kbd_inject(emu);

        /* Present to screen and handle events if not headless */
        if (!emu->headless) {
            /* OSD : garde une copie fraîche du charset Oric (valide en mode
             * texte) puis dessine l'overlay par-dessus le framebuffer. */
            if (!emu->video.hires_mode)
                osd_snapshot_font(&emu->osd, emu->memory.ram);
            osd_render(&emu->osd, &emu->video);
            renderer_present(&emu->video);
#ifdef HAS_SDL2
            /* Poll SDL events (keyboard, window close, etc.) */
            SDL_Event event;
            static Uint32 loci_f8_down_ms;   /* Action button hold timing */
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_QUIT:
                    emu->running = false;
                    break;
                case SDL_KEYDOWN:
                    /* OSD média (F6) : quand l'overlay est ouvert, les flèches /
                     * Entrée / Échap le pilotent et n'atteignent pas l'Oric. */
                    if (event.key.keysym.sym == SDLK_F6) {
                        osd_toggle(&emu->osd);
                        break;
                    }
                    if (emu->osd.open) {
                        int k = 0;
                        switch (event.key.keysym.sym) {
                        case SDLK_UP:       k = OSD_KEY_UP;    break;
                        case SDLK_DOWN:     k = OSD_KEY_DOWN;  break;
                        case SDLK_LEFT:     k = OSD_KEY_LEFT;  break;
                        case SDLK_RIGHT:    k = OSD_KEY_RIGHT; break;
                        case SDLK_RETURN:
                        case SDLK_KP_ENTER: k = OSD_KEY_ENTER; break;
                        case SDLK_DELETE:
                        case SDLK_BACKSPACE: k = OSD_KEY_EJECT; break;
                        case SDLK_ESCAPE:   k = OSD_KEY_ESC;   break;
                        default: break;
                        }
                        if (k) {
                            osd_action_t act = osd_key(&emu->osd, k);
                            if (act == OSD_ACTIVATE)
                                osd_do_load(emu, &emu->osd.entries[emu->osd.selected]);
                            else if (act == OSD_EJECT)
                                osd_do_eject(emu);
                            else if (act == OSD_EJECT_TAPE)
                                osd_do_eject_tape(emu);
                        }
                        break;  /* consomme l'événement */
                    }
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
                        /* Sprint 34ai: LOCI Action button (warm press).
                         * Installs the IRQ trap and triggers an interrupt so
                         * the LOCI ROM can take over. Release on KEYUP below:
                         * short = menu, held ≥ 2 s = diag ROM (firmware
                         * EXT_BTN_LONGPRESS_MS). */
                        if (emu->has_loci && !event.key.repeat) {
                            loci_f8_down_ms = SDL_GetTicks();
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
                                uint8_t b = memory_peek(&emu->memory, (uint16_t)a);
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
                    case SDLK_F12: {
                        char shot[64];
                        screenshot_unique_name(shot, sizeof(shot));
                        emu_export_image(emu, shot);
                        log_info("Screenshot saved to %s", shot);
                        break;
                    }
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
                         * save-state handler. Held ≥ 2 s = diag ROM. */
                        bool longp = SDL_GetTicks() - loci_f8_down_ms >= 2000;
                        emu->loci_button_long = longp;
                        loci_action_button_release(&emu->loci);
                        log_info("LOCI: Action button released (F8%s)",
                                 longp ? ", long press" : "");
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

        /* Cycle-triggered captures (--screenshot-at / -text-at / -ansi-at /
         * --dump-ram-at), RÉPÉTABLES : chaque entrée du tableau tire une fois
         * quand total_executed atteint son seuil. Échantillonné une fois par
         * frame comme avant (le contenu ne dépend que du cycle courant). */
        for (int ci = 0; ci < emu->timed_capture_count; ci++) {
            timed_capture_t* tc = &emu->timed_captures[ci];
            if (tc->done || tc->cycles < 0 ||
                (int64_t)total_executed < tc->cycles)
                continue;
            switch (tc->type) {
            case TCAP_IMAGE:
                log_info("Taking screenshot at %llu cycles -> %s",
                         (unsigned long long)total_executed, tc->file);
                emu_export_image(emu, tc->file);
                break;
            case TCAP_TEXT: {
                FILE* tf = fopen(tc->file, "w");
                if (tf) {
                    video_export_screen_text(emu->memory.ram, tf);
                    fclose(tf);
                    log_info("Text screenshot at %llu cycles -> %s",
                             (unsigned long long)total_executed, tc->file);
                } else {
                    log_error("Cannot open text screenshot file: %s", tc->file);
                }
                break;
            }
            case TCAP_ANSI:
                if (video_export_ascii_file(&emu->video, tc->file, 2, 2))
                    log_info("ANSI screenshot at %llu cycles -> %s",
                             (unsigned long long)total_executed, tc->file);
                else
                    log_error("Cannot write ANSI screenshot file: %s", tc->file);
                break;
            case TCAP_DUMP_RAM: {
                FILE* rf = fopen(tc->file, "wb");
                if (rf) {
                    fwrite(emu->memory.ram, 1, sizeof(emu->memory.ram), rf);
                    /* $C000-$FFFF : vue CPU (banking BASIC ROM / overlay / upper
                     * RAM). memory_read est sans effet de bord hors page I/O. */
                    for (uint32_t a = 0xC000; a <= 0xFFFF; a++) {
                        uint8_t b = memory_peek(&emu->memory, (uint16_t)a);
                        fwrite(&b, 1, 1, rf);
                    }
                    fclose(rf);
                    log_info("RAM dump (64KB, $C000-$FFFF = CPU view) at %llu cycles → %s",
                             (unsigned long long)total_executed, tc->file);
                } else {
                    log_error("Cannot open RAM dump file: %s", tc->file);
                }
                break;
            }
            }
            tc->done = true;
        }

        /* Frame dump */
        if (emu->frame_dump_dir && (frame_count % (uint64_t)emu->frame_dump_interval == 0)) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%06llu.ppm",
                     emu->frame_dump_dir, (unsigned long long)frame_count);
            emu_export_image(emu, path);
        }

        /* Video recording: append this frame to the MJPEG AVI. With
         * --export-border, composite the overscan border into a scratch buffer
         * (matches the larger geometry the recorder was opened with). */
        if (emu->video_avi_active) {
            if (emu->export_border) {
                static uint8_t avi_border_buf[VIDEO_BORDERED_MAX_W * VIDEO_BORDERED_MAX_H * 3];
                int bw = 0, bh = 0;
                video_compose_bordered(&emu->video, avi_border_buf, &bw, &bh);
                avi_recorder_add_frame(&emu->video_avi_rec, avi_border_buf);
            } else {
                avi_recorder_add_frame(&emu->video_avi_rec, emu->video.framebuffer);
            }
            /* GUI audio muxing : drain the SDL callback's PCM tap and append it
             * as this frame's audio chunk (headless feeds the stream inline
             * above, so this path is GUI-only). Buffer sized for a few frames
             * of jitter (~882 sample-frames/frame @44.1k/50fps). */
            if (!emu->headless && emu->video_avi_rec.has_audio) {
                enum { TAP_DRAIN_MAX = (AUDIO_SAMPLE_RATE / ORIC_FRAME_RATE) * 4 };
                static int16_t tap_pcm[TAP_DRAIN_MAX * 2];  /* interleaved L/R */
                int got = audio_avi_tap_drain(tap_pcm, TAP_DRAIN_MAX);
                if (got > 0)
                    avi_recorder_add_audio(&emu->video_avi_rec, tap_pcm, got);
            }
        }

        frame_count++;

        /* State-triggered captures : front montant sur RAM[addr] == val,
         * échantillonné ici en fin de frame (même cadence que les variantes
         * -at). when_read() lit la RAM brute hors overlay ($<C000, sans effet
         * de bord) et la vue CPU au-delà. --cycles reste la borne max. */
        if (!emu->screenshot_when_done && emu->screenshot_when_addr >= 0 &&
            when_read(emu, (uint16_t)emu->screenshot_when_addr) == emu->screenshot_when_val) {
            log_info("Screenshot on RAM[$%04X]==$%02X at %llu cycles -> %s",
                     (unsigned)emu->screenshot_when_addr, emu->screenshot_when_val,
                     (unsigned long long)total_executed, emu->screenshot_when_file);
            emu_export_image(emu, emu->screenshot_when_file);
            emu->screenshot_when_done = true;
        }

        if (!emu->screenshot_text_when_done && emu->screenshot_text_when_addr >= 0 &&
            when_read(emu, (uint16_t)emu->screenshot_text_when_addr) == emu->screenshot_text_when_val) {
            FILE* tf = fopen(emu->screenshot_text_when_file, "w");
            if (tf) {
                video_export_screen_text(emu->memory.ram, tf);
                fclose(tf);
                log_info("Text screenshot on RAM[$%04X]==$%02X at %llu cycles -> %s",
                         (unsigned)emu->screenshot_text_when_addr, emu->screenshot_text_when_val,
                         (unsigned long long)total_executed, emu->screenshot_text_when_file);
            } else {
                log_error("Cannot open text screenshot file: %s", emu->screenshot_text_when_file);
            }
            emu->screenshot_text_when_done = true;
        }

        if (!emu->dump_ram_when_done && emu->dump_ram_when_addr >= 0 &&
            when_read(emu, (uint16_t)emu->dump_ram_when_addr) == emu->dump_ram_when_val) {
            FILE* rf = fopen(emu->dump_ram_when_file, "wb");
            if (rf) {
                fwrite(emu->memory.ram, 1, sizeof(emu->memory.ram), rf);
                for (uint32_t a = 0xC000; a <= 0xFFFF; a++) {
                    uint8_t b = memory_peek(&emu->memory, (uint16_t)a);
                    fwrite(&b, 1, 1, rf);
                }
                fclose(rf);
                log_info("RAM dump on RAM[$%04X]==$%02X at %llu cycles -> %s",
                         (unsigned)emu->dump_ram_when_addr, emu->dump_ram_when_val,
                         (unsigned long long)total_executed, emu->dump_ram_when_file);
            } else {
                log_error("Cannot open RAM dump file: %s", emu->dump_ram_when_file);
            }
            emu->dump_ram_when_done = true;
        }

        /* Écritures déclenchées (--poke-at / --poke-when) : actionneur symétrique
         * des captures ci-dessus, échantillonné à la même cadence (fin de frame).
         * Une entrée à seuil de cycles tire dès total_executed >= at_cycles ; une
         * entrée conditionnelle tire au 1er échantillon où RAM[when_addr]==when_val.
         * Chaque poke ne tire qu'une fois (done). Plusieurs pokes au même seuil
         * s'appliquent dans l'ordre de la ligne de commande (ex. cx, cy, puis le
         * drapeau de clic) au même instant côté programme. */
        for (int pi = 0; pi < emu->poke_count; pi++) {
            struct poke_action* p = &emu->pokes[pi];
            if (p->done) continue;
            bool fire = (p->at_cycles >= 0 && (int64_t)total_executed >= p->at_cycles) ||
                        (p->when_addr >= 0 &&
                         when_read(emu, (uint16_t)p->when_addr) == p->when_val);
            if (!fire) continue;
            poke_write(emu, p->target, p->value);
            if (p->when_addr >= 0)
                log_info("Poke RAM[$%04X]=$%02X on RAM[$%04X]==$%02X at %llu cycles",
                         (unsigned)p->target, p->value,
                         (unsigned)p->when_addr, p->when_val,
                         (unsigned long long)total_executed);
            else
                log_info("Poke RAM[$%04X]=$%02X at %llu cycles",
                         (unsigned)p->target, p->value,
                         (unsigned long long)total_executed);
            p->done = true;
        }

#ifdef HAS_SDL2
        /* Frame limiter: 50 Hz PAL = 20ms per frame.
         * Without this, the emulator runs at monitor refresh rate (60 Hz+)
         * which is 20% faster than real ORIC hardware.
         * SDL_Delay has ~1ms resolution, good enough for frame pacing. */
        if (!emu->headless) {
            uint32_t frame_elapsed = SDL_GetTicks() - frame_start_ticks;
            uint32_t budget = 20;
#ifdef __EMSCRIPTEN__
            /* In the browser the C while-loop must yield to the event loop
             * each frame (Asyncify rewinds/unwinds the stack here). This both
             * paces to ~50 Hz and keeps the tab responsive. */
            emscripten_sleep(frame_elapsed < budget ? budget - frame_elapsed : 0);
#else
            if (frame_elapsed < budget) {
                SDL_Delay(budget - frame_elapsed);
            }
#endif
        }
#endif

#ifndef __EMSCRIPTEN__
        /* Real-time pacing (--realtime) for headless / no-SDL runs: the SDL
         * limiter above only runs in GUI mode, so without this a headless run
         * sprints at ~45x real time — which breaks network serial timing
         * (modem/XMODEM round-trips) and --type-keys sequencing. Sleep to the
         * absolute per-frame deadline (20 ms @ 50 Hz PAL); a frame that already
         * overran returns immediately. If we fall more than a frame behind
         * (e.g. a blocking network read), resync so we don't burst-catch-up. */
        if (emu->realtime
#ifdef HAS_SDL2
            && emu->headless
#endif
           ) {
            rt_next.tv_nsec += 20000000L;  /* 20 ms */
            if (rt_next.tv_nsec >= 1000000000L) {
                rt_next.tv_nsec -= 1000000000L;
                rt_next.tv_sec++;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t lag_ns = (now.tv_sec - rt_next.tv_sec) * 1000000000LL
                           + (now.tv_nsec - rt_next.tv_nsec);
            if (lag_ns > 20000000LL) {
                rt_next = now;  /* trop en retard : on recale sur l'instant courant */
            } else {
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &rt_next, NULL);
            }
        }
#endif

        /* Headless replay: once the movie is fully drained, exit so a recorded
         * session replays to completion unattended (CI regression). The GUI
         * keeps running so playback can be watched. */
        if (emu->headless && movie_replay_done(&emu->movie) &&
            frame_count > emu->movie.end_frame) {
            log_info("Movie replay complete (%u frames)",
                     (unsigned)emu->movie.end_frame);
            break;
        }

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
        emu_export_image(emu, emu->screenshot_file);
    }

    /* End-of-run text screenshot : contenu texte réel de l'écran ($BB80). */
    if (emu->screenshot_text_file) {
        FILE* tf = fopen(emu->screenshot_text_file, "w");
        if (tf) {
            video_export_screen_text(emu->memory.ram, tf);
            fclose(tf);
            log_info("Exit text screenshot -> %s", emu->screenshot_text_file);
        } else {
            log_error("Cannot open text screenshot file: %s", emu->screenshot_text_file);
        }
    }

    /* End-of-run ANSI screenshot : image true-color du framebuffer. */
    if (emu->screenshot_ansi_file) {
        video_render_frame(&emu->video, emu->memory.ram);
        if (video_export_ascii_file(&emu->video, emu->screenshot_ansi_file, 2, 2))
            log_info("Exit ANSI screenshot -> %s", emu->screenshot_ansi_file);
        else
            log_error("Cannot write ANSI screenshot file: %s", emu->screenshot_ansi_file);
    }

    /* Filet de sécurité : un -when armé qui n'a jamais tiré = échec franc.
     * main() renverra 2 pour que le CI le voie plutôt que de croire à une
     * capture silencieuse manquante. */
    if (!emu->screenshot_when_done && emu->screenshot_when_addr >= 0) {
        log_error("--screenshot-when: condition jamais atteinte (RAM[$%04X] != $%02X)",
                  (unsigned)emu->screenshot_when_addr, emu->screenshot_when_val);
        emu->when_condition_unmet = true;
    }
    if (!emu->screenshot_text_when_done && emu->screenshot_text_when_addr >= 0) {
        log_error("--screenshot-text-when: condition jamais atteinte (RAM[$%04X] != $%02X)",
                  (unsigned)emu->screenshot_text_when_addr, emu->screenshot_text_when_val);
        emu->when_condition_unmet = true;
    }
    if (!emu->dump_ram_when_done && emu->dump_ram_when_addr >= 0) {
        log_error("--dump-ram-when: condition jamais atteinte (RAM[$%04X] != $%02X)",
                  (unsigned)emu->dump_ram_when_addr, emu->dump_ram_when_val);
        emu->when_condition_unmet = true;
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
    const char* disk_create_file = NULL;
    const char* disk_web_url = NULL;   /* loci-webdisk archi B: disque servi par HTTP */
    bool disk_writeback = false;
    const char* rom_file = NULL;
    const char* hostfs_path = NULL;
    bool fast_load = false;
    bool tape_signal = false;   /* --tape-signal: signal-level cassette (Sprint 90) */
    bool tape_signal_free = false; /* --tape-signal-free: gate moteur sur ORB PB6 (clean-room ROM) */
    const char* tape_out_capture_arg = NULL; /* --tape-out-capture FILE: capture PB7 -> .TAP */
    bool verbose = false;
    bool headless = false;
    int64_t max_cycles = -1;
    const char* screenshot_file = NULL;
    const char* ula_ng_poke = NULL;   /* --ula-ng-poke "AAA=VV,..." (registres $0340-$035F) */
    /* Captures -at RÉPÉTABLES : on collecte (arg, type) au parsing, puis on
     * résout chaque "CYCLES:FILE" après la boucle getopt (comme --poke-at). */
    struct { const char* arg; timed_capture_type_t type; } tcap_cli[TIMED_CAPTURE_MAX];
    int tcap_cli_count = 0;
    const char* screenshot_text_file = NULL;
    const char* screenshot_ansi_file = NULL;
    const char* frame_dump_dir = NULL;
    int frame_dump_interval = 50;
    const char* video_avi_file = NULL;
    int video_avi_fps = 50;
    int video_avi_quality = 85;
    bool gdb_enabled = false;
    int gdb_port = GDB_DEFAULT_PORT;
    const char* movie_record_file = NULL;
    const char* movie_replay_file = NULL;
    const char* keyboard_layout = NULL;

    const char* type_keys_args[TYPE_KEYS_SEQ_MAX];
    int type_keys_arg_count = 0;
    /* --poke-at / --poke-when : arguments collectés pendant getopt puis parsés
     * après emulator_init (les pokes vivent dans emu, initialisé plus tard). */
    struct { const char* arg; bool is_when; } poke_args[POKE_MAX];
    int poke_arg_count = 0;
    const char* disk_rom_file = NULL;
    bool debug_mode = false;
    const char* debug_break_addr = NULL;
    bool cast_server_enabled = false;
    uint16_t cast_server_port = 0;
    /* HTTP control API (sprint 94) */
    bool http_api_enabled = false;
    uint16_t http_api_port = 0;            /* 0 → HTTP_API_DEFAULT_PORT */
    const char* http_api_bind = NULL;      /* NULL → 127.0.0.1          */
    const char* http_api_root = NULL;      /* NULL → "." (CWD)          */
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
    bool render_software = false;
    const char* trace_file = NULL;
    const char* screenshot_when_arg = NULL;
    const char* dump_ram_when_arg = NULL;
    const char* screenshot_text_when_arg = NULL;
    const char* type_keys_when_arg = NULL;
    const char* bad_sector_args[FDC_MAX_BAD_SECTORS];
    int bad_sector_arg_count = 0;
    const char* fdc_timing_arg = NULL;
    const char* loci_usb_args[LOCI_USB_DEV_MAX];
    int  loci_usb_count = 0;
    bool loci_usb_autoscan = true;
    const char* trace_irq_file = NULL;
    const char* psg_trace_file = NULL;
    const char* kbd_trace_file = NULL;
    const char* audio_wav_file = NULL;
    const char* symbols_file = NULL;
    bool tui_mode = false;
    bool control_mode = false;
    bool bench_mode = false;
    bool loci_enabled = false;
    const char* loci_flash_root = NULL;
    const char* loci_sdimg_path = NULL;
    const char* loci_web_url = NULL;   /* loci-webdisk archi B : disque web natif LOCI */
    const char* loci_web_base = NULL;  /* Route B : racine serveur pour le device « W: Web disks » */
    int loci_mia_win_lo = -1, loci_mia_win_hi = -1;  /* -1 = not set (open window) */
    int64_t trace_max = 0;
    int64_t trace_ring = 0;   /* --trace-ring N : garder les N DERNIÈRES instructions */
    const char* profile_file = NULL;
    const char* rom_info_file = NULL;
    bool rom_info_enabled = false;
    const char* serial_arg = NULL;
    const char* acia_addr_arg = NULL;
    const char* dtl2000_arg = NULL;
    const char* dtl2000_addr_arg = NULL;
    const char* mageco_arg = NULL;
    const char* mageco_addr_arg = NULL;
    bool mageco_oricon = false;
    bool serial_v23 = false;
    int serial_buffer_size = 0;
    int serial_baud = 0;
    bool serial_irq_on_rdrf = false;
    const char* serial_trace_file = NULL;
    bool serial_tcp_backpressure = false;  /* --serial-tcp-backpressure */
    int  serial_tcp_rcvbuf = 0;            /* explicit SO_RCVBUF cap (0 = auto) */
    long loci_irq_latency_us = 0;          /* --loci-irq-latency (LOCI I2C IRQ cost) */

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, CLI_SHORT_OPTIONS, long_options, &option_index)) != -1) {
        switch (opt) {
            case 't': tape_file = optarg; break;
            case 'd': disk_files[0] = optarg; break;
            case OPT_DISK1: disk_files[1] = optarg; break;
            case OPT_DISK2: disk_files[2] = optarg; break;
            case OPT_DISK3: disk_files[3] = optarg; break;
            case OPT_DISK_WRITEBACK: disk_writeback = true; break;
            case OPT_DISK_CREATE: disk_create_file = optarg; disk_writeback = true; break;
            case OPT_DISK_WEB: disk_web_url = optarg; break;
            case 'r': rom_file = optarg; break;
            case 'h': hostfs_path = optarg; break;
            case 'f': fast_load = true; break;
            case 'n': headless = true; break;
            case 'c': max_cycles = atoll(optarg); break;
            case 'v': verbose = true; break;
            case OPT_SCREENSHOT: screenshot_file = optarg; break;
            case OPT_SCREENSHOT_TEXT: screenshot_text_file = optarg; break;
            case OPT_SCREENSHOT_ANSI: screenshot_ansi_file = optarg; break;
            case OPT_SCREENSHOT_TEXT_AT: if (tcap_cli_count<TIMED_CAPTURE_MAX){tcap_cli[tcap_cli_count].arg=optarg;tcap_cli[tcap_cli_count++].type=TCAP_TEXT;} break;
            case OPT_SCREENSHOT_ANSI_AT: if (tcap_cli_count<TIMED_CAPTURE_MAX){tcap_cli[tcap_cli_count].arg=optarg;tcap_cli[tcap_cli_count++].type=TCAP_ANSI;} break;
            case OPT_ULA_NG_POKE: ula_ng_poke = optarg; break;
            case OPT_SCREENSHOT_AT: if (tcap_cli_count<TIMED_CAPTURE_MAX){tcap_cli[tcap_cli_count].arg=optarg;tcap_cli[tcap_cli_count++].type=TCAP_IMAGE;} break;
            case OPT_SCREENSHOT_WHEN: screenshot_when_arg = optarg; break;
            case OPT_SCREENSHOT_TEXT_WHEN: screenshot_text_when_arg = optarg; break;
            case OPT_DUMP_RAM_WHEN: dump_ram_when_arg = optarg; break;
            case OPT_POKE_AT:
            case OPT_POKE_WHEN:
                if (poke_arg_count >= POKE_MAX) {
                    log_error("Too many --poke-at/--poke-when (max %d)", POKE_MAX);
                    return 1;
                }
                poke_args[poke_arg_count].arg = optarg;
                poke_args[poke_arg_count].is_when = (opt == OPT_POKE_WHEN);
                poke_arg_count++;
                break;
            case OPT_TYPE_KEYS_WHEN: type_keys_when_arg = optarg; break;
            case OPT_FRAME_DUMP: frame_dump_dir = optarg; break;
            case OPT_FRAME_DUMP_INTERVAL: frame_dump_interval = atoi(optarg); break;
            case OPT_VIDEO: video_avi_file = optarg; break;
            case OPT_VIDEO_FPS: video_avi_fps = atoi(optarg); break;
            case OPT_VIDEO_QUALITY: video_avi_quality = atoi(optarg); break;
            case OPT_GDB:
                gdb_enabled = true;
                if (optarg) gdb_port = atoi(optarg);
                break;
            case OPT_RECORD: movie_record_file = optarg; break;
            case OPT_REPLAY: movie_replay_file = optarg; break;
            case 'k': keyboard_layout = optarg; break;
            case OPT_TYPE_KEYS:
                if (type_keys_arg_count < TYPE_KEYS_SEQ_MAX) {
                    type_keys_args[type_keys_arg_count++] = optarg;
                } else {
                    log_warning("Too many --type-keys (max %d), ignoring extra",
                                TYPE_KEYS_SEQ_MAX);
                }
                break;
            case OPT_DISK_ROM: disk_rom_file = optarg; break;
            case 'b': emu.breakpoint = (int32_t)strtol(optarg, NULL, 16); break;
            case 'D': debug_mode = true; break;
            case OPT_DEBUG_BREAK: debug_break_addr = optarg; break;
            case OPT_CAST_SERVER:
                cast_server_enabled = true;
                if (optarg) cast_server_port = (uint16_t)atoi(optarg);
                break;
            case OPT_HTTP_API:
                http_api_enabled = true;
                if (optarg) http_api_port = (uint16_t)atoi(optarg);
                break;
            case OPT_HTTP_API_BIND: http_api_bind = optarg; break;
            case OPT_HTTP_API_ROOT: http_api_root = optarg; break;
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
            case OPT_RENDER_SOFTWARE: render_software = true; break;
            case OPT_NO_BORDER: emu.no_border = true; break;
            case OPT_EXPORT_BORDER: emu.export_border = true; break;
            case OPT_REALTIME: emu.realtime = true; break;
            case OPT_TAPE_SIGNAL: tape_signal = true; break;
            case OPT_TAPE_SIGNAL_FREE: tape_signal = true; tape_signal_free = true; break;
            case OPT_TAPE_OUT_CAPTURE: tape_out_capture_arg = optarg; break;
            case OPT_TRACE: trace_file = optarg; break;
            case OPT_TRACE_MAX: trace_max = atoll(optarg); break;
            case OPT_TRACE_RING: trace_ring = atoll(optarg); break;
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
            case OPT_SERIAL_BAUD:
                serial_baud = atoi(optarg);
                if (serial_baud < 0) serial_baud = 0;
                break;
            case OPT_SERIAL_IRQ_RDRF:
                serial_irq_on_rdrf = true;
                break;
            case OPT_SERIAL_TRACE:
                serial_trace_file = optarg;
                break;
            case OPT_SERIAL_TCP_BACKPRESSURE:
                serial_tcp_backpressure = true;
                if (optarg) {
                    serial_tcp_rcvbuf = atoi(optarg);
                    if (serial_tcp_rcvbuf < 0) serial_tcp_rcvbuf = 0;
                }
                break;
            case OPT_LOCI_IRQ_LATENCY:
                loci_irq_latency_us = atol(optarg);
                if (loci_irq_latency_us < 0) loci_irq_latency_us = 0;
                break;
            case OPT_DUMP_RAM_AT: if (tcap_cli_count<TIMED_CAPTURE_MAX){tcap_cli[tcap_cli_count].arg=optarg;tcap_cli[tcap_cli_count++].type=TCAP_DUMP_RAM;} break;
            case OPT_BAD_SECTOR:
                if (bad_sector_arg_count < FDC_MAX_BAD_SECTORS)
                    bad_sector_args[bad_sector_arg_count++] = optarg;
                else
                    log_error("--bad-sector: map full (%d max), ignoring %s",
                              FDC_MAX_BAD_SECTORS, optarg);
                break;
            case OPT_FDC_TIMING: fdc_timing_arg = optarg; break;
            case OPT_TRACE_IRQ: trace_irq_file = optarg; break;
            case OPT_PSG_TRACE: psg_trace_file = optarg; break;
            case OPT_KBD_TRACE: kbd_trace_file = optarg; break;
            case OPT_AUDIO_WAV: audio_wav_file = optarg; break;
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
            case OPT_LOCI_WEB: loci_web_url = optarg; loci_enabled = true; break;
            case OPT_LOCI_WEB_BASE: loci_web_base = optarg; loci_enabled = true; break;
            case OPT_LOCI_USB:
                if (strcmp(optarg, "none") == 0) {
                    loci_usb_autoscan = false;
                } else if (loci_usb_count < LOCI_USB_DEV_MAX) {
                    loci_usb_args[loci_usb_count++] = optarg;
                    loci_enabled = true;
                } else {
                    log_error("--loci-usb: table full (%d max), ignoring %s",
                              LOCI_USB_DEV_MAX, optarg);
                }
                break;
            case OPT_LOCI_MIA_WINDOW: {
                /* Models the reliable tior range of a real LOCI/Oric pairing:
                 * "LO-HI" (0-31). picowifi ACIA accesses outside it corrupt. */
                int lo = 0, hi = 31;
                if (sscanf(optarg, "%d-%d", &lo, &hi) == 2) {
                    loci_mia_win_lo = lo;
                    loci_mia_win_hi = hi;
                } else {
                    log_error("--loci-mia-window: expected LO-HI (e.g. 12-18)");
                    return 1;
                }
                break;
            }
            case OPT_ACIA_ADDR:
                acia_addr_arg = optarg;
                break;
            case OPT_MAGECO:
                mageco_arg = optarg;
                break;
            case OPT_MAGECO_ADDR:
                mageco_addr_arg = optarg;
                break;
            case OPT_ORICON:
                mageco_arg = optarg;
                mageco_oricon = true;
                break;
            case OPT_DTL2000:
                dtl2000_arg = optarg;
                break;
            case OPT_DTL2000_ADDR:
                dtl2000_addr_arg = optarg;
                break;
            case '?':
            default:
                cli_print_usage(argv[0]);
                return 0;
        }
    }

    log_init(verbose ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);

    app_install_signal_handlers();
    /* Pilotage par un agent (--control) : stdout/stdin sont des *pipes*, pas un
     * terminal. Si le pair ferme/cesse de lire, une écriture d'événement
     * lèverait SIGPIPE et tuerait l'émulateur (mort par signal, invisible en
     * terminal interactif). control.c veut pourtant s'arrêter PROPREMENT via
     * ferror(stdout) — mais ce contrôle est inatteignable si SIGPIPE tue avant.
     * On l'ignore : le tuyau cassé est alors géré proprement (arrêt net). */
    oscompat_ignore_sigpipe();

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
    emu.render_software = render_software;

    if (!emulator_init(&emu)) {
        log_error("Failed to initialize emulator");
        return 1;
    }

    /* --ula-ng-poke "AAA=VV,..." : programme directement les registres ULA-NG
     * ($0340-$035F) au démarrage (déverrouillage, palette, copper, raster…),
     * sans passer par des POKE BASIC lents. Idéal pour démos/tests/captures. */
    if (ula_ng_poke) {
        const char* p = ula_ng_poke;
        int n = 0;
        while (*p) {
            unsigned addr = 0, val = 0;
            if (sscanf(p, "%x=%x", &addr, &val) == 2 &&
                ula_ng_addr_in_window((uint16_t)addr)) {
                ula_ng_write(&emu.ula_ng, (uint16_t)addr, (uint8_t)val);
                n++;
            }
            const char* comma = strchr(p, ',');
            if (!comma) break;
            p = comma + 1;
        }
        log_info("ULA-NG: %d register write(s) applied from --ula-ng-poke", n);
    }


    emu.fast_load = fast_load;

    emu.max_cycles = max_cycles;
    emu.screenshot_file = screenshot_file;
    emu.screenshot_text_file = screenshot_text_file;
    emu.screenshot_ansi_file = screenshot_ansi_file;

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
        emu.acia_base_addr = parse_hex16(acia_addr_arg);
        log_info("ACIA base address: $%04X", emu.acia_base_addr);
    } else if (loci_enabled && serial_arg) {
        /* LOCI firmware exposes its ACIA at $0380-$0383 (acia.c). Under
         * --loci, default there so LOCI client software finds it. */
        emu.acia_base_addr = 0x0380;
        log_info("ACIA base address: $0380 (LOCI default — override with --acia-addr)");
    } else {
        emu.acia_base_addr = ACIA_DEFAULT_BASE;
    }
    /* Garde-fou : sous --loci, la MIA occupe $03A0-$03BF et est routée AVANT
     * l'ACIA dans les callbacks I/O. Si l'ACIA y est forcée (--acia-addr dans
     * cette plage), la MIA la masque ET pilote le PSG/clavier → le scan clavier
     * lit du vide et get_key boucle → terminal « figé » (annuaire BBS gelé).
     * Le vrai LOCI expose son modem USB-CDC à $0380, pas dans la MIA. */
    if (loci_enabled && serial_arg &&
        emu.acia_base_addr <= LOCI_MIA_END &&
        (uint16_t)(emu.acia_base_addr + 3) >= LOCI_MIA_BASE) {
        log_warning("--acia-addr $%04X force l'ACIA dans la MIA LOCI ($%04X-$%04X) : "
                    "la MIA la masque ET casse le scan clavier (PSG) -> terminal fige.",
                    emu.acia_base_addr, LOCI_MIA_BASE, LOCI_MIA_END);
        log_warning("  Le modem LOCI (picowifi) est expose a $0380 sur le vrai LOCI : "
                    "laissez --loci SANS --acia-addr (ACIA -> $0380) et adressez $0380.");
    }
    if (serial_arg) {
        /* First try the shared transparent transports (loopback/tcp/pty/com),
         * then the ACIA-6551-specific protocol backends (Hayes modem, digitelec,
         * picowifi) that inject their own command/UART layer. */
        serial_backend_t* sb = serial_transport_create(serial_arg);
        if (!sb && (strcmp(serial_arg, "modem") == 0 ||
                    strncmp(serial_arg, "modem:", 6) == 0)) {
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
                parse_host_port(hp, host, sizeof(host), &port, 23);
            }
            sb = serial_backend_modem_create(host, port, listen_mode);
        } else if (!sb && strncmp(serial_arg, "digitelec:", 10) == 0) {
            /* digitelec:host:port — DEPRECATED behavioural model. It treats the
             * DTL 2000 as an external V23 modem hanging off the emulated ACIA
             * 6551 ($031C), which is *not* how the real card works: the actual
             * DTL 2000 is a memory-mapped PIA 6821 + ACIA 6850 at $03F8 (now
             * faithfully modelled by --dtl2000, validated against OTRM). Kept
             * functional for one cycle; steer users to the faithful option. */
            log_warning("--serial digitelec: is DEPRECATED — it models the DTL 2000 as a");
            log_warning("  6551 external modem ($031C), not the real PIA+ACIA-6850 card.");
            log_warning("  Use --dtl2000 tcp:%s for the faithful DTL 2000 card,",
                        serial_arg + 10);
            log_warning("  or --serial modem:/tcp: for a generic ACIA 6551 modem.");
            char host[256];
            uint16_t port;
            parse_host_port(serial_arg + 10, host, sizeof(host), &port, 23);
            sb = serial_backend_digitelec_create(host, port, &emu.acia);
        } else if (!sb && (strcmp(serial_arg, "picowifi") == 0 ||
                           strncmp(serial_arg, "picowifi:", 9) == 0)) {
            /* PicoWiFiModemUSB (sodiumlb) — WiFi modem exposed via LOCI.
             *   --serial picowifi                Credentials set via AT$SSID=
             *   --serial picowifi:SSID           Pre-set SSID, no password
             *   --serial picowifi:SSID:PASS      Pre-set SSID + password */
            char ssid[64] = {0};
            char pass[64] = {0};
            if (serial_arg[8] == ':') {
                const char* sp = serial_arg + 9;
                const char* colon = strchr(sp, ':');
                if (colon) {
                    size_t sl = (size_t)(colon - sp);
                    if (sl >= sizeof(ssid)) sl = sizeof(ssid) - 1;
                    memcpy(ssid, sp, sl);
                    ssid[sl] = '\0';
                    strncpy(pass, colon + 1, sizeof(pass) - 1);
                } else {
                    strncpy(ssid, sp, sizeof(ssid) - 1);
                }
            }
            sb = serial_backend_picowifi_create(ssid[0] ? ssid : NULL,
                                                pass[0] ? pass : NULL);
        } else if (!sb) {
            log_error("Unknown serial backend: %s", serial_arg);
            log_error("  loopback, tcp:host:port, pty, modem:host:port,");
            log_error("  modem:listen:port, com:baud,bits,P,stop,device,");
            log_error("  file:in[:out], digitelec:host:port, picowifi[:SSID[:PASS]]");
            emulator_cleanup(&emu);
            return 1;
        }

        if (sb) {
            /* Bounded RX (--serial-tcp-backpressure): cap the kernel socket
             * buffer BEFORE open() so it takes effect on the live fd. Default
             * cap tracks the RX FIFO depth (or 512) when N is not given. */
            if (serial_tcp_backpressure && sb->type == SERIAL_BACKEND_TCP) {
                int cap = serial_tcp_rcvbuf;
                if (cap <= 0) cap = (serial_buffer_size > 0) ? serial_buffer_size : 512;
                serial_backend_tcp_set_rcvbuf(sb, cap);
            } else if (serial_tcp_backpressure) {
                log_warning("--serial-tcp-backpressure has no effect on non-TCP backend '%s' "
                            "(only tcp: has a kernel socket buffer to bound)", serial_arg);
            }
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
                if (serial_baud > 0) {
                    acia_set_ext_clock_baud(&emu.acia, (uint32_t)serial_baud);
                }
                if (serial_irq_on_rdrf) {
                    acia_set_irq_on_rdrf(&emu.acia, true);
                }
                if (loci_irq_latency_us > 0) {
                    /* LOCI I2C IRQ transport cost. At 1 MHz, 1 µs = 1 cycle;
                     * compute from the master clock so it stays correct if the
                     * clock ever changes. LOCI-context only: warn on a bare 6551
                     * so nobody penalizes a plain ACIA card by accident. */
                    uint32_t cyc = (uint32_t)((uint64_t)loci_irq_latency_us *
                                              ORIC_CLOCK_HZ / 1000000u);
                    if (!loci_enabled) {
                        log_warning("--loci-irq-latency models a LOCI I2C artifact but "
                                    "--loci is not set; applying to the ACIA anyway "
                                    "(a real bare 6551 has no such transport cost)");
                    }
                    acia_set_irq_latency(&emu.acia, cyc);
                }
                if (serial_tcp_backpressure && sb->type == SERIAL_BACKEND_TCP) {
                    acia_set_rx_backpressure(&emu.acia, true);
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
            base = parse_hex16(dtl2000_addr_arg);
        }
        if (emu.has_microdisc) {
            log_warning("DTL 2000 at $%04X shares page 3 with the disc electronics "
                        "(Jasmin) — not faithful to coexist on real hardware", base);
        }
        /* The DTL card accepts the same *transparent* transports as --serial
         * (loopback/tcp/pty/com) — raw byte pipes for the V23 line. The DTL 2000
         * is dialled by its PIA 6821 line bit and carries raw data, so the
         * protocol-injecting backends (Hayes modem, digitelec, picowifi) are
         * intentionally excluded: a Hayes AT layer behind the DTL would be
         * unfaithful (the host software never issues AT commands). */
        serial_backend_t* db = serial_transport_create(dtl2000_arg);
        if (!db) {
            log_error("Unknown DTL 2000 transport: %s", dtl2000_arg);
            log_error("  loopback, tcp:host:port, pty, com:baud,bits,P,stop,device, file:in[:out]");
            log_error("  (the DTL is dialled via its PIA, not Hayes AT — no 'modem')");
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
                if (serial_trace_file) {
                    dtl2000_set_trace(&emu.dtl2000, serial_trace_file);
                }
                log_info("Digitelec DTL 2000 enabled at $%04X (transport: %s)",
                         base, dtl2000_arg);
            } else {
                log_error("Failed to open DTL 2000 transport: %s", dtl2000_arg);
                serial_backend_destroy(db);
            }
        }
    }

    /* Mageco / ORICON MIDI interface — MC6850 ACIA (forum t=2525).
     *   --mageco : original Mageco card, 6850 at $03FE-$03FF (thread p.1).
     *   --oricon : modern ORICON reboot (iss), 6850 at $031C-$031D + clock
     *              generator at $031E-$031F, LOCI-compatible decoding (p.3).
     * Both reuse the transparent serial backends: file: captures/replays the
     * raw MIDI stream, smf: plays a .mid into the Oric, midi: bridges a live
     * host MIDI port. The byte stream is identical to the real card. */
    if (mageco_arg) {
        uint16_t base = mageco_oricon ? MAGECO_ORICON_BASE : MAGECO_DEFAULT_BASE;
        if (mageco_addr_arg) {
            base = parse_hex16(mageco_addr_arg);
        }
        const char* mode = mageco_oricon ? "ORICON" : "Mageco";
        if (emu.has_microdisc) {
            log_warning("%s MIDI at $%04X shares page 3 with the disc electronics "
                        "— possible clash with other extensions (forum t=2525)",
                        mode, base);
        }
        if (mageco_oricon && emu.has_serial && base == emu.acia_base_addr) {
            log_warning("ORICON at $%04X overlaps the ACIA 6551 serial (--serial) "
                        "— disable one of them", base);
        }
        serial_backend_t* mb = serial_transport_create(mageco_arg);
        if (!mb) {
            log_error("Unknown %s transport: %s", mode, mageco_arg);
            log_error("  file:in[:out], smf:FILE[:loop], midi[:TARGET], loopback, tcp:host:port, pty");
            emulator_cleanup(&emu);
            return 1;
        }
        if (mb->open(mb)) {
            if (mageco_oricon) mageco_init_oricon(&emu.mageco, base);
            else               mageco_init(&emu.mageco, base);
            /* mageco_init*() zeroes the struct — re-wire the CPU IRQ hooks */
            emu.mageco.irq_set = mageco_cpu_irq_set;
            emu.mageco.irq_clr = mageco_cpu_irq_clr;
            emu.mageco.irq_userdata = &emu;
            mageco_set_backend(&emu.mageco, mb);
            emu.mageco_backend = mb;
            emu.has_mageco = true;
            if (serial_trace_file) {
                mageco_set_trace(&emu.mageco, serial_trace_file);
            }
            log_info("%s MIDI enabled at $%04X (31250 baud, transport: %s)",
                     mode, base, mageco_arg);
        } else {
            log_error("Failed to open %s transport: %s", mode, mageco_arg);
            serial_backend_destroy(mb);
        }
    }

    emu.frame_dump_dir = frame_dump_dir;
    emu.frame_dump_interval = (frame_dump_interval > 0) ? frame_dump_interval : 50;

    emu.video_avi_file = video_avi_file;
    emu.video_avi_fps = (video_avi_fps > 0) ? video_avi_fps : 50;
    emu.video_avi_quality = (video_avi_quality > 0) ? video_avi_quality : 85;
    emu.video_avi_active = false;
    if (video_avi_file) {
        /* With --export-border the recorded frames carry the overscan
         * border, so the stream geometry grows by the border on each side. */
        int avi_w = emu.export_border ? ORIC_SCREEN_W + 2 * VIDEO_BORDER_W : ORIC_SCREEN_W;
        int avi_h = emu.export_border ? ORIC_SCREEN_H + 2 * VIDEO_BORDER_H : ORIC_SCREEN_H;
        /* Audio muxing. Headless generates PCM inline (ay_generate) in the main
         * loop. GUI can't : the SDL audio callback owns the PSG generator, so we
         * tap the PCM it produces via a thread-safe ring (audio_avi_tap_*) and
         * drain it per frame. Either path declares a 44.1 kHz stereo stream. */
        bool avi_gui_audio = !emu.headless && audio_avi_tap_enable();
        int avi_arate = (emu.headless || avi_gui_audio) ? AUDIO_SAMPLE_RATE : 0;
        int avi_achan = (emu.headless || avi_gui_audio) ? 2 : 0;
        if (avi_recorder_open_av(&emu.video_avi_rec, video_avi_file,
                                 avi_w, avi_h,
                                 emu.video_avi_fps, emu.video_avi_quality,
                                 avi_arate, avi_achan)) {
            emu.video_avi_active = true;
            log_info("Video recording (MJPEG AVI%s) -> %s (%d fps, q%d)",
                     (emu.headless || avi_gui_audio) ? " + PCM audio" : "",
                     video_avi_file, emu.video_avi_fps, emu.video_avi_quality);
        } else {
            if (avi_gui_audio) audio_avi_tap_disable();
            log_error("Cannot open video file for recording: %s", video_avi_file);
        }
    }

    /* Open --trace-irq FILE */
    if (trace_irq_file) {
        FILE* fp = cli_open_out(trace_irq_file, "w", "trace-irq");
        if (!fp) { emulator_cleanup(&emu); return 1; }
        fprintf(fp, "# Phosphoric IRQ trace — Oric-1/Atmos\n");
        fprintf(fp, "# Format: <cycle> <event> <details>\n");
        fprintf(fp, "# IRQ-ENTRY: PC before, target (= vector at $FFFE/F), IFR/IER snapshot, srcmask\n");
        fprintf(fp, "# RTI: PC after RTI, P flags, SP\n");
        emu.irq_trace_fp = fp;
        emu.irq_trace_active = true;
        emu.cpu.irq_trace_fp = fp;
        log_info("IRQ trace → %s", trace_irq_file);
    }

    /* Open --psg-trace FILE (log of AY sound-register writes) */
    if (psg_trace_file) {
        FILE* fp = cli_open_out(psg_trace_file, "w", "psg-trace");
        if (!fp) { emulator_cleanup(&emu); return 1; }
        fprintf(fp, "# Phosphoric PSG trace — AY-3-8910 sound-register writes\n");
        fprintf(fp, "# Format: <cpu_cycle> R<reg>=<hex>  (reg 0-13 ; ports 14/15 = keyboard, excluded)\n");
        fprintf(fp, "# NB: reg 7 (mixer) is hammered to 7F by the keyboard scan — kept as measured.\n");
        emu.psg_trace_fp = fp;
        log_info("PSG trace → %s", psg_trace_file);
    }

    /* Open --kbd-scan-trace FILE (log of every VIA Port B keyboard read) */
    if (kbd_trace_file) {
        FILE* fp = cli_open_out(kbd_trace_file, "w", "kbd-scan-trace");
        if (!fp) { emulator_cleanup(&emu); return 1; }
        fprintf(fp, "# Phosphoric keyboard-scan trace — one line per VIA Port B ($0300) read\n");
        fprintf(fp, "# Format: <cpu_cycle> col=<0-7> reg7=<hex> reg14=<hex> matrix=<hex> PB3=<0|1>\n");
        fprintf(fp, "# reg7 bit6 must be 1 (Port A input) and matrix=FF means no key in that column.\n");
        emu.kbd_trace_fp = fp;
        log_info("Keyboard-scan trace → %s", kbd_trace_file);
    }

    /* Open --audio-wav FILE (PCM capture ; headless only to avoid racing the
     * SDL audio thread, which is the other consumer of the PSG generator). */
    if (audio_wav_file) {
        if (!emu.headless) {
            log_error("--audio-wav requires --headless (SDL audio owns the PSG generator)");
            emulator_cleanup(&emu);
            return 1;
        }
        FILE* fp = cli_open_out(audio_wav_file, "wb", "audio-wav");
        if (!fp) { emulator_cleanup(&emu); return 1; }
        wav_write_header(fp, 0);   /* placeholder — sizes patched at cleanup */
        emu.audio_wav_fp = fp;
        emu.audio_wav_data_bytes = 0;
        log_info("Audio WAV → %s (16-bit stereo %d Hz)", audio_wav_file, AUDIO_SAMPLE_RATE);
    }

    /* Enable LOCI peripheral (--loci) */
    if (loci_enabled) {
        loci_init(&emu.loci);
        emu.loci.enabled = true;
        emu.has_loci = true;
        /* Route B : base du serveur pour le pseudo-device « W: Web disks ». */
        if (loci_web_base) {
            snprintf(emu.loci.web_base, sizeof(emu.loci.web_base), "%s", loci_web_base);
            log_info("LOCI: device web « W: Web disks » -> %s (menu: opendir/readdir GET /disks)",
                     loci_web_base);
        }
        if (loci_mia_win_lo >= 0) {
            loci_set_mia_window(&emu.loci, (uint8_t)loci_mia_win_lo, (uint8_t)loci_mia_win_hi);
            log_info("LOCI MIA reliable tior window: %d-%d (picowifi ACIA $0380 "
                     "corrupted outside it; tune via MAP_TUNE_TIOR / ADJ_SCAN)",
                     emu.loci.mia_tior_lo, emu.loci.mia_tior_hi);
        }
        /* ROM-swap callback used by op 0xA0 MIA_BOOT (Sprint 34ad). */
        loci_set_rom_swap_callback(&emu.loci, loci_rom_swap_cb, &emu);
        /* Session-resume callback: menu "resume" → MIA_BOOT RESUME (Sprint 85). */
        loci_set_resume_callback(&emu.loci, loci_resume_session_cb, &emu);
        /* Live ROM poke: ADJ_SCAN progress byte polled by the menu ROM. */
        loci_set_rom_poke_callback(&emu.loci, loci_rom_poke_hook, &emu);

        /* Device list served by opendir("") (menu file browser: internal
         * storage first, then one line per mounted USB device — firmware
         * usb_set_status strings). */
        if (loci_sdimg_path) {
            struct stat st;
            char msc[64];
            double mb = (stat(loci_sdimg_path, &st) == 0)
                      ? (double)st.st_size / (1024.0 * 1024.0) : 0.0;
            if (mb >= 1024.0)
                snprintf(msc, sizeof(msc), "MSC %.1f GB PHOSPHOR SDIMG rev 1.0",
                         mb / 1024.0);
            else
                snprintf(msc, sizeof(msc), "MSC %.1f MB PHOSPHOR SDIMG rev 1.0", mb);
            loci_add_usb_device(&emu.loci, msc);
        }
        if (serial_arg && strncmp(serial_arg, "picowifi", 8) == 0) {
            /* firmware cdc.c: the picowifi enumerates as a CDC modem */
            loci_add_usb_device(&emu.loci, "CDC modem mounted");
        }
        /* Real USB keys: explicit --loci-usb DIRs, then media mounted on
         * the host (udisks: /media/$USER, /run/media/$USER). Their "N:"
         * paths are served from the host directory. NOTE: with
         * --loci-sdimg, file ops are owned by the SD image backend — the
         * keys still appear in the list but are not browsable. */
        for (int i = 0; i < loci_usb_count; i++)
            loci_attach_usb_dir(&emu, loci_usb_args[i]);
        if (loci_usb_autoscan)
            loci_scan_host_usb(&emu);
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

        /* --loci-web URL : montage NATIF LOCI d'un disque servi par HTTP en
         * lecteur A (loci-webdisk archi B). Jumeau de --disk-web (Microdisc),
         * mais sur le FDC propre de la LOCI. Les pistes MFM 6400 o sont
         * récupérées à la demande. */
        if (loci_web_url) {
            if (!loci_dsk_open_web(&emu.loci, 0, loci_web_url)) {
                log_error("--loci-web: montage du disque web impossible (%s)", loci_web_url);
                return 1;
            }
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

    /* Résolution des captures -at RÉPÉTABLES (--screenshot-at / -text-at /
     * -ansi-at / --dump-ram-at) collectées dans tcap_cli : chaque "CYCLES:FILE"
     * devient une entrée timed_captures[]. Format malformé = fatal (comme avant). */
    {
        static const char* const tcap_optname[] = {
            "screenshot-at", "screenshot-text-at", "screenshot-ansi-at", "dump-ram-at"
        };
        for (int i = 0; i < tcap_cli_count; i++) {
            int64_t cyc; const char* file;
            if (!cli_split_cycles_file(tcap_cli[i].arg, tcap_optname[tcap_cli[i].type],
                                       &cyc, &file)) {
                emulator_cleanup(&emu);
                return 1;
            }
            if (emu.timed_capture_count < TIMED_CAPTURE_MAX) {
                timed_capture_t* t = &emu.timed_captures[emu.timed_capture_count++];
                t->cycles = cyc; t->file = file; t->type = tcap_cli[i].type; t->done = false;
            }
            log_info("Capture %s armée à %lld cycles -> %s",
                     tcap_optname[tcap_cli[i].type], (long long)cyc, file);
        }
    }

    /* Parse des captures déclenchées par état ADDR:VAL:FILE */
    if (screenshot_when_arg) {
        if (!cli_split_addr_val_file(screenshot_when_arg, "screenshot-when",
                                     &emu.screenshot_when_addr, &emu.screenshot_when_val,
                                     &emu.screenshot_when_file)) {
            emulator_cleanup(&emu);
            return 1;
        }
    }
    if (screenshot_text_when_arg) {
        if (!cli_split_addr_val_file(screenshot_text_when_arg, "screenshot-text-when",
                                     &emu.screenshot_text_when_addr, &emu.screenshot_text_when_val,
                                     &emu.screenshot_text_when_file)) {
            emulator_cleanup(&emu);
            return 1;
        }
    }
    if (dump_ram_when_arg) {
        if (!cli_split_addr_val_file(dump_ram_when_arg, "dump-ram-when",
                                     &emu.dump_ram_when_addr, &emu.dump_ram_when_val,
                                     &emu.dump_ram_when_file)) {
            emulator_cleanup(&emu);
            return 1;
        }
    }
    /* --poke-at / --poke-when : parsés maintenant que emu (et emu.poke_count=0)
     * est initialisé. Chaque entrée est ajoutée à emu.pokes[] dans l'ordre de la
     * ligne de commande, préservant le regroupement voulu (ex. cx, cy, clic). */
    for (int i = 0; i < poke_arg_count; i++) {
        bool ok = poke_args[i].is_when
                    ? cli_add_poke_when(&emu, poke_args[i].arg)
                    : cli_add_poke_at(&emu, poke_args[i].arg);
        if (!ok) {
            emulator_cleanup(&emu);
            return 1;
        }
    }
    /* --type-keys-when ADDR:VAL:TEXT : arm the auto-typer when RAM[ADDR]==VAL
     * instead of guessing a boot cycle. TEXT keeps the same escapes as
     * --type-keys and may start with "loci-hid:" to route via the LOCI HID. */
    if (type_keys_when_arg) {
        const char* text = NULL;
        if (!cli_split_addr_val_file(type_keys_when_arg, "type-keys-when",
                                     &emu.type_keys_when_addr, &emu.type_keys_when_val,
                                     &text)) {
            emulator_cleanup(&emu);
            return 1;
        }
        if (strncmp(text, "loci-hid:", 9) == 0) {
            emu.type_keys_when_loci_hid = true;
            text += 9;
        }
        emu.type_keys_when_text = text;
        log_info("Auto-type armed when RAM[$%04X]==$%02X (%s): \"%s\"",
                 (unsigned)emu.type_keys_when_addr, emu.type_keys_when_val,
                 emu.type_keys_when_loci_hid ? "LOCI HID" : "ORIC matrix", text);
    }

    /* Parse --type-keys CYCLES:TEXT (Sprint 34av: TEXT may start with
     * "loci-hid:" to route keys via the LOCI HID bitmap instead of the
     * ORIC keyboard matrix — useful for automating the LOCI TUI).
     *
     * Plusieurs --type-keys peuvent être passés : ils sont empilés dans une
     * file (triée par cycle d'armement) et activés l'un après l'autre une
     * fois le précédent terminé. Cela remplace l'ancien « un seul --type-keys
     * retenu » et permet de séquencer proprement un parcours multi-écrans à
     * touches répétées (1 au cycle X, 1 au cycle Y, …). */
    for (int i = 0; i < type_keys_arg_count; i++) {
        const char* arg = type_keys_args[i];
        const char* colon = strchr(arg, ':');
        if (!colon) {
            log_error("Invalid --type-keys format. Use CYCLES:TEXT (e.g. 3000000:CLOAD\"\"\\n)");
            emulator_cleanup(&emu);
            return 1;
        }
        const char* text = colon + 1;
        bool loci_hid = false;
        if (strncmp(text, "loci-hid:", 9) == 0) {
            loci_hid = true;
            text += 9;
        }
        emu.type_keys_seq[emu.type_keys_seq_count].at = atoll(arg);
        emu.type_keys_seq[emu.type_keys_seq_count].text = text;
        emu.type_keys_seq[emu.type_keys_seq_count].loci_hid = loci_hid;
        emu.type_keys_seq_count++;
    }
    if (emu.type_keys_seq_count > 0) {
        /* Tri stable par cycle d'armement croissant (insertion : N <= 16). */
        for (int i = 1; i < emu.type_keys_seq_count; i++) {
            for (int j = i; j > 0 &&
                 emu.type_keys_seq[j].at < emu.type_keys_seq[j-1].at; j--) {
                int64_t tat = emu.type_keys_seq[j].at;
                const char* ttext = emu.type_keys_seq[j].text;
                bool thid = emu.type_keys_seq[j].loci_hid;
                emu.type_keys_seq[j] = emu.type_keys_seq[j-1];
                emu.type_keys_seq[j-1].at = tat;
                emu.type_keys_seq[j-1].text = ttext;
                emu.type_keys_seq[j-1].loci_hid = thid;
            }
        }
        /* Active la première entrée ; les suivantes le seront dans la boucle
         * d'émulation par activate-next quand leur cycle sera atteint. */
        emu.type_keys_at = emu.type_keys_seq[0].at;
        emu.type_keys_text = emu.type_keys_seq[0].text;
        emu.type_keys_loci_hid = emu.type_keys_seq[0].loci_hid;
        emu.type_keys_idx = 0;
        emu.type_keys_next_cycle = emu.type_keys_at;
        emu.type_keys_done = false;
        emu.type_keys_seq_idx = 1;
        for (int i = 0; i < emu.type_keys_seq_count; i++) {
            log_info("Auto-type[%d] at %lld cycles (%s): \"%s\"", i,
                     (long long)emu.type_keys_seq[i].at,
                     emu.type_keys_seq[i].loci_hid ? "LOCI HID" : "ORIC matrix",
                     emu.type_keys_seq[i].text);
        }
    }

    /* Create frame dump directory if specified */
    if (frame_dump_dir) {
        oscompat_mkdir(frame_dump_dir, 0755);
    }

    /* Store file paths for save state metadata */
    emu.rom_path = rom_file;
    emu.disk_path = disk_files[0];
    emu.diskrom_path = disk_rom_file;
    emu.tape_path = tape_file;

    /* Suivi par lecteur pour le write-back / l'éjection depuis l'OSD. */
    emu.disk_writeback = disk_writeback;
    for (int i = 0; i < MICRODISC_MAX_DRIVES; i++)
        emu.disk_paths[i] = disk_files[i];

    /* Load ROM if specified */
    if (rom_file) {
        log_info("Loading ROM: %s", rom_file);
        if (!memory_load_rom(&emu.memory, rom_file, 0)) {
            log_error("Failed to load ROM: %s", rom_file);
            emulator_cleanup(&emu);
            return 1;
        }
        /* Direct LOCI menu ROM boot (-r roms/loci/locirom --loci): patch
         * the firmware version/timing placeholders like the real MIA. */
        if (emu.has_loci)
            loci_patch_rom_info(&emu);
    }

    /* Guard: the base system (BASIC) ROM must be present.
     *
     * Real ORIC-1/Atmos hardware always has its BASIC ROM soldered in; the
     * Microdisc overlay EPROM is *additional*, never a replacement. Without a
     * main ROM, $C000-$FFFF (BASIC ROM area) stays zeroed: any code that maps
     * the BASIC ROM back in (e.g. a disc demo doing $0314=$06 then JMP into the
     * ROM) reads $00 = BRK and falls into the $0000 BRK loop — a confusing crash
     * that looks like a banking bug but is just a missing -r. Fail fast with a
     * clear message instead. (--load-state keeps only a warning: a state may be
     * paired with a ROM-less workflow, and the ROM area is not serialized.) */
    if (!rom_file && !load_state_file) {
        if (disk_rom_file) {
            log_error("--disk-rom requires the base system ROM (-r ROM): the "
                      "BASIC ROM area $C000-$FFFF would be empty and the machine "
                      "cannot boot (code mapping the ROM reads $00 = BRK). "
                      "Add e.g. -r roms/basic11b.rom");
            emulator_cleanup(&emu);
            return 1;
        }
        log_warning("No system ROM loaded (-r ROM): $C000-$FFFF is empty, the "
                    "machine will not boot. Specify e.g. -r roms/basic11b.rom");
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
        if (tape_signal && fast_load) {
            log_warning("--tape-signal is incompatible with -f/--fast-load; "
                        "using signal-level load");
            fast_load = false;
            emu.fast_load = false;
        }
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
                        if (tape_signal) {
                            cassette_signal_begin(&emu.cassette, emu.tapebuf,
                                                  emu.tapelen);
                            emu.cassette.free_gate = tape_signal_free;
                            log_info("Signal-level cassette enabled: %d bytes on "
                                     "CB1 waveform (real ROM read)%s", emu.tapelen,
                                     tape_signal_free ? " [free-gate ORB PB6]" : "");
                        }
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

    /* Load disks with Microdisc controller. A Microdisc ROM on its own is
     * enough to bring the controller up (a real Microdisc is present even with
     * no disk in the drives) — this enables hot-swapping a .dsk in later via
     * the OSD or the --control `load-disk` command. */
    bool any_disk = (disk_create_file != NULL) || (disk_rom_file != NULL);
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

        /* WD1793 timing profile: mechanical (real) by default, --fdc-timing
         * fast restores the legacy short delays (instant-feel loading). */
        if (fdc_timing_arg) {
            if (strcmp(fdc_timing_arg, "fast") == 0) {
                emu.microdisc.fdc.timing_mode = FDC_TIMING_FAST;
            } else if (strcmp(fdc_timing_arg, "real") == 0) {
                emu.microdisc.fdc.timing_mode = FDC_TIMING_REAL;
            } else {
                log_error("Invalid --fdc-timing '%s' (use real or fast)", fdc_timing_arg);
                emulator_cleanup(&emu);
                return 1;
            }
        }

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

        /* --disk-web URL : monte en lecteur A un disque dont les secteurs sont
         * servis par un serveur HTTP (projet loci-webdisk, architecture B). On
         * lit d'abord l'en-tête MFM_DISK distant (256 o) pour la géométrie, on
         * alloue une image à plat VIDE, et le FDC va chercher chaque piste MFM
         * de 6400 o à la demande (fidèle au chemin réel LOCI dsk_web / ATDISKRD). */
        if (disk_web_url && !emu.disks[0]) {
            uint8_t hdr[MFM_DISK_HEADER_SIZE];
            long hn = disk_http_get(disk_web_url, 0, MFM_DISK_HEADER_SIZE,
                                    hdr, sizeof(hdr));
            if (hn < 16 || memcmp(hdr, "MFM_DISK", 8) != 0) {
                log_error("--disk-web: en-tête MFM_DISK illisible depuis %s", disk_web_url);
                emulator_cleanup(&emu);
                return 1;
            }
            uint32_t sides  = hdr[8]  | ((uint32_t)hdr[9]  << 8) |
                              ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
            uint32_t tracks = hdr[12] | ((uint32_t)hdr[13] << 8) |
                              ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
            if (sides < 1) sides = 1;
            if (sides > MFM_MAX_SIDES)  sides  = MFM_MAX_SIDES;
            if (tracks > MFM_MAX_TRACKS) tracks = MFM_MAX_TRACKS;
            uint8_t spt = MFM_MAX_SECTORS;   /* 17 */

            emu.disks[0] = (sedoric_disk_t*)calloc(1, sizeof(sedoric_disk_t));
            uint32_t flat = sides * tracks * spt * SEDORIC_SECTOR_SIZE;
            if (!emu.disks[0] || !(emu.disks[0]->data = (uint8_t*)calloc(1, flat))) {
                log_error("--disk-web: allocation image à plat impossible");
                emulator_cleanup(&emu);
                return 1;
            }
            emu.disks[0]->size    = flat;
            emu.disks[0]->tracks  = (uint8_t)tracks;
            emu.disks[0]->sectors = spt;
            emu.disks[0]->sides   = (uint8_t)sides;
            emu.disks[0]->is_mfm  = false;   /* pas de write-back local */

            microdisc_set_disk(&emu.microdisc, 0, emu.disks[0]->data, emu.disks[0]->size,
                               emu.disks[0]->tracks, emu.disks[0]->sectors);
            fdc_set_web(&emu.microdisc.fdc, disk_web_url);   /* après set_disk */
            log_info("--disk-web: lecteur A servi par %s (%u faces x %u pistes x %u s., "
                     "pistes chargées à la demande)", disk_web_url, sides, tracks, spt);
        }

        /* --disk-create : monte une disquette Sedoric vierge en lecteur A et
         * l'écrit aussitôt sur FILE. INIT/format à l'intérieur ; le write-back
         * de sortie (armé avec cette option) persiste les changements. */
        if (disk_create_file && !emu.disks[0]) {
            /* Double face 42 pistes : géométrie que formate INIT B de Sedoric
             * (un blank simple face était sous-dimensionné, Sprint 66). */
            emu.disks[0] = sedoric_create_blank(SEDORIC_TRACKS, 2);
            if (!emu.disks[0]) {
                log_error("disk-create: allocation de la disquette vierge impossible");
                emulator_cleanup(&emu);
                return 1;
            }
            if (!sedoric_save(emu.disks[0], disk_create_file))
                log_error("disk-create: écriture impossible vers %s", disk_create_file);
            else
                log_info("disk-create: disquette vierge -> %s (%u octets), lecteur A",
                         disk_create_file, emu.disks[0]->size);
            microdisc_set_disk(&emu.microdisc, 0, emu.disks[0]->data, emu.disks[0]->size,
                               emu.disks[0]->tracks, emu.disks[0]->sectors);
            emu.disk_paths[0] = disk_create_file;
            emu.disk_path = disk_create_file;
        } else if (disk_create_file && emu.disks[0]) {
            log_warning("disk-create ignoré : le lecteur A est déjà occupé par -d");
        }
    }

    /* --loci-web : autoboot NATIF LOCI. Le disque web est déjà monté sur le FDC
     * de la LOCI (loci_dsk_open_web) ; il ne reste qu'à installer l'overlay ROM
     * Microdisc à $A000 — exactement ce que fait MIA_BOOT avec LOCI_BOOT_FDC —
     * pour que le code de boot lise le disque via $0310 (routé vers le FDC LOCI,
     * web-backed sous --loci) et démarre Sedoric sans passer par le menu. Le ROM
     * -r (BASIC) est déjà chargé à $C000 ; on ne touche donc que $A000. */
    if (loci_web_url && emu.has_loci) {
        char disc[512] = {0};
        const char* cand = disk_rom_file;                 /* --disk-rom si fourni */
        if ((!cand || access(cand, R_OK) != 0) && rom_file) {
            const char* slash = strrchr(rom_file, '/');   /* voisin de la ROM -r */
            if (slash) {
                snprintf(disc, sizeof(disc), "%.*s/microdis.rom",
                         (int)(slash - rom_file), rom_file);
                if (access(disc, R_OK) == 0) cand = disc;
            }
        }
        if (!cand || access(cand, R_OK) != 0) { if (access("roms/microdis.rom", R_OK) == 0) cand = "roms/microdis.rom"; }
        if (!cand || access(cand, R_OK) != 0) { if (access("microdis.rom", R_OK) == 0) cand = "microdis.rom"; }
        if (cand && access(cand, R_OK) == 0 && loci_rom_swap_cb(&emu, cand, 0xA000)) {
            log_info("--loci-web: autoboot LOCI (overlay Microdisc %s → boot depuis le FDC web)", cand);
        } else {
            log_warning("--loci-web: microdis.rom introuvable — disque monté mais "
                        "pas d'autoboot (bootez via le menu LOCI ou fournissez --disk-rom)");
        }
    }

    /* Apply --bad-sector [D:]S:T:N fault injections. Damage follows the
     * media: the maps live per drive at the controller layer (Microdisc
     * and/or LOCI) and are wiped when a new disk is inserted. Applied after
     * the initial disk loads so the injections stick to the loaded media. */
    for (int i = 0; i < bad_sector_arg_count; i++) {
        unsigned d = 0, s, trk, sec;
        int nf = sscanf(bad_sector_args[i], "%u:%u:%u:%u", &d, &s, &trk, &sec);
        if (nf == 3) { sec = trk; trk = s; s = d; d = 0; }   /* S:T:N → drive A */
        if ((nf == 3 || nf == 4) &&
            d < MICRODISC_MAX_DRIVES && s <= 1 && trk < 256 && sec >= 1 && sec < 256) {
            int rc = -1;
            if (emu.has_microdisc)
                rc = microdisc_add_bad_sector(&emu.microdisc, (uint8_t)d,
                                              (uint8_t)s, (uint8_t)trk, (uint8_t)sec);
            if (emu.has_loci) {
                int rc2 = loci_add_bad_sector(&emu.loci, (uint8_t)d,
                                              (uint8_t)s, (uint8_t)trk, (uint8_t)sec);
                if (rc != 0) rc = rc2;
            }
            if (rc == 0) {
                log_info("Bad sector injected: drive %c side %u track %u sector %u",
                         'A' + d, s, trk, sec);
            } else {
                log_error("--bad-sector %s: no disk subsystem (use -d/--disk-rom or --loci)",
                          bad_sector_args[i]);
                emulator_cleanup(&emu);
                return 1;
            }
        } else {
            log_error("Invalid --bad-sector format '%s'. Use [D:]S:T:N "
                      "(drive 0-3, side 0-1, track, sector 1-255)",
                      bad_sector_args[i]);
            emulator_cleanup(&emu);
            return 1;
        }
    }

    /* Setup debugger if requested */
    if (debug_mode) {
        emu.debugger.active = true;
        log_info("Debugger mode enabled (will break at first instruction)");
    }
    if (debug_break_addr) {
        uint16_t addr = parse_hex16(debug_break_addr);
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

    /* Initialize HTTP control API if requested (sprint 94). Creates the
     * frame-boundary command queue and starts the server thread; commands are
     * executed on this (emulator) thread when the main loop drains the queue. */
    if (http_api_enabled) {
#ifdef HAS_HTTPAPI
        emu.control_queue = control_queue_create();
        emu.http_api = emu.control_queue
            ? http_api_start(&emu, emu.control_queue, http_api_port,
                             http_api_bind, http_api_root)
            : NULL;
        if (emu.http_api) {
            emu.has_http_api = true;
        } else {
            log_error("Failed to start HTTP API server");
            if (emu.control_queue) { control_queue_destroy(emu.control_queue); emu.control_queue = NULL; }
        }
#else
        fprintf(stderr, "HTTP API not compiled in. Build with HTTPAPI=1.\n");
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
        } else {
            /* Prevent emulator_run()'s power-on cpu_reset from wiping the
             * restored PC/cycles (bug: --load-state landed back at reset,
             * cycles=0, most visible under --control). */
            emu.startup_state_loaded = true;
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
        /* --symbols annote la trace avec les labels, dans LES DEUX modes.
         * (Avant : seuls le trace conditionnel IPC/debugger l'exploitaient ; le
         * --trace streaming ignorait --symbols.) */
        bool trace_syms = (symbols_file != NULL);
        if (trace_syms) trace_set_symbols(&emu.trace, &emu.symbols);
        if (trace_ring > 0) {
            /* Mode ring/tail : garde en mémoire les N DERNIÈRES instructions
             * (idéal pour un hang profond, là où --trace-max ne garde que les
             * PREMIÈRES) et les écrit à la sortie via trace_save_ring(). */
            trace_arm(&emu.trace, TRACE_START_NOW, 0, TRACE_STOP_NONE, 0, 0,
                      (uint32_t)trace_ring, trace_syms);
            log_info("CPU trace ring armed (last %lld instructions) -> %s",
                     (long long)trace_ring, trace_file);
        } else {
            if (trace_max > 0) trace_set_max(&emu.trace, (uint64_t)trace_max);
            if (!trace_open(&emu.trace, trace_file)) {
                log_error("Failed to open trace file: %s", trace_file);
            } else if (trace_syms) {
                /* Streaming + symboles : (re)arme en mode "now" (ring_cap=0) pour
                 * activer l'annotation symbolique sur le fp déjà ouvert. */
                trace_arm(&emu.trace, TRACE_START_NOW, 0, TRACE_STOP_NONE, 0, 0,
                          0, true);
            }
        }
    }

    /* CPU performance profiler */
    profiler_init(&emu.profiler);
    if (profile_file) {
        profiler_start(&emu.profiler);
        log_info("CPU profiling enabled, report will be written to %s", profile_file);
    }

    /* Deterministic input record/replay (TAS movie). */
    if (movie_replay_file) {
        uint8_t mv_model = 0;
        if (movie_replay_open(&emu.movie, movie_replay_file, &mv_model)) {
            if ((oric_model_t)mv_model != emu.model) {
                log_warning("movie recorded for model %u but running model %u — "
                            "replay may diverge", mv_model, (unsigned)emu.model);
            }
        }
    } else if (movie_record_file) {
        movie_record_open(&emu.movie, movie_record_file, (uint8_t)emu.model);
    }

    /* GDB remote stub: open the listener and block until a client attaches,
     * then start the CPU halted so GDB drives execution from the reset vector. */
    gdb_stub_t gdb_stub;
    if (gdb_enabled) {
        if (gdb_stub_init(&gdb_stub, (uint16_t)gdb_port)) {
            emu.gdb_mode = true;
            emu.gdb_stub = &gdb_stub;
            emu.debugger.active = true;   /* stop at entry, wait for GDB */
        } else {
            log_error("GDB stub: failed to start on port %d", gdb_port);
        }
    }

#ifdef __EMSCRIPTEN__
    /* Expose the running machine to the JS virtual keyboard. */
    g_web_emu = &emu;
#endif

    /* --tape-out-capture : arme la capture de l'onde tape-OUT (PB7 piloté par
     * Timer 1). Indépendant de -t (CSAVE écrit, aucun tape d'entrée requis). En
     * mode capture, les hooks CSAVE PC-1.1 sont neutralisés (cf. tape write). */
    if (tape_out_capture_arg) {
        emu.tape_out_path = tape_out_capture_arg;
        tape_capture_begin(&emu.tape_capture);
        log_info("Tape-OUT capture armed (PB7/Timer1) -> %s", tape_out_capture_arg);
    }

    /* Run emulation */
    emulator_run(&emu);

    /* Écrit le .TAP reconstruit depuis l'onde PB7 capturée. */
    if (tape_out_capture_arg && emu.tape_capture.active) {
        FILE* tf = fopen(tape_out_capture_arg, "wb");
        if (tf) {
            if (emu.tape_capture.out_len > 0)
                fwrite(emu.tape_capture.out, 1, (size_t)emu.tape_capture.out_len, tf);
            fclose(tf);
            log_info("Tape-OUT capture written: %d bytes -> %s",
                     emu.tape_capture.out_len, tape_out_capture_arg);
        } else {
            log_error("Tape-OUT capture: cannot write %s", tape_out_capture_arg);
        }
        tape_capture_free(&emu.tape_capture);
    }

    if (gdb_enabled && emu.gdb_stub) {
        gdb_stub_close(&gdb_stub);
    }

    /* Flush a recording / free replay buffers. */
    if (emu.movie.mode != MOVIE_OFF) {
        movie_close(&emu.movie);
    }

    /* Finalize video recording (write index, back-patch sizes). */
    if (emu.video_avi_active) {
        uint32_t nframes = emu.video_avi_rec.frame_count;
        /* GUI: flush any PCM still in the tap so the tail of the sound isn't
         * dropped, then release the ring (still safe: audio device is alive). */
        if (!emu.headless && emu.video_avi_rec.has_audio) {
            enum { TAP_DRAIN_MAX = (AUDIO_SAMPLE_RATE / ORIC_FRAME_RATE) * 4 };
            static int16_t tap_pcm[TAP_DRAIN_MAX * 2];
            int got;
            while ((got = audio_avi_tap_drain(tap_pcm, TAP_DRAIN_MAX)) > 0)
                avi_recorder_add_audio(&emu.video_avi_rec, tap_pcm, got);
            audio_avi_tap_disable();
        }
        if (avi_recorder_close(&emu.video_avi_rec)) {
            log_info("Video recording finalized: %s (%u frames)",
                     video_avi_file, nframes);
        } else {
            log_error("Error finalizing video recording: %s", video_avi_file);
        }
        emu.video_avi_active = false;
    }

    /* Save state on exit if specified */
    if (save_state_file) {
        log_info("Saving state on exit: %s", save_state_file);
        savestate_save(&emu, save_state_file);
    }

    /* Write modified disk images back to their .dsk files (opt-in). A drive is
     * dirty only if the guest actually wrote a sector to it this session. The
     * original file is overwritten in place, so this is gated behind an explicit
     * flag to never clobber a .dsk by accident. */
    if (disk_writeback && emu.has_microdisc) {
        for (int i = 0; i < MICRODISC_MAX_DRIVES; i++) {
            /* disk_paths[] suit les swaps OSD ; disk_files[] ne voit qu'argv. */
            const char* path = emu.disk_paths[i];
            if (!emu.microdisc.disk_dirty[i] || !path || !emu.disks[i])
                continue;
            if (sedoric_save(emu.disks[i], path)) {
                /* Report the bytes actually written to the file: an MFM image
                 * writes its mfm_raw container, a raw image writes the flat
                 * sector buffer. (disks[i]->size is always the flat buffer.) */
                uint32_t written = emu.disks[i]->is_mfm
                                       ? emu.disks[i]->mfm_raw_size
                                       : emu.disks[i]->size;
                log_info("Disk write-back: drive %c -> %s (%u bytes)",
                         'A' + i, path, written);
            } else {
                log_error("Disk write-back failed: drive %c -> %s",
                          'A' + i, path);
            }
        }
    }

    /* Write profiler report if enabled */
    if (profile_file) {
        profiler_stop(&emu.profiler);
        profiler_report_to_file(&emu.profiler, profile_file);
    }

    /* --trace-ring : à la fin du run, écrire les N dernières instructions
     * gardées en mémoire (ordre ancien → récent) dans le fichier de trace. */
    if (trace_file && trace_ring > 0) {
        if (trace_save_ring(&emu.trace, trace_file))
            log_info("CPU trace ring saved (%u instructions) -> %s",
                     trace_ring_count(&emu.trace), trace_file);
        else
            log_warning("CPU trace ring empty (no instructions recorded) -> %s",
                        trace_file);
    }
    trace_close(&emu.trace);

    /* Un --*-when armé mais jamais déclenché = échec explicite (exit 2), pour
     * que le CI SCUMM distingue « état de jeu jamais atteint » d'une erreur
     * d'usage (exit 1) ou d'un succès (exit 0). */
    bool when_unmet = emu.when_condition_unmet;
    emulator_cleanup(&emu);
    log_cleanup();

    return when_unmet ? 2 : 0;
}
