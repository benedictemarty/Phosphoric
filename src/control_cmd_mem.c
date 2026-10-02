/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file control_cmd_mem.c
 * @brief --control commands: registers, memory, search, keyboard, reset
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

/* ─── command handlers ─────────────────────────────────────────────
 * Each handler writes its reply to the sink; it no longer knows whether
 * the destination is stdout or an in-memory buffer. */

void ctl_cmd_regs(emulator_t* emu, control_sink_t* s) {
    sink_ok(s, "A=%02X X=%02X Y=%02X SP=%02X P=%02X PC=%04X cycles=%llu",
            emu->cpu.A, emu->cpu.X, emu->cpu.Y, emu->cpu.SP, emu->cpu.P,
            emu->cpu.PC, (unsigned long long)emu->cpu.cycles);
}

void ctl_cmd_set(emulator_t* emu, control_sink_t* s,
                    const char* reg, const char* val, const char* tail) {
    if (!reg || !val) { sink_err(s, "set: usage `set <reg> <val>` or `set via <reg 0-15> <val>`"); return; }
    /* `set via <reg 0-15> <val>` — write a VIA 6522 register. Here `val`
     * carries the register index and `tail` the value. */
    if (strcasecmp(reg, "via") == 0) {
        uint32_t regn, vv;
        if (!tail || !ctl_parse_hex(val, &regn) || !ctl_parse_hex(tail, &vv) || regn > 15) {
            sink_err(s, "set: usage `set via <reg 0-15> <val>`");
            return;
        }
        via_write(&emu->via, (uint8_t)regn, (uint8_t)vv);
        sink_ok(s, "via=%u val=%02X", (unsigned)regn, (uint8_t)vv);
        return;
    }
    uint32_t v;
    if (!ctl_parse_hex(val, &v)) { sink_err(s, "set: bad value"); return; }
    /* Case-insensitive: A, X, Y, SP, P, PC. */
    if (strcasecmp(reg, "a")  == 0) emu->cpu.A  = (uint8_t)v;
    else if (strcasecmp(reg, "x")  == 0) emu->cpu.X  = (uint8_t)v;
    else if (strcasecmp(reg, "y")  == 0) emu->cpu.Y  = (uint8_t)v;
    else if (strcasecmp(reg, "sp") == 0) emu->cpu.SP = (uint8_t)v;
    else if (strcasecmp(reg, "p")  == 0) emu->cpu.P  = (uint8_t)v;
    else if (strcasecmp(reg, "pc") == 0) emu->cpu.PC = (uint16_t)v;
    else { sink_err(s, "set: unknown reg `%s`", reg); return; }
    sink_ok(s, "");
}

void ctl_cmd_read(emulator_t* emu, control_sink_t* s,
                     const char* addr_s, const char* len_s, const char* bank_s) {
    uint16_t addr;
    uint32_t len;
    if (!ctl_parse_u16(addr_s, &addr) || !ctl_parse_hex(len_s, &len)) {
        sink_err(s, "read: usage `read <addr> <len> [cpu|ram|rom|overlay]`");
        return;
    }
    if (len > 4096) { sink_err(s, "read: len > 4096"); return; }
    peek_bank_t bank = PEEK_CPU;
    if (bank_s && *bank_s && !debugger_parse_bank(bank_s, &bank)) {
        sink_err(s, "read: bad bank `%s` (cpu|ram|rom|overlay)", bank_s);
        return;
    }
    /* Build the reply : "OK <hex bytes>". */
    sink_printf(s, "OK");
    for (uint32_t i = 0; i < len; i++) {
        sink_printf(s, " %02X", debugger_peek_bank(emu, (uint16_t)(addr + i), bank));
    }
    sink_printf(s, "\n");
    sink_flush(s);
}

/* Sprint 35c — length-prefixed binary read. Up to 64 KB per call.
 * Wire format:
 *   client → `bread $XXXX <len>\n`
 *   server → `OK bread len=<len>\n`
 *   server → <len raw bytes>
 *   server → `\n`
 * The trailing newline lets a line-based client reader resync after
 * the binary chunk. The client must temporarily switch to raw-read
 * mode for the binary section (see phos_smoke_client.py::bread). */
void ctl_cmd_bread(emulator_t* emu, control_sink_t* s,
                      const char* addr_s, const char* len_s) {
    uint16_t addr;
    uint32_t len;
    if (!ctl_parse_u16(addr_s, &addr) || !ctl_parse_hex(len_s, &len)) {
        sink_err(s, "bread: usage `bread <addr> <len>`");
        return;
    }
    if (len == 0 || len > 0x10000) {
        sink_err(s, "bread: len must be 1..65536");
        return;
    }
    /* Stage the buffer first, then emit the OK + binary in a single
     * flush, so a partial write can't interleave with another reply. */
    static uint8_t buf[0x10000];
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = memory_peek(&emu->memory, (uint16_t)(addr + i));
    }
    sink_printf(s, "OK bread len=%u\n", len);
    sink_write(s, buf, len);
    sink_write(s, "\n", 1);
    sink_flush(s);
}

void ctl_cmd_write(emulator_t* emu, control_sink_t* s, const char* addr_s,
                      const char* first_byte, char* rest_save) {
    uint16_t addr;
    if (!addr_s || !ctl_parse_u16(addr_s, &addr) || !first_byte) {
        sink_err(s, "write: usage `write <addr> <byte>...`");
        return;
    }
    uint8_t b;
    if (!ctl_parse_u8(first_byte, &b)) {
        sink_err(s, "write: bad byte at offset 0");
        return;
    }
    memory_write(&emu->memory, addr, b);
    int n = 1;
    char* tok;
    while ((tok = strtok_r(NULL, " \t", &rest_save)) != NULL) {
        if (!ctl_parse_u8(tok, &b)) {
            sink_err(s, "write: bad byte at offset %d", n);
            return;
        }
        memory_write(&emu->memory, (uint16_t)(addr + n), b);
        n++;
    }
    sink_ok(s, "count=%d", n);
}

/* US 6 — RAM stuck-bit fault injection. */
void ctl_cmd_stuck_bits(emulator_t* emu, control_sink_t* s,
                           const char* s0_s, const char* s1_s) {
    if (!s0_s || !*s0_s) {
        sink_ok(s, "stuck0=%02X stuck1=%02X", emu->memory.stuck0, emu->memory.stuck1);
        return;
    }
    uint32_t s0 = 0, s1 = 0;
    if (!ctl_parse_hex(s0_s, &s0) || (s1_s && *s1_s && !ctl_parse_hex(s1_s, &s1)) ||
        s0 > 0xFF || s1 > 0xFF) {
        sink_err(s, "stuck-bits: usage `stuck-bits <s0> [s1]`");
        return;
    }
    memory_set_stuck_bits(&emu->memory, (uint8_t)s0, (uint8_t)s1);
    sink_ok(s, "stuck0=%02X stuck1=%02X", (uint8_t)s0, (uint8_t)s1);
}

/* Sprint 97 — iterative memory search (cheat-finder), mirrors the REPL `hunt`.
 * `hunt` (no op) seeds; op ∈ {eq <v>, same, changed, up, down, list, clear}. */
void ctl_cmd_hunt(emulator_t* emu, control_sink_t* s,
                     const char* op, const char* val_s) {
    if (!op || !*op) {
        debugger_hunt_start(emu);
        sink_ok(s, "candidates=%u", debugger_hunt_count());
        return;
    }
    if (strcasecmp(op, "clear") == 0) { debugger_hunt_clear(); sink_ok(s, ""); return; }
    if (strcasecmp(op, "list") == 0) {
        if (!debugger_hunt_active()) { sink_err(s, "hunt: not active"); return; }
        uint16_t addrs[64]; uint8_t vals[64];
        uint32_t n = debugger_hunt_list(emu, addrs, 64, vals);
        sink_printf(s, "OK count=%u", debugger_hunt_count());
        for (uint32_t i = 0; i < n; i++)
            sink_printf(s, " %04X=%02X", addrs[i], vals[i]);
        sink_printf(s, "\n");
        sink_flush(s);
        return;
    }
    if (!debugger_hunt_active()) { sink_err(s, "hunt: not active (send `hunt` first)"); return; }
    hunt_pred_t pred; uint8_t val = 0;
    if (strcasecmp(op, "eq") == 0) {
        uint32_t v;
        if (!val_s || !ctl_parse_hex(val_s, &v) || v > 0xFF) { sink_err(s, "hunt: eq needs a byte"); return; }
        pred = HUNT_EQ; val = (uint8_t)v;
    } else if (strcasecmp(op, "same") == 0)    pred = HUNT_UNCHANGED;
    else if (strcasecmp(op, "changed") == 0)   pred = HUNT_CHANGED;
    else if (strcasecmp(op, "up") == 0)        pred = HUNT_GT;
    else if (strcasecmp(op, "down") == 0)      pred = HUNT_LT;
    else { sink_err(s, "hunt: op ∈ {eq,same,changed,up,down,list,clear}"); return; }
    uint32_t k = debugger_hunt_refine(emu, pred, val);
    sink_ok(s, "candidates=%u", k);
}

/* Sprint 97 — memory ⇄ file region + full save-state from the protocol. */
void ctl_cmd_save_mem(emulator_t* emu, control_sink_t* s,
                         const char* path, const char* addr_s, const char* len_s) {
    uint16_t addr; uint32_t len;
    if (!path || !ctl_parse_u16(addr_s, &addr) || !ctl_parse_hex(len_s, &len)) {
        sink_err(s, "save-mem: usage `save-mem <file> <addr> <len>`");
        return;
    }
    if (debugger_save_region(emu, path, addr, len)) sink_ok(s, "wrote=%u addr=%04X", len, addr);
    else sink_err(s, "save-mem: write failed");
}

void ctl_cmd_load_mem(emulator_t* emu, control_sink_t* s,
                         const char* path, const char* addr_s) {
    uint16_t addr;
    if (!path || !ctl_parse_u16(addr_s, &addr)) {
        sink_err(s, "load-mem: usage `load-mem <file> <addr>`");
        return;
    }
    long n = debugger_load_region(emu, path, addr);
    if (n < 0) sink_err(s, "load-mem: read failed");
    else sink_ok(s, "loaded=%ld addr=%04X", n, addr);
}

void ctl_cmd_reset(emulator_t* emu, control_sink_t* s) {
    cpu_reset(&emu->cpu);
    sink_ok(s, "pc=%04X", emu->cpu.PC);
}

/* Sprint 35a freeze — protocol version + capability list. Bumped whenever
 * an existing command or event changes shape (additive `caps=` extensions
 * do NOT bump the version). */
#define CONTROL_PROTO_VERSION 1
#define CONTROL_PROTO_CAPS    "step-out,peek,hello,async-pause,watch,raster,load-tap,load-rom,load-sym,disasm,bread,load-disk,eject-disk,eject-tape,loci-button,keys,watch-mode,break-cond,hunt,save-mem,load-mem,state-save,state-load,set-via,bin-literal,mem-bank,trace-cond,access-map,sym-group,stuck-bits"

void ctl_cmd_hello(control_sink_t* s, const char* arg1, const char* arg2) {
    (void)arg1; (void)arg2;
    sink_ok(s, "server=phosphoric/%s proto=%d caps=%s",
            EMU_VERSION, CONTROL_PROTO_VERSION, CONTROL_PROTO_CAPS);
}

/* keys <text> — queue keystrokes for the main loop to inject (sprint 95).
 * Escapes: \n / \r → RETURN, \t → TAB, \e → ESC, \\ → backslash; every other
 * byte is literal (spaces preserved — only the command word was stripped).
 * Appends to the emulator's growable injection buffer; the main loop presses
 * one key every few frames. Runs on the emulator thread (queue drain), so the
 * shared buffer needs no lock. */
void ctl_cmd_keys(emulator_t* emu, control_sink_t* s, const char* text) {
    if (!text) { sink_err(s, "keys: usage `keys <text>`"); return; }
    while (*text == ' ' || *text == '\t') text++;   /* skip leading blanks */
    if (!*text) { sink_err(s, "keys: empty text"); return; }

    size_t added = 0;
    for (const char* p = text; *p; p++) {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': case 'r': c = '\n'; break;   /* RETURN */
                case 't': c = '\t'; break;
                case 'e': c = (char)0x1B; break;       /* ESC */
                case 's':                              /* \s<c>: <c> with SHIFT (e.g. \sn) */
                    if (p[1] && (unsigned char)p[1] >= 0x20 && (unsigned char)p[1] < 0x80) {
                        p++; c = (char)((unsigned char)*p | 0x80); break;
                    }
                    c = 's'; break;
                case '\\': c = '\\'; break;
                default: c = *p; break;                /* literal */
            }
        }
        if (emu->kbd_inject_len + 1 > emu->kbd_inject_cap) {
            size_t ncap = emu->kbd_inject_cap ? emu->kbd_inject_cap * 2 : 64;
            char* nb = (char*)realloc(emu->kbd_inject_buf, ncap);
            if (!nb) { sink_err(s, "keys: out of memory"); return; }
            emu->kbd_inject_buf = nb;
            emu->kbd_inject_cap = ncap;
        }
        emu->kbd_inject_buf[emu->kbd_inject_len++] = c;
        added++;
    }
    sink_ok(s, "queued=%zu pending=%zu", added,
            emu->kbd_inject_len - emu->kbd_inject_pos);
}
