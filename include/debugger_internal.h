/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file debugger_internal.h
 * @brief Symboles partagés entre les fichiers du débogueur (debugger.c,
 *        debugger_view.c, debugger_mem.c, debugger_asm.c, debugger_repl.c) ;
 *        pas une API publique
 * @author bmarty <bmarty@mailo.com>
 */
#ifndef DEBUGGER_INTERNAL_H
#define DEBUGGER_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "debugger.h"
#include "emulator.h"

bool dbg_parse_addr(const emulator_t* emu, const char* s, uint16_t* out);
void dbg_undo_push(debugger_t* dbg, emulator_t* emu);
bool dbg_undo_pop(debugger_t* dbg, emulator_t* emu);
const char* dbg_skip_ws(const char* s);
const char* dbg_skip_token(const char* s);
bool dbg_parse_condexpr(const emulator_t* emu, const char* text, bp_condexpr_t* out);
const char* dbg_watch_mode_name(watch_mode_t m);
void dbg_show_registers(emulator_t* emu);
uint16_t dbg_show_disassembly(emulator_t* emu, uint16_t addr, int count);
void dbg_show_memory_dump(emulator_t* emu, uint16_t addr, int len, peek_bank_t bank);
void dbg_show_stack(emulator_t* emu);
void dbg_show_via_state(emulator_t* emu);
void dbg_show_psg_state(emulator_t* emu);
void dbg_show_disk_state(emulator_t* emu);
void dbg_show_acia_state(emulator_t* emu);
void dbg_show_tape_state(emulator_t* emu);
void dbg_show_loci_state(emulator_t* emu);
void dbg_show_video_state(emulator_t* emu);
void dbg_show_keyboard_state(emulator_t* emu);
void dbg_show_joystick_state(emulator_t* emu);
void dbg_show_printer_state(emulator_t* emu);
void dbg_show_help(void);
uint8_t dbg_peek(emulator_t* emu, uint16_t a);
extern bool     dbg_hunt_cand[0x10000];
extern bool     dbg_hunt_active;
extern uint32_t dbg_hunt_count;
int dbg_assemble_one(emulator_t* emu, uint16_t addr, const char* mnem_in, const char* operand_in);

#endif /* DEBUGGER_INTERNAL_H */
