/**
 * @file trace.c
 * @brief CPU trace logging — instruction-level execution trace to file
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.0.0-alpha
 */

#define _POSIX_C_SOURCE 200809L
#include "utils/trace.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "utils/logging.h"
#include "utils/symbols.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

void trace_init(cpu_trace_t* trace) {
    memset(trace, 0, sizeof(*trace));
    trace->fp = NULL;
    trace->active = false;
    trace->count = 0;
    trace->max_count = 0;
    trace->owns_fp = false;
}

bool trace_open(cpu_trace_t* trace, const char* filename) {
    if (trace->active) {
        trace_close(trace);
    }

    FILE* fp;
    if (filename == NULL) {
        fp = stdout;
        trace->owns_fp = false;
    } else {
        fp = fopen(filename, "w");
        if (!fp) {
            log_error("Cannot open trace file: %s", filename);
            return false;
        }
        trace->owns_fp = true;
    }

    trace->fp = fp;
    trace->active = true;
    trace->count = 0;
    log_info("CPU trace logging to %s", filename ? filename : "stdout");
    return true;
}

void trace_attach(cpu_trace_t* trace, FILE* fp) {
    if (trace->active) {
        trace_close(trace);
    }
    trace->fp = fp;
    trace->active = (fp != NULL);
    trace->count = 0;
    trace->owns_fp = false;
}

/* Format one trace line (with trailing newline) into @p out. Appends a
 * "; SYMBOL" annotation when inline symbols are enabled and one is found. */
static void trace_format_line(const cpu_trace_t* trace, const cpu6502_t* cpu,
                              char* out, size_t out_size) {
    uint16_t pc = cpu->PC;
    memory_t* mem = cpu->memory;
    uint8_t b0 = memory_read(mem, pc);
    uint8_t b1 = memory_read(mem, (uint16_t)(pc + 1));
    uint8_t b2 = memory_read(mem, (uint16_t)(pc + 2));

    char disasm[32];
    int size = cpu_disassemble(cpu, pc, disasm, sizeof(disasm));

    char bytes[12];
    if (size == 1)      snprintf(bytes, sizeof(bytes), "%02X      ", b0);
    else if (size == 2) snprintf(bytes, sizeof(bytes), "%02X %02X   ", b0, b1);
    else                snprintf(bytes, sizeof(bytes), "%02X %02X %02X", b0, b1, b2);

    const char* sym = NULL;
    if (trace->with_symbols && trace->symbols)
        sym = symbol_lookup(trace->symbols, pc);

    snprintf(out, out_size,
             "%08llu  %04X  %s  %-20s  A=%02X X=%02X Y=%02X SP=%02X P=%02X%s%s\n",
             (unsigned long long)cpu->cycles, pc, bytes, disasm,
             cpu->A, cpu->X, cpu->Y, cpu->SP, cpu->P,
             sym ? "  ; " : "", sym ? sym : "");
}

/* A stop trigger fired: end recording (keep any FILE* / ring for saving). */
static void trace_trigger_stop(cpu_trace_t* trace) {
    trace->stop_hit = true;
    trace->active = false;
    trace->armed = false;
    if (trace->fp && trace->owns_fp) fflush(trace->fp);
}

void trace_log_instruction(cpu_trace_t* trace, const cpu6502_t* cpu) {
    /* Not recording yet: honour a pending PC start trigger. */
    if (!trace->active) {
        if (trace->armed && trace->start_cond == TRACE_START_PC &&
            cpu->PC == trace->start_pc) {
            trace->active = true;   /* fall through and record this instruction */
        } else {
            return;
        }
    }

    /* Legacy max-count cap (file mode). */
    if (trace->max_count > 0 && trace->count >= trace->max_count) {
        trace_close(trace);
        return;
    }

    if (trace->count == 0) trace->start_cycle = cpu->cycles;

    char line[TRACE_LINE_MAX];
    trace_format_line(trace, cpu, line, sizeof(line));

    if (trace->ring && trace->ring_cap > 0) {
        memcpy(trace->ring + (size_t)trace->ring_head * TRACE_LINE_MAX,
               line, TRACE_LINE_MAX);
        trace->ring_head = (trace->ring_head + 1) % trace->ring_cap;
        if (trace->ring_count < trace->ring_cap) trace->ring_count++;
    } else if (trace->fp) {
        fputs(line, trace->fp);
    }
    trace->count++;

    /* Stop triggers checkable from CPU state (cycle / BRK). */
    if (trace->stop_cond == TRACE_STOP_CYCLE &&
        (cpu->cycles - trace->start_cycle) >= trace->stop_cycle) {
        trace_trigger_stop(trace);
    } else if (trace->stop_cond == TRACE_STOP_BRK &&
               memory_read(cpu->memory, cpu->PC) == 0x00) {
        trace_trigger_stop(trace);
    }
}

void trace_arm(cpu_trace_t* trace, trace_start_t start_cond, uint16_t start_pc,
               trace_stop_t stop_cond, uint16_t stop_addr, uint64_t stop_cycle,
               uint32_t ring_cap, bool with_symbols) {
    /* Clear any prior conditional state but keep an attached FILE*. */
    free(trace->ring);
    trace->ring = NULL;
    trace->ring_cap = trace->ring_head = trace->ring_count = 0;
    trace->count = 0;
    trace->stop_hit = false;
    trace->start_cycle = 0;

    trace->start_cond = start_cond;
    trace->start_pc = start_pc;
    trace->stop_cond = stop_cond;
    trace->stop_addr = stop_addr;
    trace->stop_cycle = stop_cycle;
    trace->with_symbols = with_symbols;

    if (ring_cap > 0) {
        trace->ring = calloc((size_t)ring_cap, TRACE_LINE_MAX);
        if (trace->ring) trace->ring_cap = ring_cap;
    }

    trace->armed = true;
    trace->active = (start_cond == TRACE_START_NOW);
}

void trace_set_symbols(cpu_trace_t* trace, const symbol_table_t* syms) {
    trace->symbols = syms;
}

void trace_note_mem_access(cpu_trace_t* trace, uint16_t addr, int is_write) {
    if (!trace->active) return;
    if (trace->stop_cond == TRACE_STOP_WRITE && is_write && addr == trace->stop_addr)
        trace_trigger_stop(trace);
    else if (trace->stop_cond == TRACE_STOP_READ && !is_write && addr == trace->stop_addr)
        trace_trigger_stop(trace);
}

bool trace_save_ring(cpu_trace_t* trace, const char* filename) {
    if (!trace->ring || trace->ring_count == 0) return false;
    FILE* f = fopen(filename, "w");
    if (!f) { log_error("Cannot open trace file: %s", filename); return false; }
    /* Oldest entry first: when full, oldest is at ring_head. */
    uint32_t start = (trace->ring_count == trace->ring_cap) ? trace->ring_head : 0;
    for (uint32_t i = 0; i < trace->ring_count; i++) {
        uint32_t idx = (start + i) % trace->ring_cap;
        fputs(trace->ring + (size_t)idx * TRACE_LINE_MAX, f);
    }
    fclose(f);
    log_info("CPU trace: %u instructions saved to %s", trace->ring_count, filename);
    return true;
}

uint32_t trace_ring_count(const cpu_trace_t* trace) {
    return trace->ring_count;
}

void trace_reset(cpu_trace_t* trace) {
    free(trace->ring);
    trace->ring = NULL;
    trace->ring_cap = trace->ring_head = trace->ring_count = 0;
    trace->armed = false;
    trace->active = false;
    trace->stop_hit = false;
    trace->stop_cond = TRACE_STOP_NONE;
    trace->start_cond = TRACE_START_NOW;
}

void trace_stop(cpu_trace_t* trace) {
    /* End recording but keep the ring buffer so it can still be saved. */
    trace->active = false;
    trace->armed = false;
    if (trace->fp && trace->owns_fp) fflush(trace->fp);
}

/* ── Memory hook (drives write/read stop triggers) ── */
static cpu_trace_t* g_trace_mem = NULL;

static void trace_mem_cb(uint16_t addr, uint8_t val, mem_access_type_t type) {
    (void)val;
    if (g_trace_mem)
        trace_note_mem_access(g_trace_mem, addr, type == MEM_WRITE ? 1 : 0);
}

void trace_install_mem_hook(cpu_trace_t* trace, struct memory_s* mem) {
    g_trace_mem = trace;
    bool need = trace && (trace->stop_cond == TRACE_STOP_WRITE ||
                          trace->stop_cond == TRACE_STOP_READ);
    memory_set_trace2((memory_t*)mem, need ? trace_mem_cb : NULL);
}

/* ── Spec parser: "now|pc:HEX  stop:cycle:N|stop:brk|stop:write:HEX|
 *                  stop:read:HEX  ring:N  sym" ── */
bool trace_parse_spec(const char* args,
                      trace_start_t* start, uint16_t* start_pc,
                      trace_stop_t* stop, uint16_t* stop_addr, uint64_t* stop_cycle,
                      uint32_t* ring_cap, bool* with_sym) {
    *start = TRACE_START_NOW; *start_pc = 0;
    *stop = TRACE_STOP_NONE;  *stop_addr = 0; *stop_cycle = 0;
    *ring_cap = 0;            *with_sym = false;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", args ? args : "");
    char* save = NULL;
    for (char* t = strtok_r(buf, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
        if      (strcasecmp(t, "now") == 0) *start = TRACE_START_NOW;
        else if (strncasecmp(t, "pc:", 3) == 0) {
            *start = TRACE_START_PC; *start_pc = (uint16_t)strtoul(t + 3, NULL, 16);
        } else if (strncasecmp(t, "stop:cycle:", 11) == 0) {
            *stop = TRACE_STOP_CYCLE; *stop_cycle = strtoull(t + 11, NULL, 0);
        } else if (strcasecmp(t, "stop:brk") == 0) {
            *stop = TRACE_STOP_BRK;
        } else if (strncasecmp(t, "stop:write:", 11) == 0) {
            *stop = TRACE_STOP_WRITE; *stop_addr = (uint16_t)strtoul(t + 11, NULL, 16);
        } else if (strncasecmp(t, "stop:read:", 10) == 0) {
            *stop = TRACE_STOP_READ; *stop_addr = (uint16_t)strtoul(t + 10, NULL, 16);
        } else if (strncasecmp(t, "ring:", 5) == 0) {
            *ring_cap = (uint32_t)strtoul(t + 5, NULL, 0);
        } else if (strcasecmp(t, "sym") == 0) {
            *with_sym = true;
        } else {
            return false;
        }
    }
    return true;
}

void trace_close(cpu_trace_t* trace) {
    if (trace->fp && trace->owns_fp) {
        fclose(trace->fp);
    }
    trace->fp = NULL;
    trace->active = false;
    trace->armed = false;
    if (trace->count > 0) {
        log_info("CPU trace: %llu instructions logged", (unsigned long long)trace->count);
    }
    free(trace->ring);
    trace->ring = NULL;
    trace->ring_cap = trace->ring_head = trace->ring_count = 0;
}

void trace_set_max(cpu_trace_t* trace, uint64_t max) {
    trace->max_count = max;
}
