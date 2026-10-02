/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file control.c
 * @brief IPC control mode for OricForge IDE integration (sprint 35a)
 *
 * Implements the --control protocol described in include/control.h.
 * Reuses debugger.c primitives where possible.
 *
 * Sprint 92 (Epic 1): command handlers write to an abstract control_sink_t
 * instead of stdout directly, and the dispatch logic is factored into
 * control_dispatch() so the same handlers can back both the stdin/stdout IPC
 * protocol and a future HTTP API. The stdout path is byte-for-byte unchanged.
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

/* ─── dispatch ─────────────────────────────────────────────────────
 * Parse one command line (mutated in place by strtok_r) and execute it
 * into @p s. Resume/quit commands set the debugger flags and are signalled
 * to the caller through the return value; synchronous commands emit their
 * reply inline and return CONTROL_CONTINUE. */
control_result_t control_dispatch(emulator_t* emu, control_sink_t* s,
                                  char* line) {
    /* Strip trailing newline(s). */
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (n == 0) return CONTROL_CONTINUE;   /* blank line, ignore */

    /* First token = command. */
    char* save;
    char* cmd = strtok_r(line, " \t", &save);
    if (!cmd) return CONTROL_CONTINUE;

    /* `keys` consumes the raw remainder (spaces preserved), so handle it here
     * before the argument tokenizer chops the rest of the line into words. */
    if (strcmp(cmd, "keys") == 0) {
        ctl_cmd_keys(emu, s, save ? save : "");
        return CONTROL_CONTINUE;
    }

    /* `trace` needs its subcommand + raw remainder (a spec or filename), so it
     * is handled before the argument tokenizer chops the rest into words. */
    if (strcmp(cmd, "trace") == 0) {
        char* sub = strtok_r(NULL, " \t", &save);
        ctl_cmd_trace(emu, s, sub, save ? save : "");
        return CONTROL_CONTINUE;
    }

    char* arg1 = strtok_r(NULL, " \t", &save);
    char* arg2 = strtok_r(NULL, " \t", &save);

    if (strcmp(cmd, "hello") == 0) {
        ctl_cmd_hello(s, arg1, arg2);
    }
    else if (strcmp(cmd, "peek") == 0) {
        ctl_cmd_peek(emu, s, arg1);
    }
    else if (strcmp(cmd, "regs") == 0) {
        ctl_cmd_regs(emu, s);
    }
    else if (strcmp(cmd, "set") == 0) {
        ctl_cmd_set(emu, s, arg1, arg2, save);
    }
    else if (strcmp(cmd, "read") == 0) {
        ctl_cmd_read(emu, s, arg1, arg2, save);
    }
    else if (strcmp(cmd, "bread") == 0) {
        ctl_cmd_bread(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "write") == 0) {
        ctl_cmd_write(emu, s, arg1, arg2, save);
    }
    else if (strcmp(cmd, "break") == 0) {
        ctl_cmd_break(emu, s, arg1,
                  (arg2 && strcasecmp(arg2, "if") == 0) ? save : NULL);
    }
    else if (strcmp(cmd, "unbreak") == 0) {
        ctl_cmd_unbreak(emu, s, arg1);
    }
    else if (strcmp(cmd, "break-list") == 0) {
        ctl_cmd_break_list(emu, s);
    }
    else if (strcmp(cmd, "watch") == 0) {
        ctl_cmd_watch(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "hunt") == 0) {
        ctl_cmd_hunt(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "save-mem") == 0) {
        char* a3 = strtok_r(NULL, " \t", &save);
        ctl_cmd_save_mem(emu, s, arg1, arg2, a3);
    }
    else if (strcmp(cmd, "load-mem") == 0) {
        ctl_cmd_load_mem(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "state-save") == 0) {
        ctl_cmd_state_save(emu, s, arg1);
    }
    else if (strcmp(cmd, "state-load") == 0) {
        ctl_cmd_state_load(emu, s, arg1);
    }
    else if (strcmp(cmd, "unwatch") == 0) {
        ctl_cmd_unwatch(emu, s, arg1);
    }
    else if (strcmp(cmd, "watch-list") == 0) {
        ctl_cmd_watch_list(emu, s);
    }
    else if (strcmp(cmd, "watch-region") == 0) {
        ctl_cmd_watch_region(emu, s, arg1, arg2, save);
    }
    else if (strcmp(cmd, "watch-region-clear") == 0) {
        ctl_cmd_watch_region_clear(emu, s);
    }
    else if (strcmp(cmd, "watch-region-list") == 0) {
        ctl_cmd_watch_region_list(emu, s);
    }
    else if (strcmp(cmd, "raster") == 0) {
        ctl_cmd_raster(emu, s, arg1);
    }
    else if (strcmp(cmd, "unraster") == 0) {
        ctl_cmd_unraster(emu, s, arg1);
    }
    else if (strcmp(cmd, "load-tap") == 0) {
        ctl_cmd_load_tap(emu, s, arg1);
    }
    else if (strcmp(cmd, "load-disk") == 0) {
        ctl_cmd_load_disk(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "eject-disk") == 0) {
        ctl_cmd_eject_disk(emu, s, arg1);
    }
    else if (strcmp(cmd, "eject-tape") == 0) {
        ctl_cmd_eject_tape(emu, s);
    }
    else if (strcmp(cmd, "loci-button") == 0) {
        ctl_cmd_loci_button(emu, s, arg1);
    }
    else if (strcmp(cmd, "load-rom") == 0) {
        ctl_cmd_load_rom(emu, s, arg1);
    }
    else if (strcmp(cmd, "load-sym") == 0) {
        ctl_cmd_load_sym(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "sym-group") == 0) {
        ctl_cmd_sym_group(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "stuck-bits") == 0) {
        ctl_cmd_stuck_bits(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "disasm") == 0) {
        ctl_cmd_disasm(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "reset") == 0) {
        ctl_cmd_reset(emu, s);
    }
    else if (strcmp(cmd, "pause") == 0) {
        /* The REPL is only re-entered when execution is already
         * stopped, so `pause` is informational. */
        sink_ok(s, "pc=%04X cycles=%llu",
                emu->cpu.PC, (unsigned long long)emu->cpu.cycles);
    }
    else if (strcmp(cmd, "step") == 0) {
        emu->debugger.step_mode = true;
        emu->debugger.active = false;
        sink_ok(s, "");
        return CONTROL_RESUME;
    }
    else if (strcmp(cmd, "next") == 0) {
        uint8_t opc = memory_peek(&emu->memory, emu->cpu.PC);
        if (opc == 0x20) {
            emu->debugger.temp_breakpoint = (uint16_t)(emu->cpu.PC + 3);
            emu->debugger.has_temp_breakpoint = true;
            emu->debugger.step_mode = false;
        } else {
            emu->debugger.step_mode = true;
        }
        emu->debugger.active = false;
        sink_ok(s, "");
        return CONTROL_RESUME;
    }
    else if (strcmp(cmd, "step-out") == 0) {
        /* Sprint 35a freeze-time addition : peek the return address
         * from the current stack frame (push order : hi first then lo,
         * so JSR stores PC-1 with hi at $0100+SP+2 and lo at SP+1).
         * RTS adds +1 to land on the instruction after JSR. */
        uint16_t sp = (uint16_t)(0x0100 + emu->cpu.SP);
        uint8_t lo = memory_peek(&emu->memory, (uint16_t)(sp + 1));
        uint8_t hi = memory_peek(&emu->memory, (uint16_t)(sp + 2));
        uint16_t ret = (uint16_t)(((uint16_t)hi << 8) | lo) + 1;
        emu->debugger.temp_breakpoint = ret;
        emu->debugger.has_temp_breakpoint = true;
        emu->debugger.step_mode = false;
        emu->debugger.active = false;
        sink_ok(s, "ret=%04X", ret);
        return CONTROL_RESUME;
    }
    else if (strcmp(cmd, "continue") == 0) {
        emu->debugger.step_mode = false;
        emu->debugger.active = false;
        sink_ok(s, "");
        return CONTROL_RESUME;
    }
    else if (strcmp(cmd, "quit") == 0) {
        sink_ok(s, "");
        return CONTROL_QUIT;
    }
    else {
        sink_err(s, "unknown command `%s`", cmd);
    }
    return CONTROL_CONTINUE;
}

/* ─── main REPL loop ──────────────────────────────────────────── */

void control_repl(emulator_t* emu) {
    control_sink_t s;
    control_sink_init_stream(&s, stdout);
    char line[1024];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        control_result_t r = control_dispatch(emu, &s, line);
        if (r == CONTROL_RESUME) return;
        if (r == CONTROL_QUIT) { emu->running = false; return; }
    }
    /* EOF on stdin — treat as quit. */
    emu->running = false;
}
