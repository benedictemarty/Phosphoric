/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cli_parse.c
 * @brief CLI argument-parsing helpers — moved verbatim from main.c (Epic 7/US3).
 * @author bmarty <bmarty@mailo.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/cli_parse.h"
#include "utils/logging.h"

/* Parse a "CYCLES:FILE" CLI argument, shared verbatim by --dump-ram-at and
 * --screenshot-at. On success stores the cycle count and a pointer past the
 * colon (into `arg`) and returns true. On a missing colon it logs the exact
 * same error those sites used (via optname) and returns false — the caller
 * then cleans up and exits 1. Behaviour is identical to the two inlined copies
 * it replaces (covered by tests/integration/test_cli_parsing.sh). */
bool cli_split_cycles_file(const char* arg, const char* optname,
                                  int64_t* out_cycles, const char** out_file) {
    const char* colon = strchr(arg, ':');
    if (!colon) {
        log_error("Invalid --%s format. Use CYCLES:FILE", optname);
        return false;
    }
    *out_cycles = atoll(arg);
    *out_file = colon + 1;
    return true;
}

/* Parse the "ADDR:VAL:FILE" argument shared by the state-triggered captures
 * (--screenshot-when / --dump-ram-when / --screenshot-text-when). ADDR is a
 * 16-bit hexadecimal address (e.g. 9C55), VAL a hexadecimal byte — consistent
 * with ADDR — so "AB", "07" and "0x07" all parse (strtol base 16 accepts the
 * optional 0x prefix), FILE is the rest after the second colon. On success
 * stores addr/val/file and returns true; on a malformed argument logs the
 * canonical "Invalid --NAME format. Use ADDR:VAL:FILE" error and returns false
 * (caller cleans up and exits 1). Covered by test_cli_parsing.sh. */
bool cli_split_addr_val_file(const char* arg, const char* optname,
                                    int32_t* out_addr, uint8_t* out_val,
                                    const char** out_file) {
    const char* c1 = strchr(arg, ':');
    if (!c1) {
        log_error("Invalid --%s format. Use ADDR:VAL:FILE", optname);
        return false;
    }
    const char* c2 = strchr(c1 + 1, ':');
    if (!c2 || c2 == c1 + 1 || *(c2 + 1) == '\0') {
        log_error("Invalid --%s format. Use ADDR:VAL:FILE", optname);
        return false;
    }
    *out_addr = (int32_t)(strtol(arg, NULL, 16) & 0xFFFF);
    *out_val  = (uint8_t)(strtol(c1 + 1, NULL, 16) & 0xFF);
    *out_file = c2 + 1;
    return true;
}

/* Parse an "ADDR=VAL" pair (both hexadecimal) shared by --poke-at / --poke-when.
 * On success stores the 16-bit addr and the byte value and returns true; on a
 * missing/empty '=' returns false (caller logs the canonical format error). */
bool cli_split_addr_eq_val(const char* s, uint16_t* out_addr, uint8_t* out_val) {
    const char* eq = strchr(s, '=');
    if (!eq || eq == s || *(eq + 1) == '\0')
        return false;
    *out_addr = (uint16_t)(strtol(s, NULL, 16) & 0xFFFF);
    *out_val  = (uint8_t)(strtol(eq + 1, NULL, 16) & 0xFF);
    return true;
}

/* Append a --poke-at CYCLES:ADDR=VAL entry to emu->pokes[]. Fires once when
 * total_executed >= CYCLES. Returns false (logging the format error) on a
 * malformed argument or when the table is full. */
bool cli_add_poke_at(emulator_t* emu, const char* arg) {
    const char* colon = strchr(arg, ':');
    uint16_t addr; uint8_t val;
    if (!colon || !cli_split_addr_eq_val(colon + 1, &addr, &val)) {
        log_error("Invalid --poke-at format. Use CYCLES:ADDR=VAL");
        return false;
    }
    if (emu->poke_count >= POKE_MAX) {
        log_error("--poke-at: too many pokes (max %d)", POKE_MAX);
        return false;
    }
    struct poke_action* p = &emu->pokes[emu->poke_count++];
    p->at_cycles = atoll(arg);
    p->when_addr = -1; p->when_val = 0;
    p->target = addr; p->value = val; p->done = false;
    return true;
}

/* Append a --poke-when ADDR:VAL:ADDR=VAL entry to emu->pokes[]. Fires once on
 * the rising edge RAM[ADDR]==VAL. Returns false (logging the format error) on a
 * malformed argument or when the table is full. */
bool cli_add_poke_when(emulator_t* emu, const char* arg) {
    const char* c1 = strchr(arg, ':');
    const char* c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    uint16_t addr; uint8_t val;
    if (!c1 || !c2 || !cli_split_addr_eq_val(c2 + 1, &addr, &val)) {
        log_error("Invalid --poke-when format. Use ADDR:VAL:ADDR=VAL");
        return false;
    }
    if (emu->poke_count >= POKE_MAX) {
        log_error("--poke-when: too many pokes (max %d)", POKE_MAX);
        return false;
    }
    struct poke_action* p = &emu->pokes[emu->poke_count++];
    p->at_cycles = -1;
    p->when_addr = (int32_t)(strtol(arg, NULL, 16) & 0xFFFF);
    p->when_val  = (uint8_t)(strtol(c1 + 1, NULL, 16) & 0xFF);
    p->target = addr; p->value = val; p->done = false;
    return true;
}

/* Open an output file for a CLI option, logging the exact "Cannot open --NAME
 * file: PATH" error those sites used on failure. Returns the stream, or NULL —
 * the caller then cleans up and exits 1. Shared verbatim by --trace-irq /
 * --psg-trace / --audio-wav (identical open-or-fail pattern; only the mode and
 * the post-open headers differ). Covered by test_cli_parsing.sh (the fatal
 * open-failure cases for the three options). */
FILE* cli_open_out(const char* file, const char* mode, const char* optname) {
    FILE* fp = fopen(file, mode);
    if (!fp)
        log_error("Cannot open --%s file: %s", optname, file);
    return fp;
}

/* Parse a 16-bit hexadecimal address argument (--acia-addr / --dtl2000-addr /
 * --mageco-addr / --break). Same lenient strtol semantics as the four inlined
 * copies it replaces: invalid input yields 0 (no error) — behaviour preserved
 * verbatim (see test_cli_parsing.sh, --acia-addr invalid hex = non-fatal). */
uint16_t parse_hex16(const char* s) {
    return (uint16_t)strtol(s, NULL, 16);
}
