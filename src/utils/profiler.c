/**
 * @file profiler.c
 * @brief CPU performance profiler — execution hotspots and opcode statistics
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.0.0-alpha
 */

#include "utils/profiler.h"
#include "memory/memory.h"
#include "utils/logging.h"

#include <string.h>

void profiler_init(cpu_profiler_t* prof) {
    memset(prof, 0, sizeof(*prof));
}

void profiler_start(cpu_profiler_t* prof) {
    prof->active = true;
}

void profiler_stop(cpu_profiler_t* prof) {
    prof->active = false;
}

void profiler_record_instruction(cpu_profiler_t* prof, const cpu6502_t* cpu) {
    if (!prof->active) return;

    uint16_t pc = cpu->PC;
    uint8_t opcode = memory_read(cpu->memory, pc);

    prof->addr_hits[pc]++;
    prof->opcode_hits[opcode]++;
    prof->total_instructions++;
}

void profiler_record_cycles(cpu_profiler_t* prof, uint16_t pc, int cycles) {
    if (!prof->active) return;

    prof->addr_cycles[pc] += (uint32_t)cycles;
    prof->total_cycles += (uint64_t)cycles;
}

void profiler_reset(cpu_profiler_t* prof) {
    bool was_active = prof->active;
    memset(prof, 0, sizeof(*prof));
    prof->active = was_active;
}

/* Helper: find top N addresses by a given counter array */
typedef struct {
    uint16_t addr;
    uint32_t count;
} addr_entry_t;

static void find_top_n(const uint32_t counts[PROFILER_ADDR_SPACE],
                       addr_entry_t* out, int n) {
    /* Simple selection: scan for top N (good enough for 64K entries) */
    memset(out, 0, (size_t)n * sizeof(addr_entry_t));

    for (int addr = 0; addr < PROFILER_ADDR_SPACE; addr++) {
        uint32_t c = counts[addr];
        if (c == 0) continue;

        /* Find minimum in current top-N */
        int min_idx = 0;
        for (int i = 1; i < n; i++) {
            if (out[i].count < out[min_idx].count) {
                min_idx = i;
            }
        }

        if (c > out[min_idx].count) {
            out[min_idx].addr = (uint16_t)addr;
            out[min_idx].count = c;
        }
    }

    /* Sort descending by count (simple insertion sort for small N) */
    for (int i = 1; i < n; i++) {
        addr_entry_t tmp = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].count < tmp.count) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = tmp;
    }
}

void profiler_report(const cpu_profiler_t* prof, FILE* fp) {
    if (!fp) return;

    fprintf(fp, "═══════════════════════════════════════════════════════\n");
    fprintf(fp, "  CPU Performance Profile\n");
    fprintf(fp, "═══════════════════════════════════════════════════════\n\n");

    fprintf(fp, "Total instructions: %llu\n", (unsigned long long)prof->total_instructions);
    fprintf(fp, "Total cycles:       %llu\n", (unsigned long long)prof->total_cycles);
    if (prof->total_instructions > 0) {
        fprintf(fp, "Avg cycles/instr:   %.2f\n",
                (double)prof->total_cycles / (double)prof->total_instructions);
    }
    fprintf(fp, "\n");

    /* ── Execution by memory region (aggregate — answers ROM vs RAM/game share) ──
     * Regions follow the standard ORIC memory map (see memory/memory.h). */
    {
        static const struct { const char* name; uint32_t lo, hi; } regions[] = {
            { "Zero page",   0x0000, 0x00FF },
            { "Stack",       0x0100, 0x01FF },
            { "System vars", 0x0200, 0x02FF },
            { "I/O",         0x0300, 0x03FF },
            { "User RAM",    0x0400, 0x9FFF },
            { "Screen RAM",  0xA000, 0xBFFF },
            { "BASIC ROM",   0xC000, 0xF7FF },
            { "Kernel ROM",  0xF800, 0xFFFF },
        };
        fprintf(fp, "── Execution by memory region ──\n");
        fprintf(fp, "  %-14s %-10s %-9s %-11s %-8s\n",
                "Region", "Hits", "% Instr", "Cycles", "% Cyc");
        uint64_t ram_h = 0, ram_c = 0, rom_h = 0, rom_c = 0;
        for (size_t r = 0; r < sizeof(regions) / sizeof(regions[0]); r++) {
            uint64_t h = 0, c = 0;
            for (uint32_t a = regions[r].lo; a <= regions[r].hi; a++) {
                h += prof->addr_hits[a];
                c += prof->addr_cycles[a];
            }
            double hp = prof->total_instructions
                ? 100.0 * (double)h / (double)prof->total_instructions : 0.0;
            double cp = prof->total_cycles
                ? 100.0 * (double)c / (double)prof->total_cycles : 0.0;
            fprintf(fp, "  %-14s %-10llu %6.2f%%  %-11llu %6.2f%%\n",
                    regions[r].name, (unsigned long long)h, hp,
                    (unsigned long long)c, cp);
            if (regions[r].lo >= 0xC000) { rom_h += h; rom_c += c; }
            else                         { ram_h += h; ram_c += c; }
        }
        double ramhp = prof->total_instructions ? 100.0 * (double)ram_h / (double)prof->total_instructions : 0.0;
        double romhp = prof->total_instructions ? 100.0 * (double)rom_h / (double)prof->total_instructions : 0.0;
        double ramcp = prof->total_cycles ? 100.0 * (double)ram_c / (double)prof->total_cycles : 0.0;
        double romcp = prof->total_cycles ? 100.0 * (double)rom_c / (double)prof->total_cycles : 0.0;
        fprintf(fp, "  %-14s %-10llu %6.2f%%  %-11llu %6.2f%%\n",
                "RAM total", (unsigned long long)ram_h, ramhp,
                (unsigned long long)ram_c, ramcp);
        fprintf(fp, "  %-14s %-10llu %6.2f%%  %-11llu %6.2f%%\n",
                "ROM total", (unsigned long long)rom_h, romhp,
                (unsigned long long)rom_c, romcp);
        fprintf(fp, "\n");
    }

    /* Top 20 hotspots by execution count */
    #define TOP_N 20
    addr_entry_t top_hits[TOP_N];
    find_top_n(prof->addr_hits, top_hits, TOP_N);

    fprintf(fp, "── Top %d addresses by execution count ──\n", TOP_N);
    fprintf(fp, "  %-8s  %-12s  %-8s\n", "Address", "Hits", "% Total");
    for (int i = 0; i < TOP_N; i++) {
        if (top_hits[i].count == 0) break;
        double pct = (prof->total_instructions > 0)
            ? 100.0 * (double)top_hits[i].count / (double)prof->total_instructions
            : 0.0;
        fprintf(fp, "  $%04X     %-12u  %6.2f%%\n",
                top_hits[i].addr, top_hits[i].count, pct);
    }
    fprintf(fp, "\n");

    /* Top 20 hotspots by cycle usage */
    addr_entry_t top_cycles[TOP_N];
    find_top_n(prof->addr_cycles, top_cycles, TOP_N);

    fprintf(fp, "── Top %d addresses by cycle usage ──\n", TOP_N);
    fprintf(fp, "  %-8s  %-12s  %-8s\n", "Address", "Cycles", "% Total");
    for (int i = 0; i < TOP_N; i++) {
        if (top_cycles[i].count == 0) break;
        double pct = (prof->total_cycles > 0)
            ? 100.0 * (double)top_cycles[i].count / (double)prof->total_cycles
            : 0.0;
        fprintf(fp, "  $%04X     %-12u  %6.2f%%\n",
                top_cycles[i].addr, top_cycles[i].count, pct);
    }
    fprintf(fp, "\n");

    /* Opcode frequency histogram */
    fprintf(fp, "── Opcode frequency ──\n");
    fprintf(fp, "  %-8s  %-12s  %-8s\n", "Opcode", "Count", "% Total");
    for (int i = 0; i < PROFILER_OPCODE_COUNT; i++) {
        if (prof->opcode_hits[i] == 0) continue;
        double pct = (prof->total_instructions > 0)
            ? 100.0 * (double)prof->opcode_hits[i] / (double)prof->total_instructions
            : 0.0;
        fprintf(fp, "  $%02X       %-12u  %6.2f%%\n",
                i, prof->opcode_hits[i], pct);
    }
    fprintf(fp, "\n");
    fprintf(fp, "═══════════════════════════════════════════════════════\n");
}

bool profiler_report_to_file(const cpu_profiler_t* prof, const char* filename) {
    FILE* fp = fopen(filename, "w");
    if (!fp) {
        log_error("Cannot open profile output: %s", filename);
        return false;
    }
    profiler_report(prof, fp);
    fclose(fp);
    log_info("Profile report written to %s", filename);
    return true;
}
