/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file debugger_view.c
 * @brief Debugger: display (registers, memory, disassembly, peripheral state)
 * @author bmarty <bmarty@mailo.com>
 *
 * Split out of src/debugger.c (sprint F of the architecture plan), with no
 * behaviour change; shared symbols: include/debugger_internal.h.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "debugger.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "cpu/cpu_internal.h"   /* opcode_table[] — source of truth for the assembler */
#include "memory/memory.h"
#include "io/via6522.h"
#include "audio/audio.h"
#include "savestate.h"
#include "debugger_internal.h"

/* ═══════════════════════════════════════════════════════════════════ */
/*  DISPLAY HELPERS                                                    */
/* ═══════════════════════════════════════════════════════════════════ */

void dbg_show_registers(emulator_t* emu) {
    char state[128];
    cpu_get_state_string(&emu->cpu, state, sizeof(state));
    const char* sym = symbol_lookup(&emu->symbols, emu->cpu.PC);
    if (sym) printf("%s  <%s>\n", state, sym);
    else     printf("%s\n", state);
}

/* Disassemble `count` instructions starting at `addr`. Returns the
 * address of the byte just past the final instruction (next page start). */
/* Sprint 34d1 P0-A — scan a disasm string for `$XXXX` operands and
 * append `; $XXXX=<name>` comments resolved through the loaded symbol
 * table. The buf itself is left untouched (width-sensitive %-18s
 * alignment) — we only print a suffix when at least one symbol matched. */
static void append_operand_symbols(const symbol_table_t* tbl, const char* buf) {
    if (!tbl || tbl->count == 0) return;
    bool printed = false;
    const char* p = buf;
    while (*p) {
        if (*p == '$') {
            const char* q = p + 1;
            int n = 0;
            unsigned int v = 0;
            while (n < 4 && isxdigit((unsigned char)*q)) {
                int d = *q;
                d = (d <= '9') ? (d - '0')
                  : ((d & 0xDF) - 'A' + 10);
                v = (v << 4) | (unsigned)d;
                q++; n++;
            }
            if (n == 4) {
                const char* s = symbol_lookup(tbl, (uint16_t)v);
                if (s) {
                    printf("%s $%04X=%s", printed ? "," : "  ;",
                           (uint16_t)v, s);
                    printed = true;
                }
                p = q;
                continue;
            }
        }
        p++;
    }
}

uint16_t dbg_show_disassembly(emulator_t* emu, uint16_t addr, int count) {
    for (int i = 0; i < count; i++) {
        char buf[64];
        int bytes = cpu_disassemble(&emu->cpu, addr, buf, sizeof(buf));
        const char* sym = symbol_lookup(&emu->symbols, addr);
        if (sym) printf("  %s:\n", sym);
        printf("  $%04X: ", addr);
        for (int b = 0; b < 3; b++) {
            if (b < bytes)
                printf("%02X ", memory_peek(&emu->memory, (uint16_t)(addr + b)));
            else
                printf("   ");
        }
        uint8_t opc = memory_peek(&emu->memory, addr);
        printf(" %-18s ; %u cyc", buf, cpu_opcode_cycles(opc));
        append_operand_symbols(&emu->symbols, buf);   /* sprint 34d1 P0-A */
        if (addr == emu->cpu.PC)
            printf("  <---");
        printf("\n");
        addr = (uint16_t)(addr + bytes);
    }
    return addr;
}

void dbg_show_memory_dump(emulator_t* emu, uint16_t addr, int len, peek_bank_t bank) {
    if (bank != PEEK_CPU)
        printf("  [bank: %s]\n", debugger_bank_name(bank));
    for (int offset = 0; offset < len; offset += 16) {
        printf("  $%04X: ", (uint16_t)(addr + offset));
        /* Hex */
        for (int i = 0; i < 16 && (offset + i) < len; i++) {
            printf("%02X ", debugger_peek_bank(emu, (uint16_t)(addr + offset + i), bank));
        }
        /* Pad if last line is short */
        int remaining = len - offset;
        if (remaining < 16) {
            for (int i = remaining; i < 16; i++)
                printf("   ");
        }
        /* ASCII */
        printf(" |");
        for (int i = 0; i < 16 && (offset + i) < len; i++) {
            uint8_t c = debugger_peek_bank(emu, (uint16_t)(addr + offset + i), bank);
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("|\n");
    }
}

void dbg_show_stack(emulator_t* emu) {
    uint8_t sp = emu->cpu.SP;
    int depth = 0xFF - sp;
    if (depth <= 0) {
        printf("  Stack is empty (SP=$%02X)\n", sp);
        return;
    }
    if (depth > 32) depth = 32; /* Limit display */
    printf("  SP=$%02X, depth=%d bytes\n", sp, 0xFF - sp);
    printf("  $01%02X: ", (uint8_t)(sp + 1));
    for (int i = 1; i <= depth; i++) {
        printf("%02X ", memory_peek(&emu->memory, (uint16_t)(0x0100 + sp + i)));
        if (i % 16 == 0 && i < depth)
            printf("\n         ");
    }
    printf("\n");
}

void dbg_show_via_state(emulator_t* emu) {
    via6522_t* via = &emu->via;
    via_sync(via);   /* up-to-date counters (cycles skipped by via_tick) */
    printf("  VIA 6522 State:\n");
    printf("    ORA=$%02X ORB=$%02X  IRA=$%02X IRB=$%02X\n",
           via->ora, via->orb, via->ira, via->irb);
    printf("    DDRA=$%02X DDRB=$%02X\n", via->ddra, via->ddrb);
    printf("    T1: counter=$%04X latch=$%04X running=%s\n",
           via->t1_counter, via->t1_latch, via->t1_running ? "yes" : "no");
    printf("    T2: counter=$%04X latch=$%02X running=%s\n",
           via->t2_counter, via->t2_latch, via->t2_running ? "yes" : "no");
    printf("    ACR=$%02X PCR=$%02X\n", via->acr, via->pcr);
    printf("    IFR=$%02X IER=$%02X  SR=$%02X\n", via->ifr, via->ier, via->sr);
    /* Decode ACR */
    printf("    ACR decode: T1=%s T2=%s SR=%d\n",
           (via->acr & 0x40) ? "free-run" : "one-shot",
           (via->acr & 0x20) ? "count-PB6" : "one-shot",
           (via->acr >> 2) & 0x07);
    /* IRQ status */
    printf("    IRQ: %s (IFR & IER = $%02X)\n",
           (via->ifr & 0x80) ? "ASSERTED" : "inactive",
           via->ifr & via->ier & 0x7F);
}

void dbg_show_psg_state(emulator_t* emu) {
    ay3891x_t* psg = &emu->psg;
    printf("  AY-3-8910 PSG State:\n");
    printf("    Registers: ");
    for (int i = 0; i < 14; i++)
        printf("%02X ", psg->registers[i]);
    printf("\n");
    /* Decode tone periods */
    for (int ch = 0; ch < 3; ch++) {
        uint16_t period = psg->registers[ch * 2] | ((psg->registers[ch * 2 + 1] & 0x0F) << 8);
        uint8_t vol = psg->registers[8 + ch];
        bool env = (vol & 0x10) != 0;
        printf("    Chan %c: period=%4d vol=%s%d\n",
               'A' + ch, period, env ? "E" : "", vol & 0x0F);
    }
    /* Noise */
    printf("    Noise: period=%d\n", psg->registers[6] & 0x1F);
    /* Mixer */
    uint8_t mix = psg->registers[7];
    printf("    Mixer ($%02X): Tone=%c%c%c Noise=%c%c%c\n", mix,
           (mix & 0x01) ? '-' : 'A',
           (mix & 0x02) ? '-' : 'B',
           (mix & 0x04) ? '-' : 'C',
           (mix & 0x08) ? '-' : 'A',
           (mix & 0x10) ? '-' : 'B',
           (mix & 0x20) ? '-' : 'C');
    /* Envelope */
    printf("    Envelope: period=%d shape=%d step=%d vol=%d %s\n",
           psg->env_period, psg->env_shape, psg->env_step,
           psg->env_volume, psg->env_holding ? "(holding)" : "");
}

/* Sprint 34d2 P1-C — WD1793 Microdisc FDC + 4 drives mount info. */
void dbg_show_disk_state(emulator_t* emu) {
    if (!emu->card_on[CARD_IDX_microdisc]) {
        printf("  Microdisc: not active (use --disk-rom roms/microdis.rom)\n");
        return;
    }
    microdisc_t* md = &emu->microdisc;
    fdc_t* fdc = &md->fdc;
    printf("  Microdisc WD1793 State:\n");
    printf("    CTRL=$%02X  INTRQ=%s  DRQ=%s\n",
           md->status,
           md->intrq == 0x00 ? "asserted" : "clear",
           md->drq   == 0x00 ? "asserted" : "clear");
    printf("    Decoded: diskrom=%d romdis=%d intena=%d drive=%c side=%d\n",
           md->diskrom, md->romdis, md->intena, 'A' + md->drive, md->side);
    printf("  FDC registers:\n");
    printf("    CMD=$%02X  STATUS=$%02X  TRK=$%02X SEC=$%02X DATA=$%02X DIR=%d\n",
           fdc->command, fdc->status, fdc->track, fdc->sector,
           fdc->data, fdc->direction);
    printf("    Physical: c_track=$%02X c_sector=$%02X side=%d  cur_offset=$%04X/$%04X\n",
           fdc->c_track, fdc->c_sector, fdc->side,
           fdc->cur_offset, fdc->cur_sector_len);
    printf("    Delays:   drq=%d intrq=%d\n",
           fdc->delayed_drq, fdc->delayed_int);
    printf("  Drives:\n");
    for (int i = 0; i < 4; i++) {
        if (md->disk_data[i]) {
            printf("    %c: %u bytes, %d tracks, %d sectors\n",
                   'A' + i, md->disk_size[i],
                   md->disk_tracks[i], md->disk_sectors[i]);
        } else {
            printf("    %c: (unmounted)\n", 'A' + i);
        }
    }
}

/* Sprint 34d2 P1-D — ACIA 6551 serial registers + signals + FIFO. */
void dbg_show_acia_state(emulator_t* emu) {
    acia6551_t* a = &emu->acia;
    printf("  ACIA 6551 State:\n");
    printf("    TDR=$%02X RDR=$%02X  STATUS=$%02X  CMD=$%02X  CTRL=$%02X\n",
           a->tdr, a->rdr, a->status, a->command, a->control);
    printf("    Frame: %u bits  baud=%u Hz  data_mask=$%02X%s\n",
           a->framebits, a->baud_rate, a->bitmask,
           a->v23_mode ? "  V23(asym 1200/75)" : "");
    printf("    Flags: tx_pending=%d rx_full=%d irq_line=%d%s\n",
           a->tx_pending, a->rx_full, a->irq_line,
           a->irq_on_rdrf ? "  (irq-on-rdrf)" : "");
    printf("    Signals: DCD=%d DSR=%d CTS=%d\n", a->dcd, a->dsr, a->cts);
    printf("    Timing: tx=%d/%d cyc  rx=%d/%d cyc\n",
           a->tx_cycles, a->tx_reload, a->rx_cycles, a->rx_reload);
    if (a->rx_fifo_size > 0) {
        printf("    RX FIFO: %d/%d bytes (head=%d tail=%d)\n",
               a->rx_fifo_count, a->rx_fifo_size,
               a->rx_fifo_head, a->rx_fifo_tail);
    } else {
        printf("    RX FIFO: disabled (1-byte mode)\n");
    }
    printf("    Backend: %s  Trace: %s\n",
           a->backend ? "attached" : "none",
           a->trace_file ? "active" : "off");
}

/* Sprint 34d2 P1-E — Cassette tape state (position, length, status). */
void dbg_show_tape_state(emulator_t* emu) {
    printf("  Tape State:\n");
    if (!emu->tape_loaded) {
        printf("    No tape loaded (use -t FILE)\n");
        return;
    }
    int pos = emu->tapeoffs;
    int len = emu->tapelen;
    double pct = len > 0 ? 100.0 * pos / len : 0.0;
    printf("    Position: %d / %d bytes (%.1f%%)\n", pos, len, pct);
    printf("    Tape path: %s\n", emu->tape_path ? emu->tape_path : "(memory only)");
    printf("    Status: %s%s%s\n",
           emu->tape_syncstack >= 0 ? "[sync loop active] " : "",
           emu->tape_readbyte_active ? "[CLOAD reading] " : "",
           emu->fastload_pending ? "[fastload pending] " : "");
    /* Preview the next 16 bytes if available, useful to see the header type. */
    if (pos < len && emu->tapebuf) {
        int show = (len - pos) < 16 ? (len - pos) : 16;
        printf("    Next %d bytes:", show);
        for (int i = 0; i < show; i++) printf(" %02X", emu->tapebuf[pos + i]);
        printf("\n");
    }
    if (emu->csave_file) {
        printf("    CSAVE: active (capturing to .TAP file)\n");
    }
}

/* Sprint 34d3 P0-B — Full LOCI introspection (MIA register file, xstack,
 * xram windows, fd/dir tables, mount table, TAP/DSK backend, errno, top
 * ops by count). 2780 LOC of LOCI had zero debugger surface before. */
void dbg_show_loci_state(emulator_t* emu) {
    if (!emu->has_loci) {
        printf("  LOCI: not active (use --loci or --loci-sdimg PATH)\n");
        return;
    }
    loci_t* l = &emu->loci;

    printf("  LOCI MIA State (enabled=%d):\n", l->enabled);

    /* Activity: current op + errno + return regs. */
    uint16_t err = (uint16_t)l->regs[LOCI_REG_API_ERRNO_LO]
                 | ((uint16_t)l->regs[LOCI_REG_API_ERRNO_HI] << 8);
    printf("    active_op=$%02X  errno=%u  A=$%02X X=$%02X "
           "SREG=$%02X%02X  BUSY=$%02X\n",
           l->active_op, err,
           l->regs[LOCI_REG_API_A], l->regs[LOCI_REG_API_X],
           l->regs[LOCI_REG_API_SREG_HI], l->regs[LOCI_REG_API_SREG],
           l->regs[LOCI_REG_BUSY]);

    /* Top 5 ops by count. */
    uint64_t total = 0;
    int top[5] = {-1, -1, -1, -1, -1};
    for (int op = 1; op < 256; op++) {
        total += l->op_count[op];
        for (int s = 0; s < 5; s++) {
            if (top[s] < 0 ||
                l->op_count[op] > l->op_count[top[s]]) {
                for (int m = 4; m > s; m--) top[m] = top[m - 1];
                top[s] = op;
                break;
            }
        }
    }
    printf("    Ops dispatched: %llu total\n", (unsigned long long)total);
    if (total > 0) {
        printf("    Top:");
        for (int s = 0; s < 5 && top[s] >= 0 &&
                       l->op_count[top[s]] > 0; s++) {
            printf("  $%02X×%llu", (uint8_t)top[s],
                   (unsigned long long)l->op_count[top[s]]);
        }
        printf("\n");
    }

    /* xstack — print pointer + a sample of the top bytes. */
    printf("    xstack_ptr=$%04X (used %u/%u):", l->xstack_ptr,
           LOCI_XSTACK_SIZE - l->xstack_ptr, LOCI_XSTACK_SIZE);
    int used = LOCI_XSTACK_SIZE - l->xstack_ptr;
    int show = used < 16 ? used : 16;
    for (int i = 0; i < show; i++)
        printf(" %02X", l->xstack[l->xstack_ptr + i]);
    if (used > 16) printf(" …");
    printf("\n");

    /* xram windows. */
    uint16_t a0 = (uint16_t)l->regs[LOCI_REG_ADDR0_LO]
                | ((uint16_t)l->regs[LOCI_REG_ADDR0_HI] << 8);
    uint16_t a1 = (uint16_t)l->regs[LOCI_REG_ADDR1_LO]
                | ((uint16_t)l->regs[LOCI_REG_ADDR1_HI] << 8);
    int8_t s0 = (int8_t)l->regs[LOCI_REG_STEP0];
    int8_t s1 = (int8_t)l->regs[LOCI_REG_STEP1];
    printf("    xram window0 addr=$%04X step=%+d   window1 addr=$%04X step=%+d\n",
           a0, s0, a1, s1);
    printf("    HID xram: kbd=$%04X mou=$%04X pad=$%04X\n",
           l->kbd_xram, l->mou_xram, l->pad_xram);

    /* Sandbox + SDIMG backend. */
    printf("    flash_root=\"%s\"  sdimg=%s\n",
           l->flash_root[0] ? l->flash_root : "(cwd)",
           l->sdimg ? "attached" : "none");

    /* File handles. */
    int fd_active = 0;
    for (int i = 0; i < LOCI_FD_MAX; i++)
        if (l->fd_kind[i] != 0) fd_active++;
    if (fd_active > 0) {
        printf("    File handles: %d/%d open\n", fd_active, LOCI_FD_MAX);
        for (int i = 0; i < LOCI_FD_MAX; i++) {
            if (l->fd_kind[i] != 0) {
                printf("      fd=%d  %s\n", i + LOCI_FD_OFFSET,
                       l->fd_kind[i] == 1 ? "POSIX" :
                       l->fd_kind[i] == 2 ? "SDIMG" : "?");
            }
        }
    } else {
        printf("    File handles: 0 open\n");
    }

    /* Dir handles. */
    int dir_active = 0;
    for (int i = 0; i < LOCI_DIR_MAX; i++)
        if (l->dir_kind[i] != 0) dir_active++;
    if (dir_active > 0) {
        printf("    Dir handles: %d/%d open\n", dir_active, LOCI_DIR_MAX);
        for (int i = 0; i < LOCI_DIR_MAX; i++) {
            if (l->dir_kind[i] != 0) {
                printf("      dir_fd=%d  path=\"%s\"\n",
                       i + LOCI_DIR_OFFSET,
                       l->dirs_path[i][0] ? l->dirs_path[i] : "(none)");
            }
        }
    } else {
        printf("    Dir handles: 0 open\n");
    }

    /* Mount table. */
    static const char* mnt_label[LOCI_MNT_MAX] = {
        "drive A", "drive B", "drive C", "drive D", "TAP", "ROM" };
    int mnt_active = 0;
    for (int i = 0; i < LOCI_MNT_MAX; i++)
        if (l->mnt_mounted[i]) mnt_active++;
    if (mnt_active > 0) {
        printf("    Mounts: %d/%d active\n", mnt_active, LOCI_MNT_MAX);
        for (int i = 0; i < LOCI_MNT_MAX; i++) {
            if (l->mnt_mounted[i]) {
                printf("      [%d %-8s] %s\n", i, mnt_label[i],
                       l->mnt_paths[i][0] ? l->mnt_paths[i] : "(no path)");
            }
        }
    } else {
        printf("    Mounts: 0 active\n");
    }

    /* TAP backend. */
    if (l->tap_fp) {
        double pct = l->tap_size > 0 ? 100.0 * l->tap_counter / l->tap_size : 0.0;
        printf("    TAP: %u/%u bytes (%.1f%%) cmd=$%02X stat=$%02X\n",
               l->tap_counter, l->tap_size, pct, l->tap_cmd, l->tap_stat);
    } else {
        printf("    TAP: idle\n");
    }

    /* DSK bus selection + per-drive status. */
    printf("    DSK selected=%c  CTRL=$%02X  INTRQ=%s INTENA=%d\n",
           'A' + l->dsk_selected, l->dsk_ctrl,
           l->dsk_intrq == 0x00 ? "asserted" : "clear",
           l->dsk_intena);
    for (int i = 0; i < 4; i++) {
        if (l->dsk_image[i]) {
            printf("      %c: %u bytes, %d tracks, %d sec/track  %s%s\n",
                   'A' + i, l->dsk_image_size[i],
                   l->dsk_tracks[i], l->dsk_sectors[i],
                   l->dsk_is_mfm[i] ? "MFM" : "raw",
                   l->dsk_host_path[i][0] ? "" : " (no path)");
        }
    }

    /* Boot settings (last MIA_BOOT). */
    if (l->boot_settings) {
        printf("    boot_settings=$%02X:%s%s%s%s%s%s%s\n", l->boot_settings,
               (l->boot_settings & LOCI_BOOT_FDC)     ? " FDC"     : "",
               (l->boot_settings & LOCI_BOOT_TAP)     ? " TAP"     : "",
               (l->boot_settings & LOCI_BOOT_B11)     ? " B11"     : "",
               (l->boot_settings & LOCI_BOOT_TAP_BIT) ? " TAP_BIT" : "",
               (l->boot_settings & LOCI_BOOT_TAP_ALD) ? " TAP_ALD" : "",
               (l->boot_settings & LOCI_BOOT_RESUME)  ? " RESUME"  : "",
               (l->boot_settings & LOCI_BOOT_FAST)    ? " FAST"    : "");
    }
}

/* US 5 — broadening the inspection coverage (b2 window parity). */
void dbg_show_video_state(emulator_t* emu) {
    video_t* v = &emu->video;
    printf("  ULA / Video State:\n");
    printf("    mode=%s  vid_mode=$%02X  text_attr=$%02X  need_refresh=%s\n",
           v->hires_mode ? "HIRES" : "TEXT", v->vid_mode, v->text_attr,
           v->need_refresh ? "yes" : "no");
    printf("    framebuffer=%dx%d  frame_counter=%u\n",
           v->native_w, v->native_h, v->frame_counter);
}

void dbg_show_keyboard_state(emulator_t* emu) {
    oric_keyboard_t* k = &emu->keyboard;
    printf("  Keyboard (8 columns, ORB[0:2] selects col; rows active-low):\n");
    printf("    col:     0    1    2    3    4    5    6    7\n");
    printf("    matrix: ");
    for (int c = 0; c < 8; c++) printf("$%02X  ", k->matrix[c]);
    printf("\n");
    printf("    layout=%d\n", (int)k->layout);
#ifdef HAS_SDL2
    printf("    pressed_count=%d  pending=%s (scancode=%u)\n",
           k->pressed_count, k->has_pending ? "yes" : "no",
           (unsigned)k->pending_scancode);
#endif
}

void dbg_show_joystick_state(emulator_t* emu) {
    oric_joystick_t* j = &emu->joystick;
    const char* mode = j->mode == ORIC_JOY_DISABLED    ? "disabled"    :
                       j->mode == ORIC_JOY_SDL_GAMEPAD ? "sdl-gamepad" :
                       j->mode == ORIC_JOY_KEYBOARD    ? "keyboard"    : "?";
    printf("  IJK Joystick State:\n");
    printf("    mode=%s  port_a_mask=$%02X (active-low)  interface present=%s\n",
           mode, j->port_a_mask, (j->port_a_mask & IJK_PRESENCE) ? "no" : "yes");
#ifdef HAS_SDL2
    printf("    device_index=%d\n", j->device_index);
#endif
}

void dbg_show_printer_state(emulator_t* emu) {
    oric_printer_t* p = &emu->printer;
    const char* type = p->type == PRINTER_NONE  ? "none"  :
                       p->type == PRINTER_TEXT  ? "text"  :
                       p->type == PRINTER_MCP40 ? "mcp40" : "?";
    printf("  Printer State:\n");
    printf("    type=%s  output=%s  strobe_low=%s  byte_count=%u\n",
           type, p->filename ? p->filename : "(none)",
           p->strobe_low ? "yes" : "no", (unsigned)p->byte_count);
    if (p->type == PRINTER_MCP40) {
        mcp40_t* m = &p->mcp40;
        printf("    MCP-40: pen=(%d,%d) color=%d char_size=%d lines=%u chars=%u dirty=%s\n",
               m->pen_x, m->pen_y, (int)m->color, m->char_size,
               (unsigned)m->line_count, (unsigned)m->char_count,
               m->dirty ? "yes" : "no");
    }
}

void dbg_show_help(void) {
    printf("\n  Debugger Commands:\n");
    printf("  ─────────────────────────────────────────────────\n");
    printf("  s / step          Step 1 instruction\n");
    printf("  n / next          Step over (JSR → break at return)\n");
    printf("  c / continue      Continue execution\n");
    printf("  u / undo          Rewind last step (CPU+RAM, ring of 16)\n");
    printf("  r / regs          Show CPU registers\n");
    printf("  d                 Disassemble next page (page size persists)\n");
    printf("  d addr [n]        Jump-disasm; push current page to history\n");
    printf("  d +               Same as `d` (next page)\n");
    printf("  d -               Pop history (previous page)\n");
    printf("  m addr [len] [bank]  Memory dump hex+ASCII (bank: cpu|ram|rom|overlay)\n");
    printf("  m addr = V1 [V2...]  Write byte(s) to memory\n");
    printf("  a addr MNE [op]   Assemble one instruction in place (e.g. a 0400 LDA #$41)\n");
    printf("  find B1 [B2...]   Search memory for a hex byte pattern\n");
    printf("  find \"text\"       Search memory for an ASCII string\n");
    printf("  b addr            Add PC breakpoint\n");
    printf("  b addr if EXPR    Conditional breakpoint\n");
    printf("                    EXPR: TERM [&&|| TERM]... (up to 4 terms)\n");
    printf("                    TERM: REG op VAL | M[ADDR] op VAL\n");
    printf("                    REG: A X Y SP P PC   op: == != < <= > >=\n");
    printf("  b                 List all breakpoints\n");
    printf("  bd n              Delete breakpoint #n\n");
    printf("  br line           Raster bp at PAL line (0..311)\n");
    printf("  br                List raster breakpoints\n");
    printf("  brd n             Delete raster bp #n (or `brd *` to clear all)\n");
    printf("  w addr [w|r|a|c]  Add watchpoint: write/read/access/change (default write)\n");
    printf("  w                 List all watchpoints\n");
    printf("  wd n              Delete watchpoint #n\n");
    printf("  wr START END [rwx]  Access-map region breakpoint (default rw) | wr | wr clear\n");
    printf("  hunt              Start cheat-finder (all cells candidate)\n");
    printf("  hunt V | = | + | - | !   Narrow: ==V / unchanged / up / down / changed\n");
    printf("  hunt list | clear Show candidates / reset the hunt\n");
    printf("  via               Show VIA 6522 state\n");
    printf("  psg               Show PSG AY-3-8910 state\n");
    printf("  disk / fdc        Show Microdisc WD1793 + 4 drives\n");
    printf("  acia / serial     Show ACIA 6551 registers + signals + FIFO\n");
    printf("  tape / cassette   Show tape position, status, next bytes\n");
    printf("  loci              Show LOCI MIA full state (regs, fds, mounts, DSK/TAP)\n");
    printf("  video / ula       Show ULA/video state (mode, framebuffer)\n");
    printf("  kbd               Show keyboard matrix (8 columns)\n");
    printf("  joy               Show IJK joystick state\n");
    printf("  printer / mcp40   Show printer / MCP-40 plotter state\n");
    printf("  stack             Show stack contents\n");
    printf("  set reg val       Set register (A,X,Y,SP,PC,P)\n");
    printf("  set via reg val   Set VIA 6522 register (0..15)\n");
    printf("  stuck [S0 [S1]]   RAM stuck-bit fault injection (S0→0, S1→1; 0 0 = off)\n");
    printf("  save FILE a len   Write memory region to a binary file\n");
    printf("  load FILE addr    Read a binary file into memory\n");
    printf("  disf FILE a n     Disassemble n instructions to a file\n");
    printf("  ss FILE / sl FILE Save / load full machine state (.ost)\n");
    printf("  trace start ...   Conditional trace: [now|pc:HEX] [stop:cycle:N|brk|write:HEX|read:HEX] [ring:N] [sym]\n");
    printf("  trace stop|save FILE|status|off\n");
    printf("  sym [name|addr]   List symbols / resolve name or address\n");
    printf("  sym load FILE [g] | sym group N on|off | sym groups   Symbol groups (US 4)\n");
    printf("  (numbers: hex default, $ hex, %% binary; symbols if --symbols loaded)\n");
    printf("  q / quit          Quit emulator\n");
    printf("  h / help          Show this help\n");
    printf("\n");
}

