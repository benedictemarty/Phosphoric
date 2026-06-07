/**
 * @file control.c
 * @brief IPC control mode for OricForge IDE integration (sprint 35a)
 *
 * Implements the --control protocol described in include/control.h.
 * Reuses debugger.c primitives where possible.
 */

#define _POSIX_C_SOURCE 200809L
#include "control.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "debugger.h"
#include "utils/logging.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ─── output helpers ───────────────────────────────────────────────
 * All protocol output goes to stdout, one record per line, then we
 * flush so the IDE observes traffic in real time. */

static void reply_ok(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("OK", stdout);
    if (fmt && *fmt) {
        fputc(' ', stdout);
        vfprintf(stdout, fmt, ap);
    }
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

static void reply_err(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("ERR ", stdout);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

static void emit_evt(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("EVT ", stdout);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

void control_emit_ready(emulator_t* emu) {
    emit_evt("ready pc=%04X cycles=%llu version=%s",
             emu->cpu.PC, (unsigned long long)emu->cpu.cycles, EMU_VERSION);
}

void control_emit_stopped(emulator_t* emu, const char* reason) {
    emit_evt("stopped pc=%04X cycles=%llu reason=%s",
             emu->cpu.PC, (unsigned long long)emu->cpu.cycles,
             reason ? reason : "unknown");
}

/* ─── parsing helpers ──────────────────────────────────────────────
 * Accept hex with or without `$`/`0x` prefix, plus plain decimal when
 * unambiguous. The IDE side is well-defined, so we stay strict. */

static bool parse_hex(const char* s, uint32_t* out) {
    if (!s || !*s) return false;
    if (*s == '$') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_u16(const char* s, uint16_t* out) {
    uint32_t v;
    if (!parse_hex(s, &v) || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

static bool parse_u8(const char* s, uint8_t* out) {
    uint32_t v;
    if (!parse_hex(s, &v) || v > 0xFF) return false;
    *out = (uint8_t)v;
    return true;
}

/* ─── command handlers ─────────────────────────────────────────── */

static void cmd_regs(emulator_t* emu) {
    reply_ok("A=%02X X=%02X Y=%02X SP=%02X P=%02X PC=%04X cycles=%llu",
             emu->cpu.A, emu->cpu.X, emu->cpu.Y, emu->cpu.SP, emu->cpu.P,
             emu->cpu.PC, (unsigned long long)emu->cpu.cycles);
}

static void cmd_set(emulator_t* emu, const char* reg, const char* val) {
    if (!reg || !val) { reply_err("set: usage `set <reg> <val>`"); return; }
    uint32_t v;
    if (!parse_hex(val, &v)) { reply_err("set: bad value"); return; }
    /* Case-insensitive: A, X, Y, SP, P, PC. */
    if (strcasecmp(reg, "a")  == 0) emu->cpu.A  = (uint8_t)v;
    else if (strcasecmp(reg, "x")  == 0) emu->cpu.X  = (uint8_t)v;
    else if (strcasecmp(reg, "y")  == 0) emu->cpu.Y  = (uint8_t)v;
    else if (strcasecmp(reg, "sp") == 0) emu->cpu.SP = (uint8_t)v;
    else if (strcasecmp(reg, "p")  == 0) emu->cpu.P  = (uint8_t)v;
    else if (strcasecmp(reg, "pc") == 0) emu->cpu.PC = (uint16_t)v;
    else { reply_err("set: unknown reg `%s`", reg); return; }
    reply_ok("");
}

static void cmd_read(emulator_t* emu, const char* addr_s, const char* len_s) {
    uint16_t addr;
    uint32_t len;
    if (!parse_u16(addr_s, &addr) || !parse_hex(len_s, &len)) {
        reply_err("read: usage `read <addr> <len>`");
        return;
    }
    if (len > 4096) { reply_err("read: len > 4096"); return; }
    /* Build the reply : "OK <hex bytes>". stdio printf per byte is fine
     * at this scale; the IDE caller batches reads anyway. */
    fputs("OK", stdout);
    for (uint32_t i = 0; i < len; i++) {
        fprintf(stdout, " %02X",
                memory_read(&emu->memory, (uint16_t)(addr + i)));
    }
    fputc('\n', stdout);
    fflush(stdout);
}

static void cmd_write(emulator_t* emu, char* rest) {
    /* `write <addr> <b0> <b1> ...` — at least one byte required. */
    char* save;
    char* addr_s = strtok_r(rest, " \t", &save);
    uint16_t addr;
    if (!addr_s || !parse_u16(addr_s, &addr)) {
        reply_err("write: usage `write <addr> <byte>...`");
        return;
    }
    int n = 0;
    char* tok;
    while ((tok = strtok_r(NULL, " \t", &save)) != NULL) {
        uint8_t b;
        if (!parse_u8(tok, &b)) {
            reply_err("write: bad byte at offset %d", n);
            return;
        }
        memory_write(&emu->memory, (uint16_t)(addr + n), b);
        n++;
    }
    if (n == 0) { reply_err("write: no bytes given"); return; }
    reply_ok("count=%d", n);
}

static void cmd_break(emulator_t* emu, const char* addr_s) {
    uint16_t addr;
    if (!parse_u16(addr_s, &addr)) {
        reply_err("break: usage `break <addr>`");
        return;
    }
    int id = debugger_add_breakpoint(&emu->debugger, addr);
    if (id < 0) { reply_err("break: full or rejected"); return; }
    reply_ok("id=%d addr=%04X", id, addr);
}

static void cmd_unbreak(emulator_t* emu, const char* id_s) {
    if (!id_s) { reply_err("unbreak: usage `unbreak <id>`"); return; }
    int id = atoi(id_s);
    if (!debugger_remove_breakpoint(&emu->debugger, id)) {
        reply_err("unbreak: invalid id");
        return;
    }
    reply_ok("");
}

static void cmd_break_list(emulator_t* emu) {
    debugger_t* dbg = &emu->debugger;
    fputs("OK", stdout);
    for (int i = 0; i < dbg->num_breakpoints; i++) {
        fprintf(stdout, " id=%d:addr=%04X", i, dbg->breakpoints[i].addr);
    }
    fputc('\n', stdout);
    fflush(stdout);
}

static void cmd_reset(emulator_t* emu) {
    cpu_reset(&emu->cpu);
    reply_ok("pc=%04X", emu->cpu.PC);
}

/* ─── main REPL loop ──────────────────────────────────────────── */

void control_repl(emulator_t* emu) {
    char line[1024];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        /* Strip trailing newline. */
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0) continue;   /* blank line, ignore */

        /* First token = command. */
        char* save;
        char* cmd = strtok_r(line, " \t", &save);
        if (!cmd) continue;
        char* arg1 = strtok_r(NULL, " \t", &save);
        char* arg2 = strtok_r(NULL, " \t", &save);

        if (strcmp(cmd, "regs") == 0) {
            cmd_regs(emu);
        }
        else if (strcmp(cmd, "set") == 0) {
            cmd_set(emu, arg1, arg2);
        }
        else if (strcmp(cmd, "read") == 0) {
            cmd_read(emu, arg1, arg2);
        }
        else if (strcmp(cmd, "write") == 0) {
            cmd_write(emu, save);
        }
        else if (strcmp(cmd, "break") == 0) {
            cmd_break(emu, arg1);
        }
        else if (strcmp(cmd, "unbreak") == 0) {
            cmd_unbreak(emu, arg1);
        }
        else if (strcmp(cmd, "break-list") == 0) {
            cmd_break_list(emu);
        }
        else if (strcmp(cmd, "reset") == 0) {
            cmd_reset(emu);
        }
        else if (strcmp(cmd, "pause") == 0) {
            /* The REPL is only re-entered when execution is already
             * stopped, so `pause` is informational. */
            reply_ok("pc=%04X cycles=%llu",
                     emu->cpu.PC, (unsigned long long)emu->cpu.cycles);
        }
        else if (strcmp(cmd, "step") == 0) {
            emu->debugger.step_mode = true;
            emu->debugger.active = false;
            reply_ok("");
            return;
        }
        else if (strcmp(cmd, "next") == 0) {
            uint8_t opc = memory_read(&emu->memory, emu->cpu.PC);
            if (opc == 0x20) {
                emu->debugger.temp_breakpoint = (uint16_t)(emu->cpu.PC + 3);
                emu->debugger.has_temp_breakpoint = true;
                emu->debugger.step_mode = false;
            } else {
                emu->debugger.step_mode = true;
            }
            emu->debugger.active = false;
            reply_ok("");
            return;
        }
        else if (strcmp(cmd, "continue") == 0) {
            emu->debugger.step_mode = false;
            emu->debugger.active = false;
            reply_ok("");
            return;
        }
        else if (strcmp(cmd, "quit") == 0) {
            reply_ok("");
            emu->running = false;
            return;
        }
        else {
            reply_err("unknown command `%s`", cmd);
        }
    }
    /* EOF on stdin — treat as quit. */
    emu->running = false;
}
