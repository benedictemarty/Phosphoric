/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file control_cmd_debug.c
 * @brief Commandes --control : points d'arrêt, surveillances, raster, désassemblage, trace, peek
 * @author bmarty <bmarty@mailo.com>
 *
 * Découpé de src/control.c (sprint F du plan d'architecture), sans changement
 * de comportement ; fonctions partagées : include/control_internal.h.
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE  /* macOS: _POSIX_C_SOURCE masque les extensions BSD (MSG_DONTWAIT...) */
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

void ctl_cmd_break(emulator_t* emu, control_sink_t* s,
                      const char* addr_s, const char* cond) {
    uint16_t addr;
    if (!ctl_parse_u16(addr_s, &addr)) {
        sink_err(s, "break: usage `break <addr> [if <expr>]`");
        return;
    }
    if (cond && *cond) {
        int id = debugger_add_cond_breakpoint(&emu->debugger, emu, addr, cond);
        if (id == -2) { sink_err(s, "break: bad condition `%s`", cond); return; }
        if (id < 0)   { sink_err(s, "break: full or rejected"); return; }
        sink_ok(s, "id=%d addr=%04X cond=\"%s\"", id, addr,
                emu->debugger.breakpoints[id].cond_text);
        return;
    }
    int id = debugger_add_breakpoint(&emu->debugger, addr);
    if (id < 0) { sink_err(s, "break: full or rejected"); return; }
    sink_ok(s, "id=%d addr=%04X", id, addr);
}

void ctl_cmd_unbreak(emulator_t* emu, control_sink_t* s, const char* id_s) {
    if (!id_s) { sink_err(s, "unbreak: usage `unbreak <id>`"); return; }
    int id = atoi(id_s);
    if (!debugger_remove_breakpoint(&emu->debugger, id)) {
        sink_err(s, "unbreak: invalid id");
        return;
    }
    sink_ok(s, "");
}

/* Sprint 35b — watchpoints. Sprint 97 — optional access mode w|r|a|c. */
void ctl_cmd_watch(emulator_t* emu, control_sink_t* s,
                      const char* addr_s, const char* mode_s) {
    uint16_t addr;
    if (!ctl_parse_u16(addr_s, &addr)) {
        sink_err(s, "watch: usage `watch <addr> [w|r|a|c]`");
        return;
    }
    watch_mode_t mode = WATCH_WRITE;
    const char* mname = "write";
    if (mode_s && *mode_s) {
        switch (mode_s[0]) {
            case 'w': mode = WATCH_WRITE;  mname = "write";  break;
            case 'r': mode = WATCH_READ;   mname = "read";   break;
            case 'a': mode = WATCH_ACCESS; mname = "access"; break;
            case 'c': mode = WATCH_CHANGE; mname = "change"; break;
            default: sink_err(s, "watch: bad mode `%s` (w|r|a|c)", mode_s); return;
        }
    }
    int id = debugger_add_watchpoint_mode(&emu->debugger, addr, mode);
    if (id < 0) { sink_err(s, "watch: full or rejected"); return; }
    debugger_install_watchpoint_trace(&emu->debugger, emu);
    sink_ok(s, "id=%d addr=%04X mode=%s", id, addr, mname);
}

/* US 3 — access-flag map: per-byte r/w/x breakpoints over a region. */
void ctl_cmd_watch_region(emulator_t* emu, control_sink_t* s,
                             const char* start_s, const char* end_s, const char* flags_s) {
    uint16_t start, end;
    if (!ctl_parse_u16(start_s, &start) || !ctl_parse_u16(end_s, &end)) {
        sink_err(s, "watch-region: usage `watch-region <start> <end> [rwx]`");
        return;
    }
    uint8_t flags = AMAP_R | AMAP_W;   /* default rw */
    if (flags_s && *flags_s && !debugger_amap_parse_flags(flags_s, &flags)) {
        sink_err(s, "watch-region: bad flags `%s` (subset of rwx)", flags_s);
        return;
    }
    uint32_t n = debugger_amap_set(start, end, flags);
    debugger_install_watchpoint_trace(&emu->debugger, emu);
    sink_ok(s, "flagged=%u start=%04X end=%04X flags=%c%c%c", n, start, end,
            (flags & AMAP_R) ? 'r' : '-', (flags & AMAP_W) ? 'w' : '-',
            (flags & AMAP_X) ? 'x' : '-');
}

void ctl_cmd_watch_region_clear(emulator_t* emu, control_sink_t* s) {
    debugger_amap_clear();
    debugger_install_watchpoint_trace(&emu->debugger, emu);
    sink_ok(s, "");
}

void ctl_cmd_watch_region_list(emulator_t* emu, control_sink_t* s) {
    (void)emu;
    sink_printf(s, "OK count=%u", debugger_amap_count());
    int shown = 0;
    uint32_t a = 0;
    while (a < 0x10000 && shown < 64) {
        uint8_t f = debugger_amap_get((uint16_t)a);
        if (!f) { a++; continue; }
        uint32_t start = a;
        while (a < 0x10000 && debugger_amap_get((uint16_t)a) == f) a++;
        sink_printf(s, " %04X-%04X:%c%c%c", (uint16_t)start, (uint16_t)(a - 1),
                    (f & AMAP_R) ? 'r' : '-', (f & AMAP_W) ? 'w' : '-',
                    (f & AMAP_X) ? 'x' : '-');
        shown++;
    }
    sink_printf(s, "\n");
    sink_flush(s);
}

void ctl_cmd_unwatch(emulator_t* emu, control_sink_t* s, const char* id_s) {
    if (!id_s) { sink_err(s, "unwatch: usage `unwatch <id>`"); return; }
    int id = atoi(id_s);
    if (!debugger_remove_watchpoint(&emu->debugger, id)) {
        sink_err(s, "unwatch: invalid id");
        return;
    }
    debugger_install_watchpoint_trace(&emu->debugger, emu);
    sink_ok(s, "");
}

void ctl_cmd_watch_list(emulator_t* emu, control_sink_t* s) {
    debugger_t* dbg = &emu->debugger;
    sink_printf(s, "OK");
    for (int i = 0; i < dbg->num_watchpoints; i++) {
        static const char* mode_ch = "wrac";  /* write/read/access/change */
        sink_printf(s, " id=%d:addr=%04X:mode=%c", i,
                    dbg->watchpoints[i].addr, mode_ch[dbg->watchpoints[i].mode]);
    }
    sink_printf(s, "\n");
    sink_flush(s);
}

/* Sprint 35b — raster-line breakpoints (PAL 0..311). */
void ctl_cmd_raster(emulator_t* emu, control_sink_t* s, const char* line_s) {
    if (!line_s) { sink_err(s, "raster: usage `raster <line>`"); return; }
    int line = atoi(line_s);
    if (line < 0 || line >= PAL_LINES_PER_FRAME) {
        sink_err(s, "raster: line must be 0..%d", PAL_LINES_PER_FRAME - 1);
        return;
    }
    debugger_t* dbg = &emu->debugger;
    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (dbg->raster_bps[i] < 0) { slot = i; break; }
    }
    if (slot < 0) { sink_err(s, "raster: all 8 slots used"); return; }
    dbg->raster_bps[slot] = (int16_t)line;
    dbg->num_raster_bps++;
    sink_ok(s, "id=%d line=%d", slot, line);
}

void ctl_cmd_unraster(emulator_t* emu, control_sink_t* s, const char* id_s) {
    if (!id_s) { sink_err(s, "unraster: usage `unraster <id>`"); return; }
    int id = atoi(id_s);
    if (id < 0 || id >= 8 || emu->debugger.raster_bps[id] < 0) {
        sink_err(s, "unraster: invalid id");
        return;
    }
    emu->debugger.raster_bps[id] = -1;
    emu->debugger.num_raster_bps--;
    sink_ok(s, "");
}

/* Sprint 35b — disassemble N instructions starting at addr. */
void ctl_cmd_disasm(emulator_t* emu, control_sink_t* s,
                       const char* addr_s, const char* n_s) {
    uint16_t addr;
    uint32_t n;
    if (!ctl_parse_u16(addr_s, &addr) || !ctl_parse_hex(n_s, &n)) {
        sink_err(s, "disasm: usage `disasm <addr> <n>`");
        return;
    }
    if (n == 0 || n > 64) { sink_err(s, "disasm: n must be 1..64"); return; }
    /* One reply line per instruction. */
    for (uint32_t i = 0; i < n; i++) {
        char buf[64];
        int bytes = cpu_disassemble(&emu->cpu, addr, buf, sizeof(buf));
        const char* sym = symbol_lookup(&emu->symbols, addr);
        sink_printf(s, "OK addr=%04X bytes=%d disasm=\"%s\"",
                    addr, bytes, buf);
        if (sym) sink_printf(s, " label=%s", sym);
        sink_printf(s, "\n");
        addr = (uint16_t)(addr + bytes);
    }
    sink_flush(s);
}

void ctl_cmd_break_list(emulator_t* emu, control_sink_t* s) {
    debugger_t* dbg = &emu->debugger;
    sink_printf(s, "OK");
    for (int i = 0; i < dbg->num_breakpoints; i++) {
        sink_printf(s, " id=%d:addr=%04X", i, dbg->breakpoints[i].addr);
        if (dbg->breakpoints[i].has_cond)
            sink_printf(s, ":cond=\"%s\"", dbg->breakpoints[i].cond_text);
    }
    sink_printf(s, "\n");
    sink_flush(s);
}

/* Epic 6 / US 1 — conditional CPU tracing. `sub` is the subcommand and `rest`
 * the raw remainder (a trace spec, or a filename for `save`). */
void ctl_cmd_trace(emulator_t* emu, control_sink_t* s,
                      const char* sub, const char* rest) {
    cpu_trace_t* t = &emu->trace;
    if (!sub || !*sub || strcasecmp(sub, "status") == 0) {
        sink_ok(s, "active=%d armed=%d count=%llu ring=%u/%u stop_hit=%d",
                (int)t->active, (int)t->armed, (unsigned long long)t->count,
                trace_ring_count(t), t->ring_cap, (int)t->stop_hit);
    } else if (strcasecmp(sub, "start") == 0) {
        trace_start_t sc; uint16_t spc; trace_stop_t stc; uint16_t sa;
        uint64_t scy; uint32_t ring; bool sym;
        if (!trace_parse_spec(rest, &sc, &spc, &stc, &sa, &scy, &ring, &sym)) {
            sink_err(s, "trace: bad spec (now|pc:HEX stop:cycle:N|brk|write:HEX|read:HEX ring:N sym)");
            return;
        }
        trace_set_symbols(t, &emu->symbols);
        trace_arm(t, sc, spc, stc, sa, scy, ring, sym);
        trace_install_mem_hook(t, &emu->memory);
        sink_ok(s, "armed active=%d ring=%u", (int)t->active, t->ring_cap);
    } else if (strcasecmp(sub, "stop") == 0) {
        trace_stop(t);
        sink_ok(s, "count=%llu ring=%u", (unsigned long long)t->count, trace_ring_count(t));
    } else if (strcasecmp(sub, "save") == 0) {
        if (!rest || !*rest) { sink_err(s, "trace: usage `trace save <file>`"); return; }
        if (trace_save_ring(t, rest)) sink_ok(s, "saved=%u", trace_ring_count(t));
        else sink_err(s, "trace: nothing to save / write failed");
    } else if (strcasecmp(sub, "off") == 0) {
        trace_reset(t);
        trace_install_mem_hook(t, &emu->memory);   /* drops the memory hook */
        sink_ok(s, "");
    } else {
        sink_err(s, "trace: unknown subcommand `%s`", sub);
    }
}

/* Sprint 35a freeze — `peek <subsystem>` exposes the per-device REPL
 * commands (via/psg/disk/acia/tape/loci) in a single-line key=value
 * format so the IDE can populate its inspectors without parsing
 * human-friendly output. Each branch emits one line. */
void ctl_cmd_peek(emulator_t* emu, control_sink_t* s, const char* sub) {
    if (!sub) { sink_err(s, "peek: usage `peek <subsystem>`"); return; }
    if (strcmp(sub, "via") == 0) {
        via6522_t* v = &emu->via;
        via_sync(v);   /* compteurs à jour (cycles sautés par via_tick) */
        sink_ok(s, "ora=%02X orb=%02X ddra=%02X ddrb=%02X "
                "t1c=%04X t1l=%04X t2c=%04X t2l=%02X "
                "acr=%02X pcr=%02X ifr=%02X ier=%02X sr=%02X "
                "t1_run=%d t2_run=%d",
                v->ora, v->orb, v->ddra, v->ddrb,
                v->t1_counter, v->t1_latch, v->t2_counter, v->t2_latch,
                v->acr, v->pcr, v->ifr, v->ier, v->sr,
                v->t1_running ? 1 : 0, v->t2_running ? 1 : 0);
    }
    else if (strcmp(sub, "psg") == 0) {
        ay3891x_t* p = &emu->psg;
        sink_printf(s, "OK");
        for (int i = 0; i < 14; i++) sink_printf(s, " r%d=%02X", i, p->registers[i]);
        sink_printf(s, " env_period=%u env_shape=%u env_step=%u env_vol=%u",
                    p->env_period, p->env_shape, p->env_step, p->env_volume);
        sink_printf(s, "\n");
        sink_flush(s);
    }
    else if (strcmp(sub, "disk") == 0 || strcmp(sub, "fdc") == 0) {
        if (emu->card_on[CARD_IDX_jasmin]) {
            /* Jasmin has no diskrom/intena gate; DRQ (not INTRQ) drives the CPU
             * IRQ, and the side is selected externally ($03F8). */
            jasmin_t* j = &emu->jasmin;
            fdc_t* f = &j->fdc;
            sink_ok(s, "iface=jasmin intrq=%d drq=%d romdis=%d olay=%d "
                    "drive=%d side=%d cmd=%02X status=%02X trk=%02X sec=%02X "
                    "data=%02X dir=%d c_trk=%02X c_sec=%02X cur_off=%04X "
                    "drives_mounted=%d%d%d%d",
                    j->intrq == 0x00 ? 1 : 0, j->drq == 0x00 ? 1 : 0,
                    j->romdis, j->olay, j->drive, j->side,
                    f->command, f->status, f->track, f->sector, f->data,
                    f->direction, f->c_track, f->c_sector, f->cur_offset,
                    j->disk_data[0] != NULL, j->disk_data[1] != NULL,
                    j->disk_data[2] != NULL, j->disk_data[3] != NULL);
            return;
        }
        if (!emu->card_on[CARD_IDX_microdisc]) { sink_err(s, "disk: no disk controller"); return; }
        microdisc_t* md = &emu->microdisc;
        fdc_t* f = &md->fdc;
        sink_ok(s, "iface=microdisc ctrl=%02X intrq=%d drq=%d diskrom=%d romdis=%d intena=%d "
                "drive=%d side=%d cmd=%02X status=%02X trk=%02X sec=%02X "
                "data=%02X dir=%d c_trk=%02X c_sec=%02X cur_off=%04X "
                "drives_mounted=%d%d%d%d",
                md->status,
                md->intrq == 0x00 ? 1 : 0, md->drq == 0x00 ? 1 : 0,
                md->diskrom, md->romdis, md->intena, md->drive, md->side,
                f->command, f->status, f->track, f->sector, f->data,
                f->direction, f->c_track, f->c_sector, f->cur_offset,
                md->disk_data[0] != NULL, md->disk_data[1] != NULL,
                md->disk_data[2] != NULL, md->disk_data[3] != NULL);
    }
    else if (strcmp(sub, "acia") == 0 || strcmp(sub, "serial") == 0) {
        acia6551_t* a = &emu->acia;
        sink_ok(s, "tdr=%02X rdr=%02X status=%02X cmd=%02X ctrl=%02X "
                "framebits=%u baud=%u v23=%d tx_pending=%d rx_full=%d "
                "irq_line=%d dcd=%d dsr=%d cts=%d rx_fifo_count=%d "
                "rx_fifo_size=%d",
                a->tdr, a->rdr, a->status, a->command, a->control,
                a->framebits, a->baud_rate, a->v23_mode ? 1 : 0,
                a->tx_pending ? 1 : 0, a->rx_full ? 1 : 0,
                a->irq_line ? 1 : 0, a->dcd ? 1 : 0, a->dsr ? 1 : 0,
                a->cts ? 1 : 0, a->rx_fifo_count, a->rx_fifo_size);
    }
    else if (strcmp(sub, "tape") == 0 || strcmp(sub, "cassette") == 0) {
        sink_ok(s, "loaded=%d pos=%d len=%d sync_loop=%d cload_active=%d "
                "fastload_pending=%d csave_active=%d",
                emu->tape_loaded ? 1 : 0, emu->tapeoffs, emu->tapelen,
                emu->tape_syncstack >= 0 ? 1 : 0,
                emu->tape_readbyte_active ? 1 : 0,
                emu->fastload_pending ? 1 : 0,
                emu->csave_file != NULL ? 1 : 0);
    }
    else if (strcmp(sub, "loci") == 0) {
        if (!emu->card_on[CARD_IDX_loci]) { sink_err(s, "loci: inactive"); return; }
        loci_t* l = &emu->loci;
        uint16_t err = (uint16_t)l->regs[LOCI_REG_API_ERRNO_LO]
                     | ((uint16_t)l->regs[LOCI_REG_API_ERRNO_HI] << 8);
        int fd_n = 0, dir_n = 0, mnt_n = 0;
        for (int i = 0; i < LOCI_FD_MAX; i++) if (l->fd_kind[i]) fd_n++;
        for (int i = 0; i < LOCI_DIR_MAX; i++) if (l->dir_kind[i]) dir_n++;
        for (int i = 0; i < LOCI_MNT_MAX; i++) if (l->mnt_mounted[i]) mnt_n++;
        uint64_t total = 0;
        for (int i = 0; i < 256; i++) total += l->op_count[i];
        sink_ok(s, "enabled=%d active_op=%02X errno=%u busy=%02X "
                "ops_total=%llu xstack_used=%u/%d "
                "fds_open=%d dirs_open=%d mounts=%d "
                "tap_pos=%u tap_size=%u dsk_selected=%d boot_settings=%02X "
                "sdimg=%d",
                l->enabled ? 1 : 0, l->active_op, err, l->regs[LOCI_REG_BUSY],
                (unsigned long long)total,
                LOCI_XSTACK_SIZE - l->xstack_ptr, LOCI_XSTACK_SIZE,
                fd_n, dir_n, mnt_n,
                l->tap_counter, l->tap_size,
                l->dsk_selected, l->boot_settings,
                l->sdimg ? 1 : 0);
    }
    else if (strcmp(sub, "video") == 0 || strcmp(sub, "ula") == 0) {
        video_t* v = &emu->video;
        sink_ok(s, "hires=%d vid_mode=%02X text_attr=%02X w=%d h=%d frame=%u",
                v->hires_mode ? 1 : 0, v->vid_mode, v->text_attr,
                v->native_w, v->native_h, (unsigned)v->frame_counter);
    }
    else if (strcmp(sub, "kbd") == 0 || strcmp(sub, "keyboard") == 0) {
        oric_keyboard_t* k = &emu->keyboard;
        sink_printf(s, "OK layout=%d matrix=", (int)k->layout);
        for (int c = 0; c < 8; c++) sink_printf(s, "%02X", k->matrix[c]);
        sink_printf(s, "\n");
        sink_flush(s);
    }
    else if (strcmp(sub, "joy") == 0 || strcmp(sub, "joystick") == 0) {
        oric_joystick_t* j = &emu->joystick;
        sink_ok(s, "mode=%d port_a_mask=%02X present=%d",
                (int)j->mode, j->port_a_mask,
                (j->port_a_mask & IJK_PRESENCE) ? 0 : 1);
    }
    else if (strcmp(sub, "printer") == 0 || strcmp(sub, "mcp40") == 0) {
        oric_printer_t* p = &emu->printer;
        if (p->type == PRINTER_MCP40) {
            mcp40_t* m = &p->mcp40;
            sink_ok(s, "type=mcp40 byte_count=%u strobe_low=%d pen_x=%d pen_y=%d "
                    "color=%d lines=%u chars=%u dirty=%d",
                    (unsigned)p->byte_count, p->strobe_low ? 1 : 0,
                    m->pen_x, m->pen_y, (int)m->color,
                    (unsigned)m->line_count, (unsigned)m->char_count,
                    m->dirty ? 1 : 0);
        } else {
            sink_ok(s, "type=%s byte_count=%u strobe_low=%d",
                    p->type == PRINTER_TEXT ? "text" : "none",
                    (unsigned)p->byte_count, p->strobe_low ? 1 : 0);
        }
    }
    else {
        sink_err(s, "peek: unknown subsystem `%s` "
                 "(via|psg|disk|acia|tape|loci|video|kbd|joy|printer)", sub);
    }
}
