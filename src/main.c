/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file main.c
 * @brief Phosphoric — ORIC-1 Emulator main entry point - full emulation loop
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-beta.2
 */

/* clock_gettime/CLOCK_MONOTONIC (bench timer) under strict -std=c11. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE  /* macOS: _POSIX_C_SOURCE hides the BSD extensions (MSG_DONTWAIT...) */
#endif

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
#include "io/bus_timing.h"
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
#include "storage/disk_http.h"   /* loci-webdisk archi B: disk served over HTTP */
#include "io/microdisc.h"
#include "io/loci_sdimg.h"
#include "io/io_device.h"
#include "io/io_bus.h"        /* bus table + dispatch (Epic 7/US2) */
#include "io/autotype.h"      /* scan-driven pacing of --type-keys */
#include "io/tape_patches.h" /* ROM CLOAD/CSAVE PC patches (ex-main.c) */
#include "io/loci_glue.h"    /* LOCI adapter callbacks (ex-main.c, Epic 9) */
#include "io/loci_internal.h"  /* loci_dsk_open_web (loci-webdisk archi B) */
#include "io/loci_emu.h"       /* backend emulating the real RP2040 firmware (--loci-emu) */
#include "cli/cli_usage.h"    /* cli_print_usage (Epic 7/US3) */
#include "cli/cli_parse.h"    /* cli_* parse helpers (Epic 7/US3) */
#include "cli/cli_opts.h"     /* cli_opts_t: command-line options (sprint C) */
#include "cli/cli_args.h"     /* cli_parse_args: getopt loop (sprint C) */
#include "card_module.h"       /* expansion cards as modules (ADR 0006) */
#include "iomenu_glue.h"       /* I/O peripherals menu (F1) */
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
#include "utils/cycle_trace.h"
#include "cpu/microseq.h"
#include "utils/rominfo.h"

/* System ROM of the minimal profile when -r is absent: bare ORIC-1 (BASIC 1.0),
 * or BASIC 1.1 if -m atmos asks for it. */
#define DEFAULT_SYSTEM_ROM       "roms/basic10.rom"
#define DEFAULT_SYSTEM_ROM_ATMOS "roms/basic11b.rom"
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
static void iomenu_toggle(emulator_t* emu);   /* F1 menu, defined below */

/* F1 menu open: the on-screen keyboard keys drive it (arrows,
 * RETURN, ESC, DEL, letters) instead of going to the Oric matrix. */
static void web_iomenu_key(int c) {
    int k = c == 0x80 ? IOM_KEY_UP : c == 0x81 ? IOM_KEY_DOWN : c == 0x82 ? IOM_KEY_LEFT
          : c == 0x83 ? IOM_KEY_RIGHT : c == 0x0D ? IOM_KEY_ENTER : c == 0x1B ? IOM_KEY_ESC
          : c == 0x84 ? IOM_KEY_DEL : (c > ' ' && c < 0x7F) ? c : 0;
    if (!k) return;
    iom_action_t act = iom_key(&g_web_emu->iomenu, k);
    if (iomenu_apply(g_web_emu, &act))
        iomenu_toggle(g_web_emu);
}

/* Opens / closes the peripherals menu (the page's I/O button). Returns 1 if
 * the menu is now open. */
EMSCRIPTEN_KEEPALIVE int web_iomenu_toggle(void) {
    if (!g_web_emu) return 0;
    iomenu_toggle(g_web_emu);
    return g_web_emu->iomenu.open ? 1 : 0;
}

/* Press (down=1) or release (down=0) a key. `c` is an ASCII char or one of the
 * press_char sentinels (0x0D return, 0x1B esc, 0x80-0x83 arrows). `ctrl`/`funct`
 * apply the modifier for this keystroke. Release clears the whole matrix. */
EMSCRIPTEN_KEEPALIVE void web_key(int c, int ctrl, int funct, int shift, int down) {
    if (!g_web_emu) return;
    if (g_web_emu->iomenu.open) {
        if (down) web_iomenu_key(c);
        return;
    }
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
 * bit1 = disk (WD1793 BUSY), bit2 = F1 menu open (I/O button lit). Polled
 * by the web UI. */
EMSCRIPTEN_KEEPALIVE int web_io_activity(void) {
    if (!g_web_emu) return 0;
    int bits = 0;
    if (g_web_emu->tape_readbyte_active) bits |= 1;
    if (g_web_emu->microdisc.fdc.status & FDC_ST_BUSY) bits |= 2;
    if (g_web_emu->iomenu.open) bits |= 4;
    return bits;
}

/* Side-effect-free byte read (memory_peek) for the web UI and its e2e tests
 * (e.g. reading the text screen at $BB80). -1 if the machine is not up. */
EMSCRIPTEN_KEEPALIVE int web_peek(int addr) {
    if (!g_web_emu) return -1;
    return memory_peek(&g_web_emu->memory, (uint16_t)addr);
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

/* Hot-insert a .dsk into drive (0..3) from a VFS path. Requires a disk
 * controller (Microdisc via --disk-rom, or Jasmin via --jasmin-rom). Returns 0
 * if none is active, so the JS side can fall back to a reload that brings it up.
 * No write-back: the WASM build does not enable --disk-writeback. */
EMSCRIPTEN_KEEPALIVE int web_insert_disk(int drive, const char* path) {
    if (!g_web_emu || !path) return 0;
    if (!emu_has_disk_iface(g_web_emu)) return 0;
    if (drive < 0 || drive >= emu_disk_max_drives(g_web_emu)) return 0;
    sedoric_disk_t* nd = sedoric_load(path);
    if (!nd) return 0;
    if (g_web_emu->disks[drive]) sedoric_destroy(g_web_emu->disks[drive]);
    g_web_emu->disks[drive] = nd;
    emu_disk_clear_dirty(g_web_emu, drive);
    emu_disk_wire(g_web_emu, drive, nd);
    emu_set_disk_path(g_web_emu, drive, path);
    return 1;
}
#endif /* __EMSCRIPTEN__ */

/* Forward declarations for renderer (in renderer.c) */
bool renderer_init(int scale, bool prefer_software);
void renderer_cleanup(void);
void renderer_present(video_t* vid);
void renderer_present_rgb(const uint8_t* rgb, int w, int h);
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

    /* I/O bus: registered devices (LOCI first, then ACIA, Mageco,
     * Microdisc, DTL2000, ULA-NG). Falls back to the VIA.
     * ULA-NG on read: only claims when unlocked; when locked, the window
     * falls back to the VIA mirror (indistinguishable). */
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

    /* I/O bus: registered devices (LOCI first, then ACIA, Mageco,
     * Microdisc, DTL2000, ULA-NG). The device can **decline** the write
     * (write → false): this is the case of the locked ULA-NG, which watches for the
     * 'N','G' sequence in its window but lets the neutral bytes fall through to the
     * VIA (bit-for-bit, indistinguishable). Otherwise falls back to the VIA.
     *
     * NB: the LOCI snoop on the VIA ORB write $0300 (cassette motor line,
     * cf. loci_tap_motor below) is NOT a claim — the write goes to the VIA;
     * it therefore stays off the bus, in the VIA path. */
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
    if (emu->card_on[CARD_IDX_loci] && address == 0x0300) {
        loci_tap_motor(&emu->loci, (value & 0x40) != 0);
        loci_emu_tap_motor(value);     /* co-sim: the firmware's tap_act() (no-op otherwise) */
    }
    /* --tape-signal-free: gates the signal-level cassette motor on ORB PB6 (the
     * ROM-driven motor), for clean-room ROMs whose layout does not reach
     * the tape-read PC range of 1.1 (cf. tape_patches). */
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

/* Refreshes the framebuffer before a capture.
 *
 * With the cycle-level ULA (V2-E4), the framebuffer IS already the result of the scan:
 * re-rendering it in one block would overwrite precisely what we want to see (mid-line
 * splits reflect the memory state at the cycle of each cell). So we only
 * recompose in the per-line rendering mode. */
static void emu_refresh_for_capture(emulator_t* emu) {
    if (!emu->ula_per_cycle || !cpu_microseq_enabled(&emu->cpu))
        video_render_frame(&emu->video, emu->memory.ram);
}

/* Builds a capture file name that does not overwrite an existing file.
 * Base = "screenshot", extension = ".ppm". If "screenshot.ppm" is free, it
 * is used; otherwise a local timestamp "screenshot-YYYYMMDD-HHMMSS" is inserted
 * (deterministic and meaningful), and in case of an unlikely collision within the same
 * second an index "-NN" is appended. The chosen name is written into out. */
static void screenshot_unique_name(char* out, size_t out_sz) {
    const char* base = "screenshot";
    const char* ext  = ".png";

    snprintf(out, out_sz, "%s%s", base, ext);
    if (access(out, F_OK) != 0)
        return; /* default name is free */

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
/* --loci-menu-at N: at N cycles, simulate pressing the LOCI MENU button (headless test
 * of the --loci-emu backend). 0 = disabled. Fired only once. */
static uint64_t g_loci_menu_at = 0;

static void cpu_cycle_tick(void* ctx, int cycles) {
    emulator_t* emu = (emulator_t*)ctx;
    /* --cycle-trace: one line per cycle (the recorded bus access, then the
     * internal cycles of the batch). No-op when the trace is not armed. */
    if (cycle_trace_active()) cycle_trace_cycles(&emu->cpu, cycles);
    /* VIA: exact lazy path (via_tick, ported from Neo6502Vic20 US-31) —
     * a cycle where no event can happen is only counted, applied at the
     * next access. The --cpu-legacy core passes batches: full step. */
    if (cycles == 1) via_tick(&emu->via);
    else             via_update(&emu->via, cycles);
    if (emu->cassette.signal_mode)
        cassette_tick(&emu->cassette, &emu->via, cycles);
    /* Tape-OUT capture: samples PB7 (Timer1) to rebuild the .TAP.
     * Resolution = 1 bus-tick (~1-7 cy) << bit period (416/624), no effect
     * on the 512 threshold. */
    if (emu->tape_capture.active)
        tape_capture_sample(&emu->tape_capture, &emu->via, emu->cpu.cycles);
    /* Timed bus devices (FDC/ACIA/DTL/Mageco), historical order
     * preserved in io_bus_tick (Epic 7/US5). */
    io_bus_tick(emu, cycles);
}

/* Sprint 34ax: LOCI DSK bus callbacks — reuses the level-triggered IRQF_DISK
 * and synchronises overlay/ROMDIS in the memory subsystem on each
 * CTRL write. Without it the Microdisc ROM (under LOCI MIA_BOOT FDC) stays
 * stuck after the RESTORE command — it waits for the IRQ and the switch. */
/* parse_host_port → src/utils/netutil.c (Epic 7/US1, Sprint 125). */

/* Rewrites drive @p drv's .dsk to disk if the game modified it and
 * --disk-writeback is active. Called before any swap/eject so that no
 * writes are lost. Returns true if a save took place. */
/* OSD: ejects the floppy from the target drive (prior write-back if enabled). */
static void osd_do_eject(emulator_t* emu) {
    int drv = emu->osd.disk_drive;
    if (drv < 0 || drv >= emu_disk_max_drives(emu)) drv = 0;
    switch (media_disk_eject(emu, drv)) {
    case MEDIA_NO_IFACE:
        snprintf(emu->osd.status, sizeof(emu->osd.status),
                 "Pas de lecteur (--disk-rom ou --jasmin-rom requis)");
        return;
    case MEDIA_EMPTY:
        snprintf(emu->osd.status, sizeof(emu->osd.status), "Lecteur %c deja vide", 'A' + drv);
        return;
    default:
        break;
    }
    snprintf(emu->osd.status, sizeof(emu->osd.status), "Lecteur %c ejecte", 'A' + drv);
    osd_close(&emu->osd);
}

/* OSD: ejects the cassette (frees the TAP buffer, empties the read bridge). */
static void osd_do_eject_tape(emulator_t* emu) {
    if (media_tape_eject(emu) == MEDIA_EMPTY) {
        snprintf(emu->osd.status, sizeof(emu->osd.status), "Aucune cassette");
        return;
    }
    snprintf(emu->osd.status, sizeof(emu->osd.status), "Cassette ejectee");
    osd_close(&emu->osd);
}

/* OSD hot-swap: loads the selected media into the overlay (cassette or
 * target drive floppy) without leaving the emulator. Operations shared
 * with the F1 menu (src/iomenu_glue.c). */
static void osd_do_load(emulator_t* emu, const osd_entry_t* e) {
    if (e->is_disk) {
        int drv = emu->osd.disk_drive;
        if (drv < 0 || drv >= emu_disk_max_drives(emu)) drv = 0;
        media_result_t r = media_disk_insert(emu, drv, e->path);
        if (r == MEDIA_NO_IFACE) {
            snprintf(emu->osd.status, sizeof(emu->osd.status),
                     "Pas de lecteur (--disk-rom ou --jasmin-rom requis)");
            return;
        }
        if (r != MEDIA_OK) {
            snprintf(emu->osd.status, sizeof(emu->osd.status), "Echec: %.40s", e->name);
            return;
        }
        snprintf(emu->osd.status, sizeof(emu->osd.status),
                 "Disque %c: %.28s (reboot/DIR)", 'A' + drv, e->name);
    } else {
        if (media_tape_insert(emu, e->path) != MEDIA_OK) {
            snprintf(emu->osd.status, sizeof(emu->osd.status), "Echec: %.40s", e->name);
            return;
        }
        snprintf(emu->osd.status, sizeof(emu->osd.status),
                 "Cassette: %.28s (CLOAD\"\")", e->name);
    }
    osd_close(&emu->osd);
}

/* I/O peripherals menu (F1): while it is open, the machine is frozen (main
 * loop) and sound is muted. */
static void iomenu_toggle(emulator_t* emu) {
    if (emu->iomenu.open) {
        iom_close(&emu->iomenu);
        audio_pause(false);
    } else {
        if (emu->osd.open) osd_close(&emu->osd);
        iomenu_refresh(emu);
        iom_open(&emu->iomenu);
        audio_pause(true);
    }
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

    ula_ng_init(&emu->ula_ng);   /* ULA-NG locked at startup (HCS10017 state) */

    cassette_init(&emu->cassette);

    /* Initialize keyboard */
    oric_keyboard_init(&emu->keyboard);

    /* Initialize joystick (disabled by default) */
    oric_joystick_init(&emu->joystick);

    /* Initialize printer (disabled by default) */
    oric_printer_init(&emu->printer);

    /* Card modules, present or not: idle state (ACIA 6551, DTL 2000,
     * Mageco: default address, IRQs wired). */
    card_modules_init(emu);

    /* Initialize PSG (AY-3-8912) with keyboard input callback */
    ay_init(&emu->psg, ORIC_CLOCK_HZ);
    emu->psg.porta_input = keyboard_matrix_read;
    emu->psg.userdata = emu;

    /* Wire up I/O callbacks */
    memory_set_io_callbacks(&emu->memory, io_read_callback, io_write_callback, emu);
    memory_set_io_peek(&emu->memory, io_peek_callback);  /* non-destructive observers */
    via_set_irq_callback(&emu->via, irq_callback, emu);

    /* Exposes the bus table to serialisation: devices that provide a
     * save/load hook (e.g. ULA-NG → section "UNG") persist their .ost state. */
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
    emu->video.ng_scrollx    = &emu->ula_ng.scrollx;    /* fine X scroll (§5.5) */
    emu->video.ng_scrolly    = &emu->ula_ng.scrolly;    /* fine Y scroll (§5.5) */
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
    emu->timed_capture_count = 0;   /* repeatable -at captures (see timed_capture_t) */
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
    if (emu->card_on[CARD_IDX_loci]) {
        loci_cleanup(&emu->loci);
    }
    loci_emu_stop();   /* co-sim: persists the flash (internal FS 0:) — no-op without --loci-emu */
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
    /* Close the queue first: the HTTP thread may be blocked in submit(),
     * waiting for a loop that has already stopped (CPU jam, cycle limit,
     * signal) — joining it would then wait forever (deadlock seen in
     * test-httpapi). Then stop the server (join → no more producers), and only
     * then free the queue, so no submit() can touch freed memory. */
    control_queue_shutdown(emu->control_queue);
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
    card_modules_teardown(emu);   /* card transports (ACIA, DTL 2000, Mageco) */
#ifdef HAS_SDL2
    oric_joystick_close_sdl(&emu->joystick);
#endif
    for (int i = 0; i < MICRODISC_MAX_DRIVES; i++) {
        if (emu->disks[i]) {
            sedoric_destroy(emu->disks[i]);
            emu->disks[i] = NULL;
        }
        emu_set_disk_path(emu, i, NULL);   /* copies from hot insertions */
    }
    emu_set_tape_path(emu, NULL);
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
        {
            /* Byte ≥ 0xA0 = 7-bit character + SHIFT (escape \s of `keys`); the
             * arrow/DEL sentinels stay 0x80-0x84. */
            unsigned char ic = (unsigned char)emu->kbd_inject_buf[emu->kbd_inject_pos];
            if (ic >= 0xA0) {
                oric_keyboard_press_char(&emu->keyboard, (char)(ic & 0x7F));
                oric_keyboard_press_lshift(&emu->keyboard);
            } else
                oric_keyboard_press_char(&emu->keyboard, (char)ic);
        }
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

/* Counterpart of when_read for --poke-*: writes RAM[addr]. Below $C000
 * the write is direct (raw RAM, no side effect); above it goes
 * through memory_write (overlay/banking, I/O page honoured). */
static void poke_write(emulator_t* emu, uint16_t addr, uint8_t val) {
    if (addr < 0xC000)
        emu->memory.ram[addr] = val;
    else
        memory_write(&emu->memory, addr, val);
}

/* ─── Main loop: state of a run ───
 * The counters and clocks shared by the frame hooks. They used to be
 * local variables of a 1300-line function (Epic 7, reported debt);
 * moving them here lets the loop be split into named steps, each
 * readable on its own, without changing the execution order at all. */
typedef struct {
    uint64_t total_executed;      /* cycles executed since the start of the run */
    uint64_t frame_count;         /* completed frames */
    struct timespec bench_t0;     /* --bench: wall-clock start */
#ifdef HAS_SDL2
    uint32_t frame_start_ticks;   /* 50 Hz limiter: start of the frame (SDL ms) */
#endif
#ifndef __EMSCRIPTEN__
    struct timespec rt_next;      /* --realtime: absolute deadline of the frame */
#endif
} run_state_t;

/* One frame of cycles: the per-INSTRUCTION loop (debugger, trace,
 * profiler, tape patches) around the master clock emu_step(). */
static void run_frame_instructions(emulator_t* emu, run_state_t* rs) {
    /* Execute one frame worth of CPU cycles */
    emu_clock_frame_begin(emu);   /* master clock: start (or resume) of frame */
    /* The position within the frame is that of the master clock: a savestate
     * loaded midway (F4, `state-load`) resumes there, the loop follows. */
    int frame_cycles = 0;
    int frame_start = emu->raster_cycle;
    bool vsync_triggered = false;
    while (emu->raster_cycle < CYCLES_PER_FRAME && !emu->cpu.halted) {
        if (emu->clock_resume_pending) {       /* state loaded mid-frame */
            emu_clock_resume(emu);
            frame_start = emu->raster_cycle - frame_cycles;
        }
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
                /* Refresh the cast (MJPEG) frame with the current screen
                 * before blocking in the REPL: the render loop won't run
                 * while stopped, so otherwise the stream stays frozen on an
                 * earlier frame. When debugging, the user must see the screen
                 * as it is at the breakpoint. */
                if (emu->has_cast_server) {
                    emu_refresh_for_capture(emu);
                    cast_server_push_frame(&emu->cast_server, emu->video.framebuffer,
                                           (unsigned int)emu->video.native_w,
                                           (unsigned int)emu->video.native_h);
                    /* Double broadcast: MJPEG clients (browser <img>)
                     * often display the previous frame and keep the
                     * last one « in flight ». We let the cast thread broadcast
                     * this frame (tick ~20 ms) then signal it again: the 2nd
                     * broadcast brings the breakpoint screen to the foreground. */
                    nanosleep(&(struct timespec){0, 30000000L}, NULL); /* 30 ms */
                    cast_server_push_frame(&emu->cast_server, emu->video.framebuffer,
                                           (unsigned int)emu->video.native_w,
                                           (unsigned int)emu->video.native_h);
                }
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

        /* Jasmin auto-boot (Oricutron 8912.c): while the BASIC ROM is still
         * mapped, when the ROM boot reaches a fixed PC, page in the Jasmin
         * ROM ($3FB=1) and reset — the Jasmin ROM's reset vector then boots
         * the disk. One-shot per boot. Confirmed traps: Atmos $EB78, ORIC-1
         * $E905. */
        if (emu->card_on[CARD_IDX_jasmin] && !emu->jasmin.autoboot_done && !emu->jasmin.romdis) {
            uint16_t jtrap = (emu->model == ORIC_MODEL_ATMOS) ? 0xEB78 : 0xE905;
            if (emu->cpu.PC == jtrap) {
                jasmin_write(&emu->jasmin, JASMIN_ROMDIS, 1);
                emu->memory.jasmin_romdis = emu->jasmin.romdis;
                emu->memory.jasmin_olay   = emu->jasmin.olay;
                cpu_reset(&emu->cpu);
                if (fdc_trace_enabled())
                    fprintf(stderr, "[JASMIN] autoboot @ %04X → ROMDIS, "
                            "reset PC=%04X\n", jtrap, emu->cpu.PC);
                emu->jasmin.autoboot_done = true;
            }
        }

        /* Master clock (V2-E2): one instruction, cycle by cycle, with
         * the ULA and the devices advancing in lockstep (src/emu_clock.c).
         * Replaces `cpu_step` + the scanline computation that used to live here. */
        int step = emu_step(emu);
        frame_cycles = emu->raster_cycle - frame_start;

        /* Real hardware (--loci-hw): « idle poll » — if the 6502 has not
         * touched LOCI for N cycles (wait loop in RAM/hidden ROM), the
         * backend queries the cartridge (nIRQ, nRESET, nROMDIS) to bound the
         * latency of asynchronous events to ~1 Oric ms. 0 in co-sim/stub. */
        {
            int ev = loci_emu_idle_poll(step);      /* >0: nIRQ pulses; -1: reset only */
            if (ev) {
                for (int i = 0; i < ev; i++) cpu_irq_pulse(&emu->cpu);
                if (loci_emu_reset_take() > 0) cpu_reset(&emu->cpu);
            }
        }

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

        /* Scanline rendering and the ULA-NG raster tick are now
         * emitted by the master clock, in the φ1 phase of each cycle
         * (src/emu_clock.c) — no more position computation here. */
    }

    /* Finishes the frame (remaining lines if the CPU stopped midway). */
    emu_clock_frame_end(emu);
    rs->total_executed += (uint64_t)frame_cycles;
}

/* LOCI co-sim: end-of-frame IRQ, --loci-menu-at. */
static void run_loci_frame_hooks(emulator_t* emu, uint64_t total_executed) {

    /* LOCI co-sim (--loci-emu) — EDGE model: the firmware PULSES nIRQ; the emulator
     * latches each pulse. io_bus.c drains them right after each MIA transaction
     * (SYNCHRONOUS pulses). HERE, once per frame, the remaining pulses are drained
     * and delivered as EDGE / single shot (cpu_irq_pulse) → one IRQ per pulse,
     * without a storm. nRESET stays driven by the MENU button (below).
     *
     * ⚠️ NO bounded free-run (loci_emu_tick) here: advancing the firmware with Phi2
     * held HIGH BETWEEN two transactions desynchronises the bus-service state
     * machine during a multi-step MIA operation (e.g. opening a
     * file when a .dsk is selected) → the operation never completes, the 6502
     * stays stuck on `BVC *` → FROZEN MENU. The firmware must advance ONLY IN SYNC
     * with bus transactions. Accepted consequence: no purely asynchronous nIRQ
     * (timers) outside a transaction; the synchronous nIRQ is enough. */
    if (loci_emu_active()) {
        /* Pumps the CDC↔ACIA modem exchange (ASYNCHRONOUS RX: bytes arriving from the dongle
         * outside 6502 accesses). Safe between transactions: acia_task (guest-call core0,
         * bounded) does NOT drive the bus/action-SM (≠ loci_emu_tick), it only
         * moves bytes and updates the io-page. No-op without --loci-cdc. */
        loci_emu_acia_tick();
        /* Co-simulated Microdisc: a WD command in progress (RESTORE/SEEK) must finish
         * — and pulse its IRQ — even if the 6502 no longer accesses the controller.
         * Bounded dsk_task guest-call, does not drive the bus (≠ loci_emu_tick). */
        loci_emu_dsk_tick();
        int loci_irq_pulses = loci_emu_irq_take();
        for (int i = 0; i < loci_irq_pulses; i++) cpu_irq_pulse(&emu->cpu);
        /* Real hardware (--loci-hw): LOCI drove nRESET (physical MENU button,
         * freeze) → the Oric restarts; we do the same. Always 0 in co-sim/stub. */
        if (loci_emu_reset_take() > 0) cpu_reset(&emu->cpu);
    }

    /* --loci-menu-at: simulate pressing the LOCI MENU button then reset (test).
     * loci_emu_menu_button() waits for the end of the background boot if needed. */
    if (g_loci_menu_at && total_executed >= g_loci_menu_at) {
        g_loci_menu_at = 0;   /* only once */
        log_info("LOCI-emu: --loci-menu-at → appui bouton MENU");
        if (loci_emu_menu_button())
            cpu_reset(&emu->cpu);   /* restarts in the served LOCI menu */
    }
}

/* Headless sound: one PSG generation per frame, to WAV / AVI / cast. */
static void run_headless_audio_sinks(emulator_t* emu) {
    /* Headless audio sinks : render THIS frame's PSG audio ONCE via
     * ay_generate (the same routine the SDL callback uses) and feed every
     * active sink — the --audio-wav file and/or the AVI's PCM stream.
     * Generating once is essential : ay_generate consumes the PSG event
     * queue, so two calls per frame would double-drain it. Armed only in
     * headless (in GUI the SDL audio device is the generator's owner). */
    bool avi_audio = emu->headless && emu->video_avi_active && emu->video_avi_rec.has_audio;
    /* Cast /audio in headless : the SDL callback (the GUI's audio generator +
     * cast pusher) never runs, so feed the cast ring here — the exact
     * headless counterpart of what audio_callback does in GUI. Gated on the
     * cast server being active (itself opt-in via --cast-server); no extra
     * flag. In non-CAST builds has_cast_server is always false. */
    bool cast_audio = emu->headless && emu->has_cast_server;
    if (emu->audio_wav_fp || avi_audio || cast_audio) {
        enum { WAV_FRAME_SAMPLES = AUDIO_SAMPLE_RATE / ORIC_FRAME_RATE };
        int16_t wav_buf[WAV_FRAME_SAMPLES * 2];  /* interleaved L/R */
        ay_generate(&emu->psg, wav_buf, WAV_FRAME_SAMPLES);
        /* Mix in the expansion cards' audio sources (SP0256, MEA8000…), one block. */
        audio_mix_sources(wav_buf, WAV_FRAME_SAMPLES, WAV_FRAME_SAMPLES);
        if (emu->audio_wav_fp) {
            fwrite(wav_buf, sizeof(int16_t) * 2, WAV_FRAME_SAMPLES, emu->audio_wav_fp);
            emu->audio_wav_data_bytes +=
                (uint32_t)(WAV_FRAME_SAMPLES * 2 * (int)sizeof(int16_t));
        }
        if (avi_audio)
            avi_recorder_add_audio(&emu->video_avi_rec, wav_buf, WAV_FRAME_SAMPLES);
        /* Same interleaved-stereo buffer the SDL callback pushes in GUI. */
        if (cast_audio)
            cast_server_push_audio(&emu->cast_server, wav_buf, WAV_FRAME_SAMPLES);
    }
}

/* Deferred fast-load (phases 1 and 2) and auto-CLOAD"" of the inserted tape. */
static void run_fastload_hooks(emulator_t* emu, uint64_t total_executed) {
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
     * stable (~5M cycles, ROM in READY idle loop). Cf. report
     * docs/phosphoric-autorun-timing.md from the Asteroids team. */
    if (emu->fastload_autoexec_pending && total_executed > 5000000) {
        /* The user's --type-keys only neutralises the auto-RUN
         * if it types *during* the auto-RUN window (it then drives
         * the boot itself). Keystrokes scheduled later target the
         * program's menus: the auto-RUN must take place, then the keystroke
         * queue replays behind it (cf. include/io/autotype.h). */
        int64_t autorun_at = (int64_t)total_executed + AUTOTYPE_AUTORUN_DELAY_CYCLES;
        int64_t autorun_end = autorun_at + AUTOTYPE_AUTORUN_TYPING_CYCLES;
        int64_t next_user_at = -1;
        bool user_entry_pristine = false;
        if (emu->type_keys_text && !emu->type_keys_done) {
            next_user_at = emu->type_keys_at;
            /* Active entry not started yet: it can be returned to the
             * queue to replay it after the auto-RUN. */
            user_entry_pristine = (emu->type_keys_idx == 0 &&
                                   emu->type_keys_seq_idx > 0);
        } else if (emu->type_keys_seq_idx < emu->type_keys_seq_count) {
            next_user_at = emu->type_keys_seq[emu->type_keys_seq_idx].at;
        }

        if (emu->fastload_type == 0x00 &&
            autotype_autorun_allowed(next_user_at, autorun_end)) {
            if (user_entry_pristine)
                emu->type_keys_seq_idx--;  /* returned to the queue */
            emu->type_keys_text = "RUN\\n";
            emu->type_keys_loci_hid = false;
            emu->type_keys_at = autorun_at;
            emu->type_keys_idx = 0;
            emu->type_keys_next_cycle = emu->type_keys_at;
            emu->type_keys_done = false;
            emu->type_keys_last_char = 0;
            emu->type_keys_debounce = 0;
            emu->type_keys_last_pass = emu->kbd_scan_passes;
            if (next_user_at >= 0)
                log_info("Auto-typing RUN after fast-load (phase 2) — "
                         "%d frappe(s) utilisateur rejouée(s) ensuite",
                         emu->type_keys_seq_count - emu->type_keys_seq_idx);
            else
                log_info("Auto-typing RUN after fast-load (phase 2)");
        } else if (emu->fastload_type == 0x00) {
            log_info("Auto-RUN inhibé : --type-keys programmé à %lld cycles, "
                     "dans la fenêtre de l'auto-RUN (fin %lld)",
                     (long long)next_user_at, (long long)autorun_end);
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
}

/* Arming of automatic typing: multi --type-keys sequence, --type-keys-when. */
static void run_autotype_arm(emulator_t* emu, uint64_t total_executed) {
    /* Multi --type-keys sequencing: as soon as the active entry is finished
     * and the arming cycle of the next one is reached, it is loaded
     * into the active type_keys_* fields. Guarantees a real release
     * (release_all + reset of the debounce counters) between two entries,
     * even with identical keys — which the TUIs' wait_release requires. */
    if (emu->type_keys_done &&
        emu->type_keys_seq_idx < emu->type_keys_seq_count &&
        (int64_t)total_executed >= emu->type_keys_seq[emu->type_keys_seq_idx].at) {
        int s = emu->type_keys_seq_idx++;
        oric_keyboard_release_all(&emu->keyboard);
        if (emu->card_on[CARD_IDX_loci]) loci_kbd_clear(&emu->loci);
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
        if (emu->card_on[CARD_IDX_loci]) loci_kbd_clear(&emu->loci);
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
}

/* One automatic-typing step (native matrix or LOCI HID), paced by the
 * keyboard scanner — cf. include/io/autotype.h. */
/* Automatic typing, LOCI HID path (sprint 34av): each character or
 * escape sequence becomes a HID usage pushed into the LOCI keyboard
 * bitmap for ~2 frames, then released. */
static void autotype_step_loci_hid(emulator_t* emu, uint64_t total_executed,
                                   int idx, char c) {
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
                    return;
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
                return;
            }
            case 'p': {
                int secs = emu->type_keys_text[idx+2] - '0';
                if (secs < 1) secs = 1;
                if (secs > 9) secs = 9;
                loci_kbd_clear(&emu->loci);
                emu->type_keys_idx += 3;
                emu->type_keys_next_cycle = (int64_t)total_executed + ORIC_CLOCK_HZ * secs;
                return;
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
}

/* Automatic typing, native path: the ORIC keyboard matrix, with a release
 * frame between two identical keys so that the ROM scanner tells them apart. */
static void autotype_step_native(emulator_t* emu, uint64_t total_executed,
                                 int idx, char c) {
    if (c == '\0') {
        /* Done typing */
        oric_keyboard_release_all(&emu->keyboard);
        emu->type_keys_done = true;
    } else if (c == '\\' && emu->type_keys_text[idx+1] == 'n') {
        /* \n = RETURN. If two consecutive \n, insert a release
         * frame between them: otherwise the ROM scanner sees a
         * single long press instead of two distinct RETURNs.
         * last_char is reused without touching type_keys_debounce
         * (which is reserved for the ordinary character branch). */
        if (emu->type_keys_last_char == '\n') {
            oric_keyboard_release_all(&emu->keyboard);
            emu->type_keys_last_char = 0;
            /* idx not advanced: this \n will be processed again on the next frame */
            emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME;
        } else {
            oric_keyboard_release_all(&emu->keyboard);
            oric_keyboard_press_char(&emu->keyboard, '\n');
            emu->type_keys_last_char = '\n';
            emu->type_keys_idx += 2;
            emu->type_keys_next_cycle = (int64_t)total_executed + CYCLES_PER_FRAME * 4;
        }
    } else if (c == '\\' && emu->type_keys_text[idx+1] == 'e') {
        /* Sprint 34av: \e = ESC. Useful key for the LOCI TUI. */
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
        /* \b = DEL (backspace) — line editing (readline). Presses
         * the DEL key (matrix 5,5) via the 0x84 sentinel of press_char. */
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
        /* Sprint 34av: arrows for LOCI TUI navigation. */
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

static void run_autotype_step(emulator_t* emu, uint64_t total_executed) {
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
            if (emu->type_keys_loci_hid && emu->card_on[CARD_IDX_loci]) {
                autotype_step_loci_hid(emu, total_executed, idx, c);
            } else {
                autotype_step_native(emu, total_executed, idx, c);
            }
        }
    }
}

/* GUI: OSD, SDL presentation and events (keyboard, F-keys, mouse, gamepad). */
#ifdef HAS_SDL2
/* LOCI Action button (F8): press instant, to distinguish short / long. */
static Uint32 loci_f8_down_ms;

/* F1 menu open: every key goes to the menu (nothing reaches the Oric). */
static bool sdl_iomenu_key(emulator_t* emu, SDL_Keycode sym) {
    if (!emu->iomenu.open) return false;
    int k = 0;
    switch (sym) {
    case SDLK_UP:        k = IOM_KEY_UP;    break;
    case SDLK_DOWN:      k = IOM_KEY_DOWN;  break;
    case SDLK_LEFT:      k = IOM_KEY_LEFT;  break;
    case SDLK_RIGHT:     k = IOM_KEY_RIGHT; break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  k = IOM_KEY_ENTER; break;
    case SDLK_ESCAPE:    k = IOM_KEY_ESC;   break;
    case SDLK_DELETE:
    case SDLK_BACKSPACE: k = IOM_KEY_DEL;   break;
    case SDLK_HOME:      k = IOM_KEY_HOME;  break;
    case SDLK_END:       k = IOM_KEY_END;   break;
    case SDLK_PAGEUP:    k = IOM_KEY_PGUP;  break;
    case SDLK_PAGEDOWN:  k = IOM_KEY_PGDN;  break;
    default:
        /* letter: jump to the initial; while editing, the text arrives via
         * SDL_TEXTINPUT (capitals, symbols). */
        if (sym > ' ' && sym < 0x7F && !emu->iomenu.editing) k = (int)sym;
        break;
    }
    if (k) {
        iom_action_t act = iom_key(&emu->iomenu, k);
        if (iomenu_apply(emu, &act))
            iomenu_toggle(emu);                 /* Resume / Reset / snapshot restored */
    }
    return true;
}

static bool sdl_osd_key(emulator_t* emu, SDL_Keycode sym) {
    if (!emu->osd.open) return false;
    int k = 0;
    switch (sym) {
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
    return true;  /* event consumed */
}

/* Emulator function keys (F2/F4 savestate, F3 scale, F5 reset,
 * F7 memory dump, F8 LOCI button, F9 debugger, F10 quit, F11 fullscreen,
 * F12 capture). The other keys then go to the ORIC keyboard / gamepad. */
/* LOCI button without F8: on many laptops F8 is also a multimedia key
 * (volume) that GNOME grabs BEFORE the application as soon as the
 * Fn lock toggles — and F12 (screenshot here) is precisely the
 * Fn-Lock key of some keyboards. Ctrl+Alt+M = short press (menu),
 * Ctrl+Alt+D = long press (diagnostic ROM); never passed to the Oric. */
static bool sdl_loci_button_chord(emulator_t* emu, SDL_Keycode sym, uint16_t mod) {
    if (!emu->card_on[CARD_IDX_loci]) return false;
    if (!(mod & KMOD_CTRL) || !(mod & KMOD_ALT)) return false;
    if (sym != SDLK_m && sym != SDLK_d) return false;
    bool longp = (sym == SDLK_d);
    log_info("LOCI: Action button via Ctrl+Alt+%c (%s)", longp ? 'D' : 'M',
             longp ? "long press" : "short press");
    if (loci_emu_active()) {
        bool armed = longp ? loci_emu_diag_button() : loci_emu_menu_button();
        if (armed) cpu_reset(&emu->cpu);
    } else {
        emu->loci_button_long = longp;
        loci_action_button_short(&emu->loci);
        loci_action_button_release(&emu->loci);
    }
    return true;
}

static void sdl_function_key(emulator_t* emu, SDL_Keycode sym, bool repeat,
                             uint64_t total_executed) {
    switch (sym) {
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
        if (emu->card_on[CARD_IDX_loci]) {
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
        if (emu->card_on[CARD_IDX_loci] && !repeat) {
            loci_f8_down_ms = SDL_GetTicks();
            /* Co-simulation (--loci-emu): the REAL firmware owns the
             * button — the internal model must not swap its ROM in parallel
             * (the two trod on each other: diagnostic ROM loaded by
             * one, menu served by the other → frozen « Booting »). The firmware
             * is only called on release, when the duration is known. */
            if (!loci_emu_active())
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
            /* $C000-$FFFF: banked CPU view (same 64 KB contract
             * as --dump-ram-at, cf. sprint 38) */
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
}

/* SDL mouse → LOCI mou_xram (sprint 34al). Only concerns --loci. */
static void sdl_mouse_event(emulator_t* emu, const SDL_Event* event) {
    switch (event->type) {
    case SDL_MOUSEMOTION:
        if (emu->card_on[CARD_IDX_loci]) {
            uint32_t bs = SDL_GetMouseState(NULL, NULL);
            uint8_t btn = 0;
            if (bs & SDL_BUTTON(SDL_BUTTON_LEFT))   btn |= 0x01;
            if (bs & SDL_BUTTON(SDL_BUTTON_RIGHT))  btn |= 0x02;
            if (bs & SDL_BUTTON(SDL_BUTTON_MIDDLE)) btn |= 0x04;
            loci_mou_report(&emu->loci, btn,
                            (int8_t)event->motion.xrel,
                            (int8_t)event->motion.yrel,
                            0, 0);
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (emu->card_on[CARD_IDX_loci]) {
            uint32_t bs = SDL_GetMouseState(NULL, NULL);
            uint8_t btn = 0;
            if (bs & SDL_BUTTON(SDL_BUTTON_LEFT))   btn |= 0x01;
            if (bs & SDL_BUTTON(SDL_BUTTON_RIGHT))  btn |= 0x02;
            if (bs & SDL_BUTTON(SDL_BUTTON_MIDDLE)) btn |= 0x04;
            loci_mou_report(&emu->loci, btn, 0, 0, 0, 0);
        }
        break;
    case SDL_MOUSEWHEEL:
        if (emu->card_on[CARD_IDX_loci]) {
            loci_mou_report(&emu->loci, 0, 0, 0,
                            (int8_t)event->wheel.y,
                            (int8_t)event->wheel.x);
        }
        break;
    default: break;
    }
}
#endif /* HAS_SDL2 */

static void run_present_and_events(emulator_t* emu, uint64_t total_executed) {
    /* Present to screen and handle events if not headless */
    if (!emu->headless) {
        /* OSD: keeps a fresh copy of the Oric charset (valid in text
         * mode) then draws the overlay on top of the framebuffer. */
        if (!emu->video.hires_mode)
            osd_snapshot_font(&emu->osd, emu->memory.ram);
        if (emu->iomenu.open) {
            /* F1 menu: full screen, in place of the machine image. */
            static iom_surface_t iom_surf;
            static uint8_t iom_rgb[IOM_WIDTH * IOM_HEIGHT * 3];
            iomenu_refresh(emu);
            iom_draw(&emu->iomenu, &iom_surf);
            iom_rasterize(&iom_surf, iom_rgb);
            renderer_present_rgb(iom_rgb, IOM_WIDTH, IOM_HEIGHT);
        } else {
            osd_render(&emu->osd, &emu->video);
            renderer_present(&emu->video);
        }
#ifdef HAS_SDL2
        /* Poll SDL events (keyboard, window close, etc.) */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                emu->running = false;
                break;
            case SDL_KEYDOWN:
                /* Peripherals menu (F1): toggle; while open, it takes
                 * every key. */
                if (event.key.keysym.sym == SDLK_F1 && !event.key.repeat) {
                    iomenu_toggle(emu);
                    break;
                }
                if (sdl_iomenu_key(emu, event.key.keysym.sym))
                    break;
                if (event.key.keysym.sym == SDLK_F6) {
                    osd_toggle(&emu->osd);
                    break;
                }
                if (sdl_osd_key(emu, event.key.keysym.sym))
                    break;  /* event consumed */
                /* F5 = Reset, F10 = Quit, F11 = Fullscreen, F12 = Screenshot */
                if (!event.key.repeat &&
                    sdl_loci_button_chord(emu, event.key.keysym.sym, event.key.keysym.mod))
                    break;  /* consumed: does not go to the Oric keyboard */
                sdl_function_key(emu, event.key.keysym.sym, event.key.repeat != 0,
                                 total_executed);
                /* Fall through to keyboard/joystick handler */
                if (!oric_joystick_handle_sdl_event(&emu->joystick, &event)) {
                    oric_keyboard_handle_sdl_event(&emu->keyboard, &event);
                }
                /* Sprint 34ak: mirror SDL keyboard state into the
                 * LOCI kbd bitmap so the LOCI ROM TUI can navigate. */
                loci_sync_kbd_from_sdl(emu);
                break;
            case SDL_KEYUP:
                if (event.key.keysym.sym == SDLK_F8 && emu->card_on[CARD_IDX_loci]) {
                    bool longp = SDL_GetTicks() - loci_f8_down_ms >= 2000;
                    log_info("LOCI: Action button released (F8%s)",
                             longp ? ", long press" : "");
                    if (loci_emu_active()) {
                        /* Co-simulation: the real firmware handles the press —
                         * short = LOCI menu, long (≥ 2 s) = its embedded diagnostic
                         * ROM (EXT_BOOT_DIAG). Then reset of the 6502, which
                         * restarts on the vector served by LOCI. */
                        bool armed = longp ? loci_emu_diag_button()
                                           : loci_emu_menu_button();
                        if (armed) cpu_reset(&emu->cpu);
                    } else {
                        /* Internal model (sprint 34ai): the release sets V,
                         * the BVC spin exits and JMP ($FFFA) runs the session
                         * save handler. Hold ≥ 2 s = diag ROM. */
                        emu->loci_button_long = longp;
                        loci_action_button_release(&emu->loci);
                    }
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
                if (emu->iomenu.open) {        /* F1 menu: no typing to the Oric */
                    if (emu->iomenu.editing) iom_text(&emu->iomenu, event.text.text);
                    break;
                }
                oric_keyboard_handle_sdl_event(&emu->keyboard, &event);
                break;
            /* Sprint 34al: bridge SDL mouse → LOCI mou_xram. */
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEWHEEL:
                sdl_mouse_event(emu, &event);
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
}

/* Cycle-threshold captures (--screenshot-at / -text-at / -ansi-at / --dump-ram-at). */
static void run_timed_captures(emulator_t* emu, uint64_t total_executed) {
    /* Cycle-triggered captures (--screenshot-at / -text-at / -ansi-at /
     * --dump-ram-at), REPEATABLE: each array entry fires once
     * when total_executed reaches its threshold. Sampled once per
     * frame as before (the content only depends on the current cycle). */
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
                /* $C000-$FFFF: CPU view (BASIC ROM / overlay / upper
                 * RAM banking). memory_read has no side effect outside the I/O page. */
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
}

/* AVI recording: frame image + sound (SDL tap in GUI). */
static void run_video_recording(emulator_t* emu) {
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
}

/* Captures conditionnelles (--screenshot-when / -text-when / --dump-ram-when). */
static void run_when_captures(emulator_t* emu, uint64_t total_executed) {
    /* State-triggered captures: rising edge on RAM[addr] == val,
     * sampled here at end of frame (same rate as the
     * -at variants). when_read() reads raw RAM outside the overlay ($<C000, no side
     * effect) and the CPU view above. --cycles remains the max bound. */
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
}

/* Triggered writes (--poke-at / --poke-when). */
static void run_pokes(emulator_t* emu, uint64_t total_executed) {
    /* Triggered writes (--poke-at / --poke-when): actuator symmetric to
     * the captures above, sampled at the same rate (end of frame).
     * A cycle-threshold entry fires as soon as total_executed >= at_cycles; a
     * conditional entry fires on the 1st sample where RAM[when_addr]==when_val.
     * Each poke fires only once (done). Several pokes at the same threshold
     * are applied in command-line order (e.g. cx, cy, then the
     * click flag) at the same instant from the program's point of view. */
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
}

/* Pacing: 50 Hz limiter in GUI, --realtime in headless. */
static void run_frame_pacing(emulator_t* emu, run_state_t* rs) {
#ifdef HAS_SDL2
    /* Frame limiter: 50 Hz PAL = 20ms per frame.
     * Without this, the emulator runs at monitor refresh rate (60 Hz+)
     * which is 20% faster than real ORIC hardware.
     * SDL_Delay has ~1ms resolution, good enough for frame pacing. */
    if (!emu->headless) {
        uint32_t frame_elapsed = SDL_GetTicks() - rs->frame_start_ticks;
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
        rs->rt_next.tv_nsec += 20000000L;  /* 20 ms */
        if (rs->rt_next.tv_nsec >= 1000000000L) {
            rs->rt_next.tv_nsec -= 1000000000L;
            rs->rt_next.tv_sec++;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t lag_ns = (now.tv_sec - rs->rt_next.tv_sec) * 1000000000LL
                       + (now.tv_nsec - rs->rt_next.tv_nsec);
        if (lag_ns > 20000000LL) {
            rs->rt_next = now;  /* too late: resync on the current instant */
        } else {
            /* Sleep until rs->rt_next. clock_nanosleep(TIMER_ABSTIME) exists
             * on Linux/BSD but NOT on macOS → fall back to a relative nanosleep
             * of the remaining time (lag_ns < 0 = we are ahead). */
#if defined(__APPLE__)
            if (lag_ns < 0) {
                int64_t rem_ns = -lag_ns;
                struct timespec rem = {
                    .tv_sec  = (time_t)(rem_ns / 1000000000LL),
                    .tv_nsec = (long)(rem_ns % 1000000000LL)
                };
                nanosleep(&rem, NULL);
            }
#else
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &rs->rt_next, NULL);
#endif
        }
    }
#endif
}

/* End-of-run outputs: captures, -when safety net, final state, --bench report. */
static void run_end_of_run(emulator_t* emu, const run_state_t* rs) {
    /* End-of-run screenshot */
    if (emu->screenshot_file) {
        log_info("Taking exit screenshot -> %s", emu->screenshot_file);
        emu_refresh_for_capture(emu);
        emu_export_image(emu, emu->screenshot_file);
    }

    /* End-of-run text screenshot: actual text content of the screen ($BB80). */
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

    /* End-of-run ANSI screenshot: true-color image of the framebuffer. */
    if (emu->screenshot_ansi_file) {
        emu_refresh_for_capture(emu);
        if (video_export_ascii_file(&emu->video, emu->screenshot_ansi_file, 2, 2))
            log_info("Exit ANSI screenshot -> %s", emu->screenshot_ansi_file);
        else
            log_error("Cannot write ANSI screenshot file: %s", emu->screenshot_ansi_file);
    }

    /* Safety net: an armed -when that never fired = outright failure.
     * main() will return 2 so that the CI sees it rather than assuming a
     * silently missing capture. */
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
             (unsigned long long)rs->total_executed, (unsigned long long)rs->frame_count);

    char state[128];
    cpu_get_state_string(&emu->cpu, state, sizeof(state));
    log_info("Final CPU state: %s", state);

    /* Sprint 36a — single-line bench report on stdout. Easy to grep from
     * scripts. ORIC clock is 1 MHz, so `mhz_eq` of 1.0 = real-time,
     * 50.0 = 50x real-time. */
    if (emu->bench_mode) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double wall_s = (double)(t1.tv_sec - rs->bench_t0.tv_sec)
                      + (double)(t1.tv_nsec - rs->bench_t0.tv_nsec) * 1e-9;
        if (wall_s <= 0.0) wall_s = 1e-9;
        double mhz_eq = (double)rs->total_executed / (wall_s * 1e6);
        double speed_ratio = mhz_eq / 1.0;   /* ORIC is 1 MHz */
        double frame_us = wall_s * 1e6 / (rs->frame_count > 0 ? (double)rs->frame_count : 1.0);
        printf("BENCH cycles=%llu frames=%llu wall_ms=%.3f mhz_eq=%.2f "
               "speed_ratio=%.1fx frame_us=%.1f\n",
               (unsigned long long)rs->total_executed,
               (unsigned long long)rs->frame_count,
               wall_s * 1000.0, mhz_eq, speed_ratio, frame_us);
        fflush(stdout);
    }
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

    run_state_t rs = {0};
    /* Sprint 36a — start wall clock for --bench. CLOCK_MONOTONIC is what
     * we want : insensitive to NTP / RTC adjustments. */
    if (emu->bench_mode) {
        clock_gettime(CLOCK_MONOTONIC, &rs.bench_t0);
    }
#ifdef HAS_SDL2
    rs.frame_start_ticks = SDL_GetTicks();
#endif
#ifndef __EMSCRIPTEN__
    /* Real-time pacing deadline (--realtime, headless/no-SDL). Absolute
     * CLOCK_MONOTONIC target advanced by one PAL frame each iteration so the
     * pacing never drifts. */
    if (emu->realtime) clock_gettime(CLOCK_MONOTONIC, &rs.rt_next);
#endif

    while (emu->running && app_should_run()) {
#ifdef HAS_SDL2
        rs.frame_start_ticks = SDL_GetTicks();
#endif
        /* F1 menu open: the machine is frozen; we only present the menu,
         * handle keys and keep the 50 Hz pace. */
        if (emu->iomenu.open) {
            run_present_and_events(emu, rs.total_executed);
            run_frame_pacing(emu, &rs);
            continue;
        }
        /* Movie record/replay: the keyboard matrix is the only deterministic
         * input. Apply this frame's state BEFORE the CPU runs so the VIA scan
         * sees it. Replay overwrites live input; record samples it. */
        if (emu->movie.mode == MOVIE_REPLAY) {
            movie_replay_frame(&emu->movie, rs.frame_count, emu->keyboard.matrix);
        } else if (emu->movie.mode == MOVIE_RECORD) {
            movie_record_frame(&emu->movie, rs.frame_count, emu->keyboard.matrix);
        }

        /* One machine frame, then the end-of-frame hooks — in the
         * historical order, which is observable (captures, keystrokes, pacing). */
        run_frame_instructions(emu, &rs);
        run_loci_frame_hooks(emu, rs.total_executed);
        run_headless_audio_sinks(emu);

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

        run_fastload_hooks(emu, rs.total_executed);
        run_autotype_arm(emu, rs.total_executed);
        run_autotype_step(emu, rs.total_executed);

        /* Flush serial trace once per frame (not per byte) */
        if (emu->card_on[CARD_IDX_acia]) {
            acia_trace_flush(&emu->acia);
        }
        /* Video frame already rendered scanline-by-scanline (per-cycle ULA). */
        if (emu->has_cast_server) {
            cast_server_push_frame(&emu->cast_server, emu->video.framebuffer,
                                   (unsigned int)emu->video.native_w,
                                   (unsigned int)emu->video.native_h);
        }
        /* Drain any HTTP-API commands at this frame boundary (sprint 94); the
         * server thread parked in control_queue_submit() is unblocked here, so
         * state-mutating commands run on the emulator thread. No-op when the
         * API is disabled (control_queue is NULL). */
        control_queue_drain(emu->control_queue, emu);
        /* Inject any keystrokes queued by the `keys` command (sprint 95). */
        feed_kbd_inject(emu);

        run_present_and_events(emu, rs.total_executed);
        run_timed_captures(emu, rs.total_executed);

        /* Frame dump */
        if (emu->frame_dump_dir &&
            (rs.frame_count % (uint64_t)emu->frame_dump_interval == 0)) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%06llu.ppm",
                     emu->frame_dump_dir, (unsigned long long)rs.frame_count);
            emu_export_image(emu, path);
        }
        run_video_recording(emu);

        rs.frame_count++;

        run_when_captures(emu, rs.total_executed);
        run_pokes(emu, rs.total_executed);
        run_frame_pacing(emu, &rs);

        /* Headless replay: once the movie is fully drained, exit so a recorded
         * session replays to completion unattended (CI regression). The GUI
         * keeps running so playback can be watched. */
        if (emu->headless && movie_replay_done(&emu->movie) &&
            rs.frame_count > emu->movie.end_frame) {
            log_info("Movie replay complete (%u frames)",
                     (unsigned)emu->movie.end_frame);
            break;
        }
        /* Check cycle limit for headless/test mode */
        if (emu->max_cycles >= 0 && (int64_t)rs.total_executed >= emu->max_cycles) {
            log_info("Cycle limit reached (%lld cycles)", (long long)emu->max_cycles);
            if (emu->control_mode) control_emit_halt(emu, "cycle_limit");
            break;
        }
        if (emu->cpu.halted) {
            log_info("CPU halted after %llu cycles", (unsigned long long)rs.total_executed);
            if (emu->control_mode) control_emit_halt(emu, "jam");
            break;
        }
    }

    run_end_of_run(emu, &rs);
}



/* ── Steps of main() (sprint C): extracted verbatim, in order ── */

/* Log, signals, SIGPIPE; --cast-discover (lists the devices and exits).
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_process(emulator_t* emu, cli_opts_t* cfg) {
    (void)emu;  /* common signature of the steps */
    log_init(cfg->verbose ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);

    app_install_signal_handlers();
    /* Driven by an agent (--control): stdout/stdin are *pipes*, not a
     * terminal. If the peer closes/stops reading, an event write
     * would raise SIGPIPE and kill the emulator (death by signal, invisible in
     * an interactive terminal). Yet control.c wants to stop CLEANLY via
     * ferror(stdout) — but that check is unreachable if SIGPIPE kills first.
     * It is ignored: the broken pipe is then handled cleanly (clean stop). */
    oscompat_ignore_sigpipe();

    /* Cast discover: standalone mode, list devices and exit */
    if (cfg->cast_discover) {
#ifdef HAS_CAST
        cast_server_discover_devices(3000);
#else
        fprintf(stderr, "Cast support not compiled in. Build with CAST=1.\n");
#endif
        return 0;
    }
    return -1;
}

/* Peripherals menu (F1): phosphoric.cfg supplements the command line (which
 * keeps priority), then the menu and its display information.
 * Returns -1 to continue, otherwise the program's exit code. */
/* Configuration file to read, or NULL. PHOSPHORIC_NO_CONFIG (exported by
 * `make tests`): same effect as --no-config, so that a personal
 * phosphoric.cfg influences no test. In headless mode (tests, automation),
 * only an explicitly requested configuration is read. */
static const char* config_to_read(cli_opts_t* cfg) {
    const bool explicit_cfg = cfg->config_path != NULL;
    const char* nocfg = getenv("PHOSPHORIC_NO_CONFIG");
    if (nocfg && *nocfg && strcmp(nocfg, "0") != 0 && !explicit_cfg) cfg->no_config = true;
    if (cfg->no_config || (!explicit_cfg && cfg->headless)) return NULL;
    return explicit_cfg ? cfg->config_path : IOMENU_CONFIG_DEFAULT;
}

static int main_setup_config(emulator_t* emu, cli_opts_t* cfg) {
    const bool explicit_cfg = cfg->config_path != NULL;
    const char* path = config_to_read(cfg);
    if (path) {
        int n = iomenu_config_load(path, cfg);
        if (n >= 0) {
            log_info("Configuration : %s (%d réglage(s) appliqué(s))", path, n);
        } else if (explicit_cfg) {
            log_error("Configuration introuvable : %s", path);
            return 1;
        }
    }
    iom_init(&emu->iomenu);
    emu->config_path     = cfg->config_path;
    emu->jasmin_rom_path = cfg->jasmin_rom_file;
    return -1;
}

/* Initialises the emulator; --ula-ng-poke; run and capture options copied into emu.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_machine(emulator_t* emu, cli_opts_t* cfg) {
    /* Set headless and scale before init so renderer is configured correctly */
    emu->headless = cfg->headless;
    emu->scale_factor = cfg->scale_factor;
    emu->render_software = cfg->render_software;

    if (!emulator_init(emu)) {
        log_error("Failed to initialize emulator");
        return 1;
    }

    /* Cards as modules of the « machine » stage (ULA-NG: --ula-ng-poke). */
    if (card_modules_setup(emu, cfg, CARD_STAGE_MACHINE) != 0) {
        emulator_cleanup(emu);
        return 1;
    }


    emu->fast_load = cfg->fast_load;

    emu->max_cycles = cfg->max_cycles;
    emu->screenshot_file = cfg->screenshot_file;
    emu->screenshot_text_file = cfg->screenshot_text_file;
    emu->screenshot_ansi_file = cfg->screenshot_ansi_file;
    return -1;
}

/* Keyboard, joystick, printer / plotter.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_input_printer(emulator_t* emu, cli_opts_t* cfg) {
    /* Set keyboard layout */
    if (cfg->keyboard_layout && strcasecmp(cfg->keyboard_layout, "azerty") == 0) {
        oric_keyboard_set_layout(&emu->keyboard, ORIC_KB_AZERTY);
        log_info("Keyboard layout: AZERTY");
    } else {
        log_info("Keyboard layout: QWERTY");
    }
    /* Set joystick mode */
    if (cfg->joystick_mode) {
        if (strcasecmp(cfg->joystick_mode, "keys") == 0 || strcasecmp(cfg->joystick_mode, "keyboard") == 0) {
            oric_joystick_set_mode(&emu->joystick, ORIC_JOY_KEYBOARD);
        } else if (strcasecmp(cfg->joystick_mode, "gamepad") == 0 || strcasecmp(cfg->joystick_mode, "sdl") == 0) {
            oric_joystick_set_mode(&emu->joystick, ORIC_JOY_SDL_GAMEPAD);
#ifdef HAS_SDL2
            if (SDL_NumJoysticks() > 0) {
                oric_joystick_open_sdl(&emu->joystick, 0);
            } else {
                log_info("Joystick: no SDL game controller found, waiting for hot-plug");
            }
#endif
        } else {
            log_error("Unknown joystick mode '%s'. Use: keys, gamepad", cfg->joystick_mode);
        }
    }

    /* Set printer type and open output */
    if (cfg->printer_file) {
        if (cfg->printer_type_arg && strcasecmp(cfg->printer_type_arg, "mcp40") == 0) {
            emu->printer.type = PRINTER_MCP40;
            log_info("Printer type: MCP-40 plotter");
        } else {
            emu->printer.type = PRINTER_TEXT;
            log_info("Printer type: text");
        }
        if (!oric_printer_open(&emu->printer, cfg->printer_file)) {
            log_error("Failed to open printer output: %s", cfg->printer_file);
        }
    }
    return -1;
}

/* ACIA 6551 and serial backends, Digitelec DTL 2000, Mageco / ORICON (MIDI).
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_serial_cards(emulator_t* emu, cli_opts_t* cfg) {
    /* Cards as modules of the « serial » stage (ACIA 6551, DTL 2000,
     * Mageco/ORICON), at the place their code used to occupy here (card_module.h). */
    if (card_modules_setup(emu, cfg, CARD_STAGE_SERIAL) != 0) {
        emulator_cleanup(emu);
        return 1;
    }
    return -1;
}

/* Frame dump, AVI video, IRQ / PSG / keyboard traces, WAV capture.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_recordings(emulator_t* emu, cli_opts_t* cfg) {
    emu->frame_dump_dir = cfg->frame_dump_dir;
    emu->frame_dump_interval = (cfg->frame_dump_interval > 0) ? cfg->frame_dump_interval : 50;

    emu->video_avi_file = cfg->video_avi_file;
    emu->video_avi_fps = (cfg->video_avi_fps > 0) ? cfg->video_avi_fps : 50;
    emu->video_avi_quality = (cfg->video_avi_quality > 0) ? cfg->video_avi_quality : 85;
    emu->video_avi_active = false;
    if (cfg->video_avi_file) {
        /* With --export-border the recorded frames carry the overscan
         * border, so the stream geometry grows by the border on each side. */
        int avi_w = emu->export_border ? ORIC_SCREEN_W + 2 * VIDEO_BORDER_W : ORIC_SCREEN_W;
        int avi_h = emu->export_border ? ORIC_SCREEN_H + 2 * VIDEO_BORDER_H : ORIC_SCREEN_H;
        /* Audio muxing. Headless generates PCM inline (ay_generate) in the main
         * loop. GUI can't : the SDL audio callback owns the PSG generator, so we
         * tap the PCM it produces via a thread-safe ring (audio_avi_tap_*) and
         * drain it per frame. Either path declares a 44.1 kHz stereo stream. */
        bool avi_gui_audio = !emu->headless && audio_avi_tap_enable();
        int avi_arate = (emu->headless || avi_gui_audio) ? AUDIO_SAMPLE_RATE : 0;
        int avi_achan = (emu->headless || avi_gui_audio) ? 2 : 0;
        if (avi_recorder_open_av(&emu->video_avi_rec, cfg->video_avi_file,
                                 avi_w, avi_h,
                                 emu->video_avi_fps, emu->video_avi_quality,
                                 avi_arate, avi_achan)) {
            emu->video_avi_active = true;
            log_info("Video recording (MJPEG AVI%s) -> %s (%d fps, q%d)",
                     (emu->headless || avi_gui_audio) ? " + PCM audio" : "",
                     cfg->video_avi_file, emu->video_avi_fps, emu->video_avi_quality);
        } else {
            if (avi_gui_audio) audio_avi_tap_disable();
            log_error("Cannot open video file for recording: %s", cfg->video_avi_file);
        }
    }

    /* Open --trace-irq FILE */
    if (cfg->trace_irq_file) {
        FILE* fp = cli_open_out(cfg->trace_irq_file, "w", "trace-irq");
        if (!fp) { emulator_cleanup(emu); return 1; }
        fprintf(fp, "# Phosphoric IRQ trace — Oric-1/Atmos\n");
        fprintf(fp, "# Format: <cycle> <event> <details>\n");
        fprintf(fp, "# IRQ-ENTRY: PC before, target (= vector at $FFFE/F), IFR/IER snapshot, srcmask\n");
        fprintf(fp, "# RTI: PC after RTI, P flags, SP\n");
        emu->irq_trace_fp = fp;
        emu->irq_trace_active = true;
        emu->cpu.irq_trace_fp = fp;
        log_info("IRQ trace → %s", cfg->trace_irq_file);
    }

    /* Open --psg-trace FILE (log of AY sound-register writes) */
    if (cfg->psg_trace_file) {
        FILE* fp = cli_open_out(cfg->psg_trace_file, "w", "psg-trace");
        if (!fp) { emulator_cleanup(emu); return 1; }
        fprintf(fp, "# Phosphoric PSG trace — AY-3-8910 sound-register writes\n");
        fprintf(fp, "# Format: <cpu_cycle> R<reg>=<hex>  (reg 0-13 ; ports 14/15 = keyboard, excluded)\n");
        fprintf(fp, "# NB: reg 7 (mixer) is hammered to 7F by the keyboard scan — kept as measured.\n");
        emu->psg_trace_fp = fp;
        log_info("PSG trace → %s", cfg->psg_trace_file);
    }

    /* Open --kbd-scan-trace FILE (log of every VIA Port B keyboard read) */
    if (cfg->kbd_trace_file) {
        FILE* fp = cli_open_out(cfg->kbd_trace_file, "w", "kbd-scan-trace");
        if (!fp) { emulator_cleanup(emu); return 1; }
        fprintf(fp, "# Phosphoric keyboard-scan trace — one line per VIA Port B ($0300) read\n");
        fprintf(fp, "# Format: <cpu_cycle> col=<0-7> reg7=<hex> reg14=<hex> matrix=<hex> PB3=<0|1>\n");
        fprintf(fp, "# reg7 bit6 must be 1 (Port A input) and matrix=FF means no key in that column.\n");
        emu->kbd_trace_fp = fp;
        log_info("Keyboard-scan trace → %s", cfg->kbd_trace_file);
    }

    /* Open --audio-wav FILE (PCM capture ; headless only to avoid racing the
     * SDL audio thread, which is the other consumer of the PSG generator). */
    if (cfg->audio_wav_file) {
        if (!emu->headless) {
            log_error("--audio-wav requires --headless (SDL audio owns the PSG generator)");
            emulator_cleanup(emu);
            return 1;
        }
        FILE* fp = cli_open_out(cfg->audio_wav_file, "wb", "audio-wav");
        if (!fp) { emulator_cleanup(emu); return 1; }
        wav_write_header(fp, 0);   /* placeholder — sizes patched at cleanup */
        emu->audio_wav_fp = fp;
        emu->audio_wav_data_bytes = 0;
        log_info("Audio WAV → %s (16-bit stereo %d Hz)", cfg->audio_wav_file, AUDIO_SAMPLE_RATE);
    }
    return -1;
}

/* LOCI cartridge: firmware co-simulation, real hardware, HLE model.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_loci(emulator_t* emu, cli_opts_t* cfg) {
    /* LOCI card as a module (src/cards/card_loci.c): its code used to be here. An
     * error stops without emulator_cleanup, as before. */
    if (card_modules_setup(emu, cfg, CARD_STAGE_LOCI) != 0) return 1;
    return -1;
}

/* Symbol table, --control mode, TUI.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_debug_frontends(emulator_t* emu, cli_opts_t* cfg) {
    /* Load symbol table (--symbols FILE) */
    symbol_table_init(&emu->symbols);

    /* Sprint 35a — IPC control mode for OricForge. Logs go to stderr so
     * stdout stays a clean protocol channel. Forces headless so SDL output
     * never collides with stdout traffic. */
    emu->control_mode = cfg->control_mode;
    emu->bench_mode = cfg->bench_mode;
    if (cfg->control_mode) {
        log_set_stream(stderr);
        emu->headless = true;
        emu->debugger.active = true;   /* wait for first client command */
    }

    /* Route debugger break into ncurses TUI when --tui is set
     * (requires build with TUI=1). Init done lazily on first break. */
    emu->tui_mode = cfg->tui_mode;
    if (cfg->tui_mode) {
#ifdef HAS_TUI
        if (!tui_init()) {
            log_error("Failed to initialise ncurses TUI");
            emu->tui_mode = false;
        }
#else
        log_error("--tui requires a build with TUI=1 (ncurses)");
        emu->tui_mode = false;
#endif
    }
    if (cfg->symbols_file) {
        if (symbol_table_load(&emu->symbols, cfg->symbols_file) < 0) {
            emulator_cleanup(emu);
            return 1;
        }
    }
    return -1;
}

/* -at / -when captures, pokes, --type-keys(-when), media paths.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_captures_input(emulator_t* emu, cli_opts_t* cfg) {
    /* Resolution of the REPEATABLE -at captures (--screenshot-at / -text-at /
     * -ansi-at / --dump-ram-at) collected in tcap_cli: each "CYCLES:FILE"
     * becomes a timed_captures[] entry. Malformed format = fatal (as before). */
    {
        static const char* const tcap_optname[] = {
            "screenshot-at", "screenshot-text-at", "screenshot-ansi-at", "dump-ram-at"
        };
        for (int i = 0; i < cfg->tcap_cli_count; i++) {
            int64_t cyc; const char* file;
            if (!cli_split_cycles_file(cfg->tcap_cli[i].arg, tcap_optname[cfg->tcap_cli[i].type],
                                       &cyc, &file)) {
                emulator_cleanup(emu);
                return 1;
            }
            if (emu->timed_capture_count < TIMED_CAPTURE_MAX) {
                timed_capture_t* t = &emu->timed_captures[emu->timed_capture_count++];
                t->cycles = cyc; t->file = file; t->type = cfg->tcap_cli[i].type; t->done = false;
            }
            log_info("Capture %s armée à %lld cycles -> %s",
                     tcap_optname[cfg->tcap_cli[i].type], (long long)cyc, file);
        }
    }

    /* Parse the state-triggered captures ADDR:VAL:FILE */
    if (cfg->screenshot_when_arg) {
        if (!cli_split_addr_val_file(cfg->screenshot_when_arg, "screenshot-when",
                                     &emu->screenshot_when_addr, &emu->screenshot_when_val,
                                     &emu->screenshot_when_file)) {
            emulator_cleanup(emu);
            return 1;
        }
    }
    if (cfg->screenshot_text_when_arg) {
        if (!cli_split_addr_val_file(cfg->screenshot_text_when_arg, "screenshot-text-when",
                                     &emu->screenshot_text_when_addr, &emu->screenshot_text_when_val,
                                     &emu->screenshot_text_when_file)) {
            emulator_cleanup(emu);
            return 1;
        }
    }
    if (cfg->dump_ram_when_arg) {
        if (!cli_split_addr_val_file(cfg->dump_ram_when_arg, "dump-ram-when",
                                     &emu->dump_ram_when_addr, &emu->dump_ram_when_val,
                                     &emu->dump_ram_when_file)) {
            emulator_cleanup(emu);
            return 1;
        }
    }
    /* --poke-at / --poke-when: parsed now that emu (and emu.poke_count=0)
     * is initialised. Each entry is appended to emu.pokes[] in
     * command-line order, preserving the intended grouping (e.g. cx, cy, click). */
    for (int i = 0; i < cfg->poke_arg_count; i++) {
        bool ok = cfg->poke_args[i].is_when
                    ? cli_add_poke_when(emu, cfg->poke_args[i].arg)
                    : cli_add_poke_at(emu, cfg->poke_args[i].arg);
        if (!ok) {
            emulator_cleanup(emu);
            return 1;
        }
    }
    /* --type-keys-when ADDR:VAL:TEXT : arm the auto-typer when RAM[ADDR]==VAL
     * instead of guessing a boot cycle. TEXT keeps the same escapes as
     * --type-keys and may start with "loci-hid:" to route via the LOCI HID. */
    if (cfg->type_keys_when_arg) {
        const char* text = NULL;
        if (!cli_split_addr_val_file(cfg->type_keys_when_arg, "type-keys-when",
                                     &emu->type_keys_when_addr, &emu->type_keys_when_val,
                                     &text)) {
            emulator_cleanup(emu);
            return 1;
        }
        if (strncmp(text, "loci-hid:", 9) == 0) {
            emu->type_keys_when_loci_hid = true;
            text += 9;
        }
        emu->type_keys_when_text = text;
        log_info("Auto-type armed when RAM[$%04X]==$%02X (%s): \"%s\"",
                 (unsigned)emu->type_keys_when_addr, emu->type_keys_when_val,
                 emu->type_keys_when_loci_hid ? "LOCI HID" : "ORIC matrix", text);
    }

    /* Parse --type-keys CYCLES:TEXT (Sprint 34av: TEXT may start with
     * "loci-hid:" to route keys via the LOCI HID bitmap instead of the
     * ORIC keyboard matrix — useful for automating the LOCI TUI).
     *
     * Several --type-keys may be given: they are stacked in a
     * queue (sorted by arming cycle) and activated one after the other once
     * the previous one is finished. This replaces the old « only one --type-keys
     * kept » and allows cleanly sequencing a multi-screen walkthrough with
     * repeated keys (1 at cycle X, 1 at cycle Y, …). */
    for (int i = 0; i < cfg->type_keys_arg_count; i++) {
        const char* arg = cfg->type_keys_args[i];
        const char* colon = strchr(arg, ':');
        if (!colon) {
            log_error("Invalid --type-keys format. Use CYCLES:TEXT (e.g. 3000000:CLOAD\"\"\\n)");
            emulator_cleanup(emu);
            return 1;
        }
        const char* text = colon + 1;
        bool loci_hid = false;
        if (strncmp(text, "loci-hid:", 9) == 0) {
            loci_hid = true;
            text += 9;
        }
        emu->type_keys_seq[emu->type_keys_seq_count].at = atoll(arg);
        emu->type_keys_seq[emu->type_keys_seq_count].text = text;
        emu->type_keys_seq[emu->type_keys_seq_count].loci_hid = loci_hid;
        emu->type_keys_seq_count++;
    }
    if (emu->type_keys_seq_count > 0) {
        /* Stable sort by increasing arming cycle (insertion: N <= 16). */
        for (int i = 1; i < emu->type_keys_seq_count; i++) {
            for (int j = i; j > 0 &&
                 emu->type_keys_seq[j].at < emu->type_keys_seq[j-1].at; j--) {
                int64_t tat = emu->type_keys_seq[j].at;
                const char* ttext = emu->type_keys_seq[j].text;
                bool thid = emu->type_keys_seq[j].loci_hid;
                emu->type_keys_seq[j] = emu->type_keys_seq[j-1];
                emu->type_keys_seq[j-1].at = tat;
                emu->type_keys_seq[j-1].text = ttext;
                emu->type_keys_seq[j-1].loci_hid = thid;
            }
        }
        /* Activates the first entry; the following ones will be activated in the
         * emulation loop by activate-next when their cycle is reached. */
        emu->type_keys_at = emu->type_keys_seq[0].at;
        emu->type_keys_text = emu->type_keys_seq[0].text;
        emu->type_keys_loci_hid = emu->type_keys_seq[0].loci_hid;
        emu->type_keys_idx = 0;
        emu->type_keys_next_cycle = emu->type_keys_at;
        emu->type_keys_done = false;
        emu->type_keys_seq_idx = 1;
        for (int i = 0; i < emu->type_keys_seq_count; i++) {
            log_info("Auto-type[%d] at %lld cycles (%s): \"%s\"", i,
                     (long long)emu->type_keys_seq[i].at,
                     emu->type_keys_seq[i].loci_hid ? "LOCI HID" : "ORIC matrix",
                     emu->type_keys_seq[i].text);
        }
    }

    /* Create frame dump directory if specified */
    if (cfg->frame_dump_dir) {
        oscompat_mkdir(cfg->frame_dump_dir, 0755);
    }

    /* Store file paths for save state metadata */
    emu->rom_path = cfg->rom_file;
    emu->disk_path = cfg->disk_files[0];
    emu->diskrom_path = cfg->disk_rom_file;
    emu->tape_path = cfg->tape_file;

    /* Per-drive tracking for write-back / ejection from the OSD. */
    emu->disk_writeback = cfg->disk_writeback;
    for (int i = 0; i < MICRODISC_MAX_DRIVES; i++)
        emu->disk_paths[i] = cfg->disk_files[i];
    return -1;
}

/* System ROM, ROM guard, --rom-info, machine model, hostfs.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_rom_model(emulator_t* emu, cli_opts_t* cfg) {
    /* Minimal profile: without -r or --load-state, bare ORIC-1 on its BASIC 1.0
     * ROM (BASIC 1.1 under -m atmos), looked up in the current directory then
     * next to the executable. --no-rom keeps the $C000-$FFFF area empty. */
    static char default_rom[1024 + sizeof(DEFAULT_SYSTEM_ROM_ATMOS) + 1];
    if (!cfg->rom_file && !cfg->load_state_file && !cfg->no_rom) {
        const bool atmos = cfg->model_arg && (strcasecmp(cfg->model_arg, "atmos") == 0 ||
                                              strcmp(cfg->model_arg, "1.1") == 0);
        const char* rom = atmos ? DEFAULT_SYSTEM_ROM_ATMOS : DEFAULT_SYSTEM_ROM;
        if (access(rom, R_OK) == 0) {
            cfg->rom_file = rom;
        } else {
            char self[1024];
            ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
            char* slash = n > 0 ? (self[n] = '\0', strrchr(self, '/')) : NULL;
            if (slash) {
                *slash = '\0';
                snprintf(default_rom, sizeof(default_rom), "%s/%s", self, rom);
                if (access(default_rom, R_OK) == 0) cfg->rom_file = default_rom;
            }
        }
        if (cfg->rom_file) log_info("Profil minimal : %s, ROM par défaut %s",
                                    atmos ? "Atmos" : "ORIC-1", cfg->rom_file);
    }

    /* Load ROM if specified */
    if (cfg->rom_file) {
        log_info("Loading ROM: %s", cfg->rom_file);
        if (!memory_load_rom(&emu->memory, cfg->rom_file, 0)) {
            log_error("Failed to load ROM: %s", cfg->rom_file);
            emulator_cleanup(emu);
            return 1;
        }
        /* Direct LOCI menu ROM boot (-r roms/loci/locirom --loci): patch
         * the firmware version/timing placeholders like the real MIA. */
        if (emu->card_on[CARD_IDX_loci])
            loci_patch_rom_info(emu);
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
    if (!cfg->rom_file && !cfg->load_state_file) {
        if (cfg->disk_rom_file) {
            log_error("--disk-rom requires the base system ROM (-r ROM): the "
                      "BASIC ROM area $C000-$FFFF would be empty and the machine "
                      "cannot boot (code mapping the ROM reads $00 = BRK). "
                      "Add e.g. -r roms/basic11b.rom");
            emulator_cleanup(emu);
            return 1;
        }
        log_warning("No system ROM loaded (-r ROM): $C000-$FFFF is empty, the "
                    "machine will not boot. Specify e.g. -r roms/basic11b.rom");
    }

    /* ROM analysis (if requested) */
    if (cfg->rom_info_enabled && cfg->rom_file) {
        rom_analysis_t rom_analysis;
        rominfo_analyze(&rom_analysis, emu->memory.rom, ROM_SIZE);
        if (cfg->rom_info_file) {
            rominfo_report_to_file(&rom_analysis, emu->memory.rom, ROM_SIZE, cfg->rom_info_file);
        } else {
            rominfo_report(&rom_analysis, emu->memory.rom, ROM_SIZE, stdout);
        }
    } else if (cfg->rom_info_enabled && !cfg->rom_file) {
        log_error("--rom-info requires a ROM file (-r ROM)");
    }

    /* Detect or set machine model */
    if (cfg->model_arg) {
        if (strcasecmp(cfg->model_arg, "atmos") == 0 || strcmp(cfg->model_arg, "1.1") == 0) {
            emu->model = ORIC_MODEL_ATMOS;
        } else if (strcasecmp(cfg->model_arg, "oric1") == 0 || strcmp(cfg->model_arg, "1.0") == 0) {
            emu->model = ORIC_MODEL_ORIC1;
        } else {
            log_error("Unknown model '%s'. Use: oric1, atmos, 1.0, or 1.1", cfg->model_arg);
            emulator_cleanup(emu);
            return 1;
        }
        log_info("Machine model: %s (user-specified)",
                 emu->model == ORIC_MODEL_ATMOS ? "ORIC Atmos" : "ORIC-1");
    } else if (cfg->rom_file) {
        emu->model = detect_rom_version(&emu->memory);
        log_info("Machine model: %s (auto-detected from ROM)",
                 emu->model == ORIC_MODEL_ATMOS ? "ORIC Atmos" : "ORIC-1");
    } else {
        emu->model = ORIC_MODEL_ORIC1;
    }
    emu->rom_patches = get_rom_patches(emu->model);
    log_info("ROM patches: %s", emu->rom_patches->name);

    /* Mount host filesystem */
    if (cfg->hostfs_path) {
        log_info("Mounting host filesystem: %s", cfg->hostfs_path);
        if (!hostfs_mount(&emu->hostfs, cfg->hostfs_path, false)) {
            log_error("Failed to mount host filesystem: %s", cfg->hostfs_path);
            emulator_cleanup(emu);
            return 1;
        }
    }
    return -1;
}

/* Tape: .TAP loading and playback modes.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_tape(emulator_t* emu, cli_opts_t* cfg) {
    /* Load tape */
    if (cfg->tape_file) {
        log_info("Loading tape: %s", cfg->tape_file);
        if (cfg->tape_signal && cfg->fast_load) {
            log_warning("--tape-signal is incompatible with -f/--fast-load; "
                        "using signal-level load");
            cfg->fast_load = false;
            emu->fast_load = false;
        }
        if (cfg->fast_load) {
            /* Fast load: buffer TAP data for deferred injection after RAM test */
            tap_file_t* tap = tap_open_read(cfg->tape_file, true);
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
                            emu->fastload_buf = buf;
                            emu->fastload_addr = header.start_addr;
                            emu->fastload_end = header.end_addr;
                            emu->fastload_size = (uint16_t)rd;
                            emu->fastload_type = header.type;
                            emu->fastload_auto_run = header.auto_run;
                            emu->fastload_pending = true;
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
                    emu->tapelen = (int)tap_size(tap);
                    emu->tapebuf = (uint8_t*)malloc((size_t)emu->tapelen);
                    if (emu->tapebuf) {
                        memcpy(emu->tapebuf, tap->data, (size_t)emu->tapelen);
                        emu->tapeoffs = (int)remaining_pos;
                        emu->tape_loaded = true;
                        emu->tape_syncstack = -1;
                        log_info("Tape buffered for CLOAD: %d bytes, offset=%d",
                                 emu->tapelen, emu->tapeoffs);
                    }
                }

                tap_close(tap);
            } else {
                log_warning("Failed to open tape: %s", cfg->tape_file);
            }
        } else {
            /* Normal load: buffer TAP for CLOAD via ROM patching */
            FILE* f = fopen(cfg->tape_file, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                emu->tapelen = ftell(f);
                fseek(f, 0, SEEK_SET);
                emu->tapebuf = (uint8_t*)malloc(emu->tapelen);
                if (emu->tapebuf) {
                    size_t rd = fread(emu->tapebuf, 1, emu->tapelen, f);
                    if ((int)rd == emu->tapelen) {
                        emu->tapeoffs = 0;
                        emu->tape_loaded = true;
                        emu->tape_syncstack = -1;
                        emu->tape_auto_cload_pending = true;
                        log_info("Tape buffered for CLOAD: %d bytes", emu->tapelen);
                        if (cfg->tape_signal) {
                            cassette_signal_begin(&emu->cassette, emu->tapebuf,
                                                  emu->tapelen);
                            emu->cassette.free_gate = cfg->tape_signal_free;
                            log_info("Signal-level cassette enabled: %d bytes on "
                                     "CB1 waveform (real ROM read)%s", emu->tapelen,
                                     cfg->tape_signal_free ? " [free-gate ORB PB6]" : "");
                        }
                    } else {
                        log_warning("Tape read incomplete: %zu/%d bytes", rd, emu->tapelen);
                        free(emu->tapebuf);
                        emu->tapebuf = NULL;
                    }
                }
                fclose(f);
            } else {
                log_warning("Failed to open tape: %s", cfg->tape_file);
            }
        }
    }
    return -1;
}

/* Jasmin, SP0256 / MEA8000 speech synthesis, Microdisc and disks, LOCI web disk, bad sectors.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_disks_speech(emulator_t* emu, cli_opts_t* cfg) {
    /* Cards as modules of the « disks, early » stage (Jasmin), at the place
     * their code used to occupy here (card_module.h). */
    if (card_modules_setup(emu, cfg, CARD_STAGE_DISKS_EARLY) != 0) {
        emulator_cleanup(emu);
        return 1;
    }

    /* Cards as modules of the « speech » stage (SP0256, MEA8000), at the
     * place their code used to occupy here (card_module.h). */
    if (card_modules_setup(emu, cfg, CARD_STAGE_SPEECH) != 0) {
        emulator_cleanup(emu);
        return 1;
    }

    /* Cards as modules of the « disks » stage (Microdisc), at the place
     * their code used to occupy here (card_module.h). */
    if (card_modules_setup(emu, cfg, CARD_STAGE_DISKS) != 0) {
        emulator_cleanup(emu);
        return 1;
    }

    /* --loci-web: NATIVE LOCI autoboot. The web disk is already mounted on the
     * LOCI's FDC (loci_dsk_open_web); all that remains is to install the Microdisc
     * ROM overlay at $A000 — exactly what MIA_BOOT does with LOCI_BOOT_FDC —
     * so that the boot code reads the disk via $0310 (routed to the LOCI FDC,
     * web-backed under --loci) and boots Sedoric without going through the menu. The
     * -r ROM (BASIC) is already loaded at $C000; so only $A000 is touched. */
    if (cfg->loci_web_url && emu->card_on[CARD_IDX_loci]) {
        char disc[512] = {0};
        const char* cand = cfg->disk_rom_file;                 /* --disk-rom if provided */
        if ((!cand || access(cand, R_OK) != 0) && cfg->rom_file) {
            const char* slash = strrchr(cfg->rom_file, '/');   /* next to the -r ROM */
            if (slash) {
                snprintf(disc, sizeof(disc), "%.*s/microdis.rom",
                         (int)(slash - cfg->rom_file), cfg->rom_file);
                if (access(disc, R_OK) == 0) cand = disc;
            }
        }
        if (!cand || access(cand, R_OK) != 0) { if (access("roms/microdis.rom", R_OK) == 0) cand = "roms/microdis.rom"; }
        if (!cand || access(cand, R_OK) != 0) { if (access("microdis.rom", R_OK) == 0) cand = "microdis.rom"; }
        if (cand && access(cand, R_OK) == 0 && loci_rom_swap_cb(emu, cand, 0xA000)) {
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
    for (int i = 0; i < cfg->bad_sector_arg_count; i++) {
        unsigned d = 0, s, trk, sec;
        int nf = sscanf(cfg->bad_sector_args[i], "%u:%u:%u:%u", &d, &s, &trk, &sec);
        if (nf == 3) { sec = trk; trk = s; s = d; d = 0; }   /* S:T:N → drive A */
        if ((nf == 3 || nf == 4) &&
            d < MICRODISC_MAX_DRIVES && s <= 1 && trk < 256 && sec >= 1 && sec < 256) {
            int rc = -1;
            if (emu->card_on[CARD_IDX_microdisc])
                rc = microdisc_add_bad_sector(&emu->microdisc, (uint8_t)d,
                                              (uint8_t)s, (uint8_t)trk, (uint8_t)sec);
            if (emu->card_on[CARD_IDX_jasmin]) {
                int rc2 = jasmin_add_bad_sector(&emu->jasmin, (uint8_t)d,
                                                (uint8_t)s, (uint8_t)trk, (uint8_t)sec);
                if (rc != 0) rc = rc2;
            }
            if (emu->card_on[CARD_IDX_loci]) {
                int rc2 = loci_add_bad_sector(&emu->loci, (uint8_t)d,
                                              (uint8_t)s, (uint8_t)trk, (uint8_t)sec);
                if (rc != 0) rc = rc2;
            }
            if (rc == 0) {
                log_info("Bad sector injected: drive %c side %u track %u sector %u",
                         'A' + d, s, trk, sec);
            } else {
                log_error("--bad-sector %s: no disk subsystem (use -d/--disk-rom, --jasmin-rom or --loci)",
                          cfg->bad_sector_args[i]);
                emulator_cleanup(emu);
                return 1;
            }
        } else {
            log_error("Invalid --bad-sector format '%s'. Use [D:]S:T:N "
                      "(drive 0-3, side 0-1, track, sector 1-255)",
                      cfg->bad_sector_args[i]);
            emulator_cleanup(emu);
            return 1;
        }
    }
    return -1;
}

/* Debugger, cast server, HTTP API, CASTV2 client, resuming a saved state.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_services(emulator_t* emu, cli_opts_t* cfg) {
    /* Tabs set by phosphoric.cfg (protection_x=oui), once the floppies
     * are in place. */
    for (int d = 0; d < 4; d++)
        if (cfg->disk_protect[d] && emu_has_disk_iface(emu) && d < emu_disk_max_drives(emu))
            emu_disk_set_protected(emu, d, true);

    /* Setup debugger if requested */
    if (cfg->debug_mode) {
        emu->debugger.active = true;
        log_info("Debugger mode enabled (will break at first instruction)");
    }
    if (cfg->debug_break_addr) {
        uint16_t addr = parse_hex16(cfg->debug_break_addr);
        debugger_add_breakpoint(&emu->debugger, addr);
        log_info("Debugger breakpoint set at $%04X", addr);
    }

    /* --cast-to implicitly enables --cast-server */
    if (cfg->cast_to_enabled && !cfg->cast_server_enabled) {
        cfg->cast_server_enabled = true;
    }

    /* Initialize cast server if requested */
    if (cfg->cast_server_enabled) {
#ifdef HAS_CAST
        if (cast_server_init(&emu->cast_server, cfg->cast_server_port)) {
            emu->has_cast_server = true;
            /* Connect audio output to cast server for WAV streaming */
            audio_set_cast_server(&emu->cast_server);
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
    if (cfg->http_api_enabled) {
#ifdef HAS_HTTPAPI
        emu->control_queue = control_queue_create();
        emu->http_api = emu->control_queue
            ? http_api_start(emu, emu->control_queue, cfg->http_api_port,
                             cfg->http_api_bind, cfg->http_api_root)
            : NULL;
        if (emu->http_api) {
            emu->has_http_api = true;
        } else {
            log_error("Failed to start HTTP API server");
            if (emu->control_queue) { control_queue_destroy(emu->control_queue); emu->control_queue = NULL; }
        }
#else
        fprintf(stderr, "HTTP API not compiled in. Build with HTTPAPI=1.\n");
#endif
    }

    /* Initialize CASTV2 client: discover device and cast */
    if (cfg->cast_to_enabled && emu->has_cast_server) {
#ifdef HAS_CAST
        char device_ip[64] = "";
        bool discovered = false;

        if (cfg->cast_to_device && cfg->cast_to_device[0]) {
            /* Try to parse as IP address first */
            struct in_addr test_addr;
            if (inet_pton(AF_INET, cfg->cast_to_device, &test_addr) == 1) {
                strncpy(device_ip, cfg->cast_to_device, sizeof(device_ip) - 1);
                discovered = true;
            }
        }

        if (!discovered) {
            discovered = castv2_discover_device(device_ip, cfg->cast_to_device, 5000);
        }

        if (discovered) {
            /* Build stream URL */
            char local_ip[64] = "";
            if (!castv2_get_local_ip(local_ip)) {
                strncpy(local_ip, "127.0.0.1", sizeof(local_ip));
            }
            char stream_url[256];
            snprintf(stream_url, sizeof(stream_url), "http://%s:%d/",
                     local_ip, emu->cast_server.port);

            log_info("Casting to %s, stream URL: %s", device_ip, stream_url);

            if (castv2_connect_and_cast(&emu->castv2_client, device_ip, stream_url)) {
                emu->has_castv2 = true;
            } else {
                log_error("Failed to connect CASTV2 to %s", device_ip);
            }
        } else {
            log_error("No Chromecast device found%s%s",
                      cfg->cast_to_device ? " matching '" : "",
                      cfg->cast_to_device ? cfg->cast_to_device : "");
            if (cfg->cast_to_device) log_error("'");
        }
#else
        fprintf(stderr, "Cast support not compiled in. Build with CAST=1.\n");
#endif
    }

    /* Load save state if specified */
    if (cfg->load_state_file) {
        log_info("Loading save state: %s", cfg->load_state_file);
        if (!savestate_load(emu, cfg->load_state_file)) {
            log_error("Failed to load save state: %s", cfg->load_state_file);
        } else {
            /* Prevent emulator_run()'s power-on cpu_reset from wiping the
             * restored PC/cycles (bug: --load-state landed back at reset,
             * cycles=0, most visible under --control). */
            emu->startup_state_loaded = true;
        }
    }
    return -1;
}

/* Banner, CPU / bus traces, core and ULA, profiler, TAS movie, GDB stub, tape-out capture.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_setup_tracing(emulator_t* emu, cli_opts_t* cfg, gdb_stub_t* gdb_stub) {
    if (!cfg->headless) {
        printf("\n");
        printf("Phosphoric v%s\n", EMU_VERSION);
        printf("Press Ctrl+C to quit\n\n");
    }

    /* CPU trace logging */
    trace_init(&emu->trace);
    if (cfg->trace_file) {
        /* --symbols annotates the trace with labels, in BOTH modes.
         * (Before: only the conditional IPC/debugger trace used it; the
         * streaming --trace ignored --symbols.) */
        bool trace_syms = (cfg->symbols_file != NULL);
        if (trace_syms) trace_set_symbols(&emu->trace, &emu->symbols);
        if (cfg->trace_ring > 0) {
            /* Ring/tail mode: keeps the N LAST instructions in memory
             * (ideal for a deep hang, where --trace-max only keeps the
             * FIRST ones) and writes them on exit via trace_save_ring(). */
            trace_arm(&emu->trace, TRACE_START_NOW, 0, TRACE_STOP_NONE, 0, 0,
                      (uint32_t)cfg->trace_ring, trace_syms);
            log_info("CPU trace ring armed (last %lld instructions) -> %s",
                     (long long)cfg->trace_ring, cfg->trace_file);
        } else {
            if (cfg->trace_max > 0) trace_set_max(&emu->trace, (uint64_t)cfg->trace_max);
            if (!trace_open(&emu->trace, cfg->trace_file)) {
                log_error("Failed to open trace file: %s", cfg->trace_file);
            } else if (trace_syms) {
                /* Streaming + symbols: (re)arm in "now" mode (ring_cap=0) to
                 * enable symbolic annotation on the already-open fp. */
                trace_arm(&emu->trace, TRACE_START_NOW, 0, TRACE_STOP_NONE, 0, 0,
                          0, true);
            }
        }
    }

    /* Micro-sequenced core (--cpu-microseq, V2-E1): each cycle emits its
     * own bus access, NMOS dummy accesses included. Opt-in during the
     * migration; semantics identical to the historical engine (same computation
     * functions), only the ordering of the cycles changes. */
    cpu_set_microseq(&emu->cpu, cfg->cpu_microseq);
    /* Cycle-level ULA (V2-E4): one cell fetched per cycle, at the instant the
     * real ULA reads it. Opt-in during validation. */
    emu->ula_per_cycle = cfg->ula_per_cycle;
    emu->ula_fetch_offset = cfg->ula_fetch_offset;
    if (!cfg->ula_per_cycle)
        log_info("ULA: rendu par ligne (--ula-line) — pas de split en milieu de ligne");
    else if (!cfg->cpu_microseq)
        log_warning("ULA: --cpu-legacy impose le rendu par ligne "
                    "(une instruction y est indivisible)");
    if (!cfg->cpu_microseq)
        log_info("CPU: cœur historique (--cpu-legacy) — pas d'accès factices");

    /* Cycle-by-cycle bus trace (--cycle-trace) — a V2 instrument.
     * Hooked onto the CPU's bus-access hook; the internal cycles are
     * emitted by cpu_cycle_tick(), which calls cycle_trace_cycles(). */
    if (cfg->cycle_trace_file) {
        if (!cycle_trace_open(cfg->cycle_trace_file, cfg->cycle_trace_max)) {
            log_error("Failed to open cycle trace file: %s", cfg->cycle_trace_file);
        } else {
            cpu_set_bus_callback(&emu->cpu, cycle_trace_bus, NULL);
            log_info("Cycle trace -> %s%s", cfg->cycle_trace_file,
                     cfg->cycle_trace_max ? " (capped)" : "");
        }
    }

    /* CPU performance profiler */
    profiler_init(&emu->profiler);
    if (cfg->profile_file) {
        profiler_start(&emu->profiler);
        log_info("CPU profiling enabled, report will be written to %s", cfg->profile_file);
    }

    /* Deterministic input record/replay (TAS movie). */
    if (cfg->movie_replay_file) {
        uint8_t mv_model = 0;
        if (movie_replay_open(&emu->movie, cfg->movie_replay_file, &mv_model)) {
            if ((oric_model_t)mv_model != emu->model) {
                log_warning("movie recorded for model %u but running model %u — "
                            "replay may diverge", mv_model, (unsigned)emu->model);
            }
        }
    } else if (cfg->movie_record_file) {
        movie_record_open(&emu->movie, cfg->movie_record_file, (uint8_t)emu->model);
    }

    /* GDB remote stub: open the listener and block until a client attaches,
     * then start the CPU halted so GDB drives execution from the reset vector. */
    if (cfg->gdb_enabled) {
        if (gdb_stub_init(gdb_stub, (uint16_t)cfg->gdb_port, cfg->gdb_bind)) {
            emu->gdb_mode = true;
            emu->gdb_stub = gdb_stub;
            emu->debugger.active = true;   /* stop at entry, wait for GDB */
        } else {
            log_error("GDB stub: failed to start on port %d", cfg->gdb_port);
        }
    }

#ifdef __EMSCRIPTEN__
    /* Expose the running machine to the JS virtual keyboard. */
    g_web_emu = emu;
#endif

    /* --tape-out-capture: arms the capture of the tape-OUT waveform (PB7 driven by
     * Timer 1). Independent of -t (CSAVE writes, no input tape required). In
     * capture mode, the CSAVE PC-1.1 hooks are neutralised (cf. tape write). */
    if (cfg->tape_out_capture_arg) {
        emu->tape_out_path = cfg->tape_out_capture_arg;
        tape_capture_begin(&emu->tape_capture);
        log_info("Tape-OUT capture armed (PB7/Timer1) -> %s", cfg->tape_out_capture_arg);
    }
    return -1;
}

/* End of run: captured .TAP, GDB, movie, AVI, state, disk write-back, profiler, traces; exit code.
 * Returns -1 to continue, otherwise the program's exit code. */
static int main_finish(emulator_t* emu, cli_opts_t* cfg, gdb_stub_t* gdb_stub) {
    /* --menu-screenshot: the F1 menu as it would appear right now. */
    if (cfg->menu_screenshot) {
        if (iomenu_screenshot(emu, cfg->menu_screenshot))
            log_info("Menu des périphériques : %s", cfg->menu_screenshot);
        else
            log_error("Impossible d'écrire %s", cfg->menu_screenshot);
    }
    /* Writes the .TAP rebuilt from the captured PB7 waveform. */
    if (cfg->tape_out_capture_arg && emu->tape_capture.active) {
        FILE* tf = fopen(cfg->tape_out_capture_arg, "wb");
        if (tf) {
            if (emu->tape_capture.out_len > 0)
                fwrite(emu->tape_capture.out, 1, (size_t)emu->tape_capture.out_len, tf);
            fclose(tf);
            log_info("Tape-OUT capture written: %d bytes -> %s",
                     emu->tape_capture.out_len, cfg->tape_out_capture_arg);
        } else {
            log_error("Tape-OUT capture: cannot write %s", cfg->tape_out_capture_arg);
        }
        tape_capture_free(&emu->tape_capture);
    }

    if (cfg->gdb_enabled && emu->gdb_stub) {
        gdb_stub_close(gdb_stub);
    }

    /* Flush a recording / free replay buffers. */
    if (emu->movie.mode != MOVIE_OFF) {
        movie_close(&emu->movie);
    }

    /* Finalize video recording (write index, back-patch sizes). */
    if (emu->video_avi_active) {
        uint32_t nframes = emu->video_avi_rec.frame_count;
        /* GUI: flush any PCM still in the tap so the tail of the sound isn't
         * dropped, then release the ring (still safe: audio device is alive). */
        if (!emu->headless && emu->video_avi_rec.has_audio) {
            enum { TAP_DRAIN_MAX = (AUDIO_SAMPLE_RATE / ORIC_FRAME_RATE) * 4 };
            static int16_t tap_pcm[TAP_DRAIN_MAX * 2];
            int got;
            while ((got = audio_avi_tap_drain(tap_pcm, TAP_DRAIN_MAX)) > 0)
                avi_recorder_add_audio(&emu->video_avi_rec, tap_pcm, got);
            audio_avi_tap_disable();
        }
        if (avi_recorder_close(&emu->video_avi_rec)) {
            log_info("Video recording finalized: %s (%u frames)",
                     cfg->video_avi_file, nframes);
        } else {
            log_error("Error finalizing video recording: %s", cfg->video_avi_file);
        }
        emu->video_avi_active = false;
    }

    /* Save state on exit if specified */
    if (cfg->save_state_file) {
        log_info("Saving state on exit: %s", cfg->save_state_file);
        savestate_save(emu, cfg->save_state_file);
    }

    /* Write modified disk images back to their .dsk files (opt-in). A drive is
     * dirty only if the guest actually wrote a sector to it this session. The
     * original file is overwritten in place, so this is gated behind an explicit
     * flag to never clobber a .dsk by accident. */
    if (cfg->disk_writeback && (emu->card_on[CARD_IDX_microdisc] || emu->card_on[CARD_IDX_jasmin])) {
        for (int i = 0; i < emu_disk_max_drives(emu); i++) {
            /* disk_paths[] follows the OSD swaps; disk_files[] only sees argv. */
            const char* path = emu->disk_paths[i];
            if (!emu_disk_dirty(emu, i) || !path || !emu->disks[i])
                continue;
            if (sedoric_save(emu->disks[i], path)) {
                /* Report the bytes actually written to the file: an MFM image
                 * writes its mfm_raw container, a raw image writes the flat
                 * sector buffer. (disks[i]->size is always the flat buffer.) */
                uint32_t written = emu->disks[i]->is_mfm
                                       ? emu->disks[i]->mfm_raw_size
                                       : emu->disks[i]->size;
                log_info("Disk write-back: drive %c -> %s (%u bytes)",
                         'A' + i, path, written);
            } else {
                log_error("Disk write-back failed: drive %c -> %s",
                          'A' + i, path);
            }
        }
    }

    /* Write profiler report if enabled */
    if (cfg->profile_file) {
        profiler_stop(&emu->profiler);
        profiler_report_to_file(&emu->profiler, cfg->profile_file);
    }

    /* --trace-ring: at the end of the run, write the N last instructions
     * kept in memory (oldest → newest) into the trace file. */
    if (cfg->trace_file && cfg->trace_ring > 0) {
        if (trace_save_ring(&emu->trace, cfg->trace_file))
            log_info("CPU trace ring saved (%u instructions) -> %s",
                     trace_ring_count(&emu->trace), cfg->trace_file);
        else
            log_warning("CPU trace ring empty (no instructions recorded) -> %s",
                        cfg->trace_file);
    }
    /* Diagnostic: a healthy disk transfer loses no byte. If the counter
     * is non-zero, the software serviced a DRQ too late (or the model drifts). */
    if (emu->card_on[CARD_IDX_microdisc] && emu->microdisc.fdc.lost_data_count)
        log_warning("FDC: %u octet(s) signalé(s) perdus (LOST DATA) pendant la session",
                    emu->microdisc.fdc.lost_data_count);

    trace_close(&emu->trace);
    if (cfg->cycle_trace_file) {
        uint64_t ct_lines = cycle_trace_close();
        log_info("Cycle trace: %llu lines -> %s",
                 (unsigned long long)ct_lines, cfg->cycle_trace_file);
    }

    /* An armed --*-when that never fired = explicit failure (exit 2), so
     * that the SCUMM CI can tell « game state never reached » from a usage
     * error (exit 1) or a success (exit 0). */
    bool when_unmet = emu->when_condition_unmet;
    emulator_cleanup(emu);
    log_cleanup();

    return when_unmet ? 2 : 0;
}

int main(int argc, char* argv[]) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    emu.breakpoint = -1;
    cli_opts_t cfg_storage;
    cli_opts_t* cfg = &cfg_storage;
    cli_opts_init(cfg);
    int parse_rc = cli_parse_args(argc, argv, cfg, &emu);
    if (parse_rc >= 0) return parse_rc;
    emu.argc = argc;
    emu.argv = argv;
    /* Expansion cards saved in phosphoric.cfg (carte.*): added for those the
     * command line does not mention. The strings stay allocated (cli_opts_t
     * keeps them until the end). */
    const char* card_cfg = config_to_read(cfg);
    if (card_cfg && !cfg->no_config_cards) {
        int xc = 0;
        char** xa = cards_config_argv(card_cfg, argc, argv, &xc);
        if (xa) {
            log_info("Cartes reprises de %s (%d option(s))", card_cfg, xc - 1);
            parse_rc = cli_parse_more(xc, xa, cfg, &emu);
            if (parse_rc >= 0) return parse_rc;
        }
    }
    g_loci_menu_at = cfg->loci_menu_at;

    int rc;
    gdb_stub_t gdb_stub;  /* address kept in emu.gdb_stub until main_finish */

    if ((rc = main_setup_process(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_config(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_machine(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_input_printer(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_serial_cards(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_recordings(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_loci(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_debug_frontends(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_captures_input(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_rom_model(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_tape(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_disks_speech(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_services(&emu, cfg)) >= 0) return rc;
    if ((rc = main_setup_tracing(&emu, cfg, &gdb_stub)) >= 0) return rc;
    /* Run emulation */
    emulator_run(&emu);

    rc = main_finish(&emu, cfg, &gdb_stub);
    /* F1 menu → cards changed: cold restart (new process). */
    if (emu.restart_argv) cards_exec(emu.restart_argv);
    return rc;
}
