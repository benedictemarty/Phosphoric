/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cli_parse.h
 * @brief Small CLI argument-parsing helpers, extracted from main.c (Epic 7/US3).
 * @author bmarty <bmarty@mailo.com>
 *
 * Pure helpers shared by the option-parsing switch in main(); moved out to keep
 * main.c from growing. Behaviour is verbatim — covered by
 * tests/integration/test_cli_parsing.sh.
 */
#ifndef CLI_PARSE_H
#define CLI_PARSE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "emulator.h"   /* emulator_t (used by the --poke-* helpers) */

/* "CYCLES:FILE" (--dump-ram-at / --screenshot-at). */
bool cli_split_cycles_file(const char* arg, const char* optname,
                           int64_t* out_cycles, const char** out_file);
/* "ADDR:VAL:FILE" (--screenshot-when / --dump-ram-when / --screenshot-text-when). */
bool cli_split_addr_val_file(const char* arg, const char* optname,
                             int32_t* out_addr, uint8_t* out_val,
                             const char** out_file);
/* "ADDR=VAL" hex pair (--poke-at / --poke-when). */
bool cli_split_addr_eq_val(const char* s, uint16_t* out_addr, uint8_t* out_val);
/* Append a --poke-at CYCLES:ADDR=VAL / --poke-when ADDR:VAL:ADDR=VAL entry. */
bool cli_add_poke_at(emulator_t* emu, const char* arg);
bool cli_add_poke_when(emulator_t* emu, const char* arg);
/* fopen wrapper logging "Cannot open --NAME file: PATH" (--trace-irq/--psg-trace/--audio-wav). */
FILE* cli_open_out(const char* file, const char* mode, const char* optname);
/* Lenient 16-bit hex parse (--acia-addr / --dtl2000-addr / --mageco-addr / --break). */
uint16_t parse_hex16(const char* s);

#endif /* CLI_PARSE_H */
