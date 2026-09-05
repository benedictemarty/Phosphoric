/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file rominfo.c
 * @brief ROM analysis tools — vector detection, subroutine map, string search
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.0.0-alpha
 */

#include "utils/rominfo.h"
#include "cpu/cpu_internal.h"
#include "utils/logging.h"

#include <string.h>
#include <ctype.h>

void rominfo_init(rom_analysis_t* analysis) {
    memset(analysis, 0, sizeof(*analysis));
}

/**
 * @brief Read a 16-bit little-endian value from ROM data
 */
static uint16_t read16(const uint8_t* data, size_t offset) {
    return (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
}

/**
 * @brief Extract hardware vectors from the last 6 bytes of ROM
 */
static void extract_vectors(rom_analysis_t* analysis, const uint8_t* rom, size_t rom_size) {
    if (rom_size < 6) return;

    /* Vectors are at the very end of the 64K address space:
     * $FFFA = NMI, $FFFC = RESET, $FFFE = IRQ
     * In the ROM array: offset = address - ROM_BASE_ADDR */
    size_t nmi_off   = rom_size - 6;  /* $FFFA - $C000 = $3FFA */
    size_t reset_off = rom_size - 4;  /* $FFFC - $C000 = $3FFC */
    size_t irq_off   = rom_size - 2;  /* $FFFE - $C000 = $3FFE */

    analysis->vectors.nmi   = read16(rom, nmi_off);
    analysis->vectors.reset = read16(rom, reset_off);
    analysis->vectors.irq   = read16(rom, irq_off);
}

/**
 * @brief Check if a byte is a printable ASCII character for string detection
 */
static bool is_printable(uint8_t c) {
    /* Accept printable ASCII + common control chars */
    return (c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\r' || c == '\t';
}

/**
 * @brief Scan ROM for JSR ($20) and JMP ($4C) targets
 */
static void scan_targets(rom_analysis_t* analysis, const uint8_t* rom, size_t rom_size) {
    analysis->target_count = 0;

    for (size_t i = 0; i + 2 < rom_size; ) {
        uint8_t opcode = rom[i];
        const opcode_info_t* info = &opcode_table[opcode];

        if ((opcode == 0x20 || opcode == 0x4C) && i + 2 < rom_size) {
            /* JSR abs or JMP abs: 3-byte instruction */
            uint16_t target = read16(rom, i + 1);

            /* Find or create target entry */
            int found = -1;
            for (int j = 0; j < analysis->target_count; j++) {
                if (analysis->targets[j].address == target) {
                    found = j;
                    break;
                }
            }

            if (found >= 0) {
                analysis->targets[found].ref_count++;
                if (opcode == 0x20) analysis->targets[found].is_jsr = true;
                if (opcode == 0x4C) analysis->targets[found].is_jmp = true;
            } else if (analysis->target_count < ROMINFO_MAX_TARGETS) {
                rom_target_t* t = &analysis->targets[analysis->target_count++];
                t->address = target;
                t->ref_count = 1;
                t->is_jsr = (opcode == 0x20);
                t->is_jmp = (opcode == 0x4C);
            }
        }

        /* Advance by instruction size (minimum 1 to avoid infinite loop) */
        size_t step = info->size;
        if (step < 1) step = 1;
        i += step;
    }

    /* Sort targets by address (insertion sort, small N) */
    for (int i = 1; i < analysis->target_count; i++) {
        rom_target_t tmp = analysis->targets[i];
        int j = i - 1;
        while (j >= 0 && analysis->targets[j].address > tmp.address) {
            analysis->targets[j + 1] = analysis->targets[j];
            j--;
        }
        analysis->targets[j + 1] = tmp;
    }
}

/**
 * @brief Scan ROM for ASCII strings
 */
static void scan_strings(rom_analysis_t* analysis, const uint8_t* rom, size_t rom_size) {
    analysis->string_count = 0;
    size_t start = 0;
    bool in_string = false;

    for (size_t i = 0; i < rom_size; i++) {
        /* Check bit 7 clear and printable */
        bool printable = is_printable(rom[i]);

        if (printable && !in_string) {
            start = i;
            in_string = true;
        } else if (!printable && in_string) {
            size_t len = i - start;
            if (len >= ROMINFO_MIN_STRING_LEN &&
                analysis->string_count < ROMINFO_MAX_STRINGS) {
                rom_string_t* s = &analysis->strings[analysis->string_count++];
                s->address = (uint16_t)(ROM_BASE_ADDR + start);
                s->length = (uint16_t)len;
            }
            in_string = false;
        }
    }

    /* Handle string at end of ROM */
    if (in_string) {
        size_t len = rom_size - start;
        if (len >= ROMINFO_MIN_STRING_LEN &&
            analysis->string_count < ROMINFO_MAX_STRINGS) {
            rom_string_t* s = &analysis->strings[analysis->string_count++];
            s->address = (uint16_t)(ROM_BASE_ADDR + start);
            s->length = (uint16_t)len;
        }
    }
}

/**
 * @brief Compute ROM region usage statistics
 */
static void compute_usage(rom_analysis_t* analysis, const uint8_t* rom, size_t rom_size) {
    analysis->usage.total_bytes = (uint16_t)rom_size;
    analysis->usage.zero_bytes = 0;
    analysis->usage.ff_bytes = 0;
    analysis->usage.code_bytes = 0;
    analysis->usage.data_bytes = 0;

    /* Linear scan to classify bytes */
    for (size_t i = 0; i < rom_size; ) {
        uint8_t opcode = rom[i];

        if (opcode == 0x00) {
            analysis->usage.zero_bytes++;
            i++;
            continue;
        }
        if (opcode == 0xFF) {
            analysis->usage.ff_bytes++;
            i++;
            continue;
        }

        const opcode_info_t* info = &opcode_table[opcode];

        /* Check if this looks like a valid instruction */
        if (strcmp(info->name, "???") != 0 && info->size > 0 && i + info->size <= rom_size) {
            analysis->usage.code_bytes += info->size;
            i += info->size;
        } else {
            analysis->usage.data_bytes++;
            i++;
        }
    }
}

bool rominfo_analyze(rom_analysis_t* analysis, const uint8_t* rom, size_t rom_size) {
    rominfo_init(analysis);

    if (!rom || rom_size == 0) {
        return false;
    }

    extract_vectors(analysis, rom, rom_size);
    scan_targets(analysis, rom, rom_size);
    scan_strings(analysis, rom, rom_size);
    compute_usage(analysis, rom, rom_size);

    analysis->valid = true;
    return true;
}

void rominfo_report(const rom_analysis_t* analysis, const uint8_t* rom,
                    size_t rom_size, FILE* fp) {
    if (!fp || !analysis->valid) return;

    fprintf(fp, "═══════════════════════════════════════════════════════\n");
    fprintf(fp, "  ROM Analysis Report\n");
    fprintf(fp, "═══════════════════════════════════════════════════════\n\n");

    /* Vectors */
    fprintf(fp, "── Hardware Vectors ──\n");
    fprintf(fp, "  RESET: $%04X\n", analysis->vectors.reset);
    fprintf(fp, "  IRQ:   $%04X\n", analysis->vectors.irq);
    fprintf(fp, "  NMI:   $%04X\n\n", analysis->vectors.nmi);

    /* Usage statistics */
    fprintf(fp, "── ROM Usage (%u bytes) ──\n", analysis->usage.total_bytes);
    fprintf(fp, "  Code bytes:  %u (%.1f%%)\n",
            analysis->usage.code_bytes,
            100.0 * analysis->usage.code_bytes / analysis->usage.total_bytes);
    fprintf(fp, "  Data bytes:  %u (%.1f%%)\n",
            analysis->usage.data_bytes,
            100.0 * analysis->usage.data_bytes / analysis->usage.total_bytes);
    fprintf(fp, "  Zero ($00):  %u\n", analysis->usage.zero_bytes);
    fprintf(fp, "  Fill ($FF):  %u\n\n", analysis->usage.ff_bytes);

    /* JSR/JMP targets */
    fprintf(fp, "── Subroutine/Jump Targets (%d found) ──\n", analysis->target_count);
    fprintf(fp, "  %-8s  %-6s  %-5s  %-5s\n", "Address", "Refs", "JSR", "JMP");
    for (int i = 0; i < analysis->target_count; i++) {
        const rom_target_t* t = &analysis->targets[i];
        fprintf(fp, "  $%04X     %-6u  %-5s  %-5s\n",
                t->address, t->ref_count,
                t->is_jsr ? "yes" : "-",
                t->is_jmp ? "yes" : "-");
    }
    fprintf(fp, "\n");

    /* ASCII strings */
    fprintf(fp, "── ASCII Strings (%d found, min %d chars) ──\n",
            analysis->string_count, ROMINFO_MIN_STRING_LEN);
    for (int i = 0; i < analysis->string_count; i++) {
        const rom_string_t* s = &analysis->strings[i];
        uint16_t rom_offset = (uint16_t)(s->address - ROM_BASE_ADDR);

        fprintf(fp, "  $%04X [%3u]: \"", s->address, s->length);
        /* Print the string, replacing non-printable with '.' */
        for (uint16_t j = 0; j < s->length && (rom_offset + j) < rom_size; j++) {
            uint8_t c = rom[rom_offset + j];
            if (c >= 0x20 && c <= 0x7E) {
                fputc(c, fp);
            } else {
                fputc('.', fp);
            }
        }
        fprintf(fp, "\"\n");
    }
    fprintf(fp, "\n");

    fprintf(fp, "═══════════════════════════════════════════════════════\n");
}

bool rominfo_report_to_file(const rom_analysis_t* analysis, const uint8_t* rom,
                            size_t rom_size, const char* filename) {
    FILE* fp = fopen(filename, "w");
    if (!fp) {
        log_error("Cannot open ROM info output: %s", filename);
        return false;
    }
    rominfo_report(analysis, rom, rom_size, fp);
    fclose(fp);
    log_info("ROM analysis report written to %s", filename);
    return true;
}

int rominfo_find_pattern(const uint8_t* rom, size_t rom_size,
                         const uint8_t* pattern, size_t pattern_len,
                         uint16_t* results, int max_results) {
    if (!rom || !pattern || pattern_len == 0 || pattern_len > rom_size) {
        return 0;
    }

    int count = 0;
    for (size_t i = 0; i + pattern_len <= rom_size && count < max_results; i++) {
        if (memcmp(&rom[i], pattern, pattern_len) == 0) {
            results[count++] = (uint16_t)(ROM_BASE_ADDR + i);
        }
    }
    return count;
}
