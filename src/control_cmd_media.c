/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file control_cmd_media.c
 * @brief --control commands: tape, floppies, ROM, symbols, states, LOCI button
 * @author bmarty <bmarty@mailo.com>
 *
 * Split out of src/control.c (sprint F of the architecture plan), with no
 * behaviour change; shared functions: include/control_internal.h.
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE  /* macOS: _POSIX_C_SOURCE hides the BSD extensions (MSG_DONTWAIT...) */
#endif
#include "control.h"
#include "io/loci_emu.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "debugger.h"
#include "savestate.h"
#include "utils/logging.h"
#include "utils/symbols.h"
#include "io/via6522.h"
#include "audio/audio.h"
#include "io/microdisc.h"
#include "io/acia6551.h"
#include "io/loci.h"
#include "io/loci_internal.h"   /* loci_dsk_open / loci_dsk_close */
#include "storage/disk.h"
#include "storage/sedoric.h"
#ifndef _WIN32
#include <sys/select.h>
#endif
#include <unistd.h>
#include <signal.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "control_internal.h"

/* Sprint 35b — runtime load helpers. They call the same primitives as
 * the CLI bootstrap path: file existence + size + memcpy into the right
 * slot. Errors return ERR with a short description. */
bool load_file_into(const char* path, uint8_t** out_buf,
                           size_t* out_len, size_t max_len) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > max_len) { fclose(fp); return false; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(fp); return false; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (n != (size_t)sz) { free(buf); return false; }
    *out_buf = buf;
    *out_len = (size_t)sz;
    return true;
}

void ctl_cmd_load_tap(emulator_t* emu, control_sink_t* s, const char* path) {
    if (!path) { sink_err(s, "load-tap: usage `load-tap <path>`"); return; }
    uint8_t* buf = NULL;
    size_t len = 0;
    if (!load_file_into(path, &buf, &len, 1 << 20)) {
        sink_err(s, "load-tap: cannot read `%s`", path);
        return;
    }
    if (emu->tapebuf) free(emu->tapebuf);
    emu->tapebuf = buf;
    emu->tapelen = (int)len;
    emu->tapeoffs = 0;
    emu->tape_loaded = true;
    emu_set_tape_path(emu, path);
    sink_ok(s, "size=%zu", len);
}

/* Parse a drive selector: "A".."D", "a".."d" or "0".."3". -1 if invalid. */
int control_drive_index(const char* s) {
    if (!s || !s[0] || s[1] != '\0') return -1;
    char c = s[0];
    if (c >= 'a' && c <= 'd') return c - 'a';
    if (c >= 'A' && c <= 'D') return c - 'A';
    if (c >= '0' && c <= '3') return c - '0';
    return -1;
}

/* Write a modified .dsk back to its source file, mirroring osd_writeback_drive()
 * in main.c. Opt-in via --disk-writeback; only when the drive is dirty and has a
 * known source path. Returns true if a write-back actually happened. */
bool control_writeback_drive(emulator_t* emu, int drv) {
    if (!emu->disk_writeback || drv < 0 || drv >= emu_disk_max_drives(emu)) return false;
    if (!emu_disk_dirty(emu, drv) || !emu->disks[drv] || !emu->disk_paths[drv])
        return false;
    bool ok = sedoric_save(emu->disks[drv], emu->disk_paths[drv]);
    log_info("control: write-back drive %c -> %s (%s)", 'A' + drv,
             emu->disk_paths[drv], ok ? "OK" : "FAIL");
    emu_disk_clear_dirty(emu, drv);
    return ok;
}

/* load-disk <drive> <path> — hot-swap a .dsk into drive A-D. Mirrors the OSD
 * hot-load path: write back the outgoing disk (if dirty + --disk-writeback),
 * free it, then load and wire the new image. */
void ctl_cmd_load_disk(emulator_t* emu, control_sink_t* s,
                          const char* drive_s, const char* path) {
    if (!drive_s || !path) {
        sink_err(s, "load-disk: usage `load-disk <drive A-D> <path>`");
        return;
    }
    int drv = control_drive_index(drive_s);
    if (emu_loci_disks(emu)) {   /* LOCI only: its drive, like the F1 menu */
        if (drv < 0) { sink_err(s, "load-disk: drive must be A-D"); return; }
        if (!loci_dsk_open(&emu->loci, (uint8_t)drv, path)) {
            sink_err(s, "load-disk: cannot read `%s`", path);
            return;
        }
        log_info("control: disk %c <- %s (LOCI)", 'A' + drv, path);
        sink_ok(s, "drive=%c loci=1 size=%u tracks=%u sectors=%u", 'A' + drv,
                emu->loci.dsk_image_size[drv], emu->loci.dsk_tracks[drv],
                emu->loci.dsk_sectors[drv]);
        return;
    }
    if (!emu_has_disk_iface(emu)) {
        sink_err(s, emu_loci_fw_disks(emu)
                 ? "load-disk: LOCI firmware mounts its own images (MENU button)"
                 : "load-disk: no disk controller (need --disk-rom or --jasmin-rom)");
        return;
    }
    if (drv < 0) { sink_err(s, "load-disk: drive must be A-D"); return; }

    sedoric_disk_t* nd = sedoric_load(path);
    if (!nd) { sink_err(s, "load-disk: cannot read `%s`", path); return; }

    control_writeback_drive(emu, drv);
    if (emu->disks[drv]) sedoric_destroy(emu->disks[drv]);
    emu->disks[drv] = nd;
    emu_disk_clear_dirty(emu, drv);
    emu_disk_wire(emu, drv, nd);
    emu_set_disk_path(emu, drv, path);
    log_info("control: disk %c <- %s", 'A' + drv, path);
    sink_ok(s, "drive=%c size=%u tracks=%u sectors=%u", 'A' + drv,
            nd->size, nd->tracks, nd->sectors);
}

/* eject-disk <drive> — empty drive A-D, writing back first if dirty. */
void ctl_cmd_eject_disk(emulator_t* emu, control_sink_t* s,
                           const char* drive_s) {
    int drv = control_drive_index(drive_s);
    if (emu_loci_disks(emu)) {
        if (drv < 0) { sink_err(s, "eject-disk: usage `eject-disk <drive A-D>`"); return; }
        if (!emu->loci.dsk_host_path[drv][0] && !emu->loci.dsk_image[drv]) {
            sink_err(s, "eject-disk: drive %c already empty", 'A' + drv);
            return;
        }
        loci_dsk_close(&emu->loci, (uint8_t)drv);   /* writes the modified sectors */
        log_info("control: drive %c ejected (LOCI)", 'A' + drv);
        sink_ok(s, "drive=%c ejected loci=1", 'A' + drv);
        return;
    }
    if (!emu_has_disk_iface(emu)) {
        sink_err(s, emu_loci_fw_disks(emu)
                 ? "eject-disk: LOCI firmware mounts its own images (MENU button)"
                 : "eject-disk: no disk controller");
        return;
    }
    if (drv < 0) { sink_err(s, "eject-disk: usage `eject-disk <drive A-D>`"); return; }
    if (!emu->disks[drv]) { sink_err(s, "eject-disk: drive %c already empty", 'A' + drv); return; }

    bool wb = control_writeback_drive(emu, drv);
    sedoric_destroy(emu->disks[drv]);
    emu->disks[drv] = NULL;
    emu_set_disk_path(emu, drv, NULL);
    emu_disk_wire(emu, drv, NULL);
    log_info("control: drive %c ejected", 'A' + drv);
    sink_ok(s, "drive=%c ejected writeback=%d", 'A' + drv, wb ? 1 : 0);
}

/* loci-button [long] — LOCI Action button, warm press + release. Same
 * path as F8 in the GUI: session snapshot, IRQ trap, then boot the menu
 * ROM (short) or the test108k diagnostic ROM ("long", ≥ 2 s hold). */
void ctl_cmd_loci_button(emulator_t* emu, control_sink_t* s, const char* mode) {
    if (!emu->card_on[CARD_IDX_loci]) {
        sink_err(s, "loci-button: LOCI not enabled (--loci)");
        return;
    }
    bool longp = (mode && strcmp(mode, "long") == 0);
    if (loci_emu_active()) {
        /* Co-simulation: the real firmware owns the button (same rule as F8
         * in the GUI) -- short = menu, long = embedded diagnostic ROM. */
        bool armed = longp ? loci_emu_diag_button() : loci_emu_menu_button();
        if (armed) cpu_reset(&emu->cpu);
        sink_ok(s, "action-button pulsed%s (firmware, %s)", longp ? " (long)" : "",
                armed ? "ROM service armed, CPU reset"
                      : loci_emu_button_was_warm() ? "warm freeze: IRQ trap, no reset"
                                                   : "not armed");
        return;
    }
    emu->loci_button_long = longp;
    loci_action_button_short(&emu->loci);
    loci_action_button_release(&emu->loci);
    sink_ok(s, "action-button pulsed%s", longp ? " (long)" : "");
}

/* eject-tape — unload the cassette and free its buffer. */
void ctl_cmd_eject_tape(emulator_t* emu, control_sink_t* s) {
    if (!emu->tape_loaded && !emu->tapebuf) {
        sink_err(s, "eject-tape: no tape loaded");
        return;
    }
    if (emu->tapebuf) { free(emu->tapebuf); emu->tapebuf = NULL; }
    emu->tapelen = 0;
    emu->tapeoffs = 0;
    emu->tape_loaded = false;
    emu_set_tape_path(emu, NULL);
    log_info("control: tape ejected");
    sink_ok(s, "ejected");
}

void ctl_cmd_load_rom(emulator_t* emu, control_sink_t* s, const char* path) {
    if (!path) { sink_err(s, "load-rom: usage `load-rom <path>`"); return; }
    uint8_t* buf = NULL;
    size_t len = 0;
    /* Cap at 16 KB — typical Oric BASIC ROM. */
    if (!load_file_into(path, &buf, &len, 16 * 1024)) {
        sink_err(s, "load-rom: cannot read `%s`", path);
        return;
    }
    if (len != 16 * 1024) {
        free(buf);
        sink_err(s, "load-rom: expected 16384 bytes, got %zu", len);
        return;
    }
    memcpy(emu->memory.rom, buf, len);
    free(buf);
    cpu_reset(&emu->cpu);
    sink_ok(s, "size=%zu pc=%04X", len, emu->cpu.PC);
}

void ctl_cmd_load_sym(emulator_t* emu, control_sink_t* s,
                         const char* path, const char* group_s) {
    if (!path) { sink_err(s, "load-sym: usage `load-sym <path> [group]`"); return; }
    uint8_t group = 0;
    if (group_s && *group_s) {
        uint32_t g;
        if (!ctl_parse_hex(group_s, &g) || g > 255) { sink_err(s, "load-sym: bad group"); return; }
        group = (uint8_t)g;
    }
    int n = symbol_table_load_group(&emu->symbols, path, group);
    if (n < 0) { sink_err(s, "load-sym: parse failed"); return; }
    sink_ok(s, "count=%d total=%d group=%u", n, emu->symbols.count, group);
}

/* US 4 — enable/disable a symbol group. */
void ctl_cmd_sym_group(emulator_t* emu, control_sink_t* s,
                          const char* group_s, const char* onoff_s) {
    uint32_t g;
    if (!group_s || !ctl_parse_hex(group_s, &g) || g > 255 || !onoff_s) {
        sink_err(s, "sym-group: usage `sym-group <N> <on|off>`");
        return;
    }
    bool en = (strcasecmp(onoff_s, "on") == 0 || strcmp(onoff_s, "1") == 0);
    symbol_set_group_enabled(&emu->symbols, (uint8_t)g, en);
    sink_ok(s, "group=%u enabled=%d symbols=%d",
            g, en ? 1 : 0, symbol_group_count(&emu->symbols, (uint8_t)g));
}

void ctl_cmd_state_save(emulator_t* emu, control_sink_t* s, const char* path) {
    if (!path) { sink_err(s, "state-save: usage `state-save <file>`"); return; }
    if (savestate_save(emu, path)) sink_ok(s, "");
    else sink_err(s, "state-save: failed");
}

void ctl_cmd_state_load(emulator_t* emu, control_sink_t* s, const char* path) {
    if (!path) { sink_err(s, "state-load: usage `state-load <file>`"); return; }
    if (savestate_load(emu, path)) sink_ok(s, "pc=%04X", emu->cpu.PC);
    else sink_err(s, "state-load: failed");
}
