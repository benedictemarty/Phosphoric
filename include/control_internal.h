/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file control_internal.h
 * @brief Functions shared between the files of the --control protocol
 *        (control.c, control_util.c, control_cmd_*.c); not a public API
 * @author bmarty <bmarty@mailo.com>
 */
#ifndef CONTROL_INTERNAL_H
#define CONTROL_INTERNAL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include "control.h"
#include "emulator.h"

void sink_ensure(control_sink_t* s, size_t extra);
void sink_write(control_sink_t* s, const void* data, size_t len);
void sink_vprintf(control_sink_t* s, const char* fmt, va_list ap);
void sink_printf(control_sink_t* s, const char* fmt, ...);
void sink_flush(control_sink_t* s);
void sink_ok(control_sink_t* s, const char* fmt, ...);
void sink_err(control_sink_t* s, const char* fmt, ...);
void emit_evt(const char* fmt, ...);
bool ctl_parse_hex(const char* s, uint32_t* out);
bool ctl_parse_u16(const char* s, uint16_t* out);
bool ctl_parse_u8(const char* s, uint8_t* out);
void ctl_cmd_regs(emulator_t* emu, control_sink_t* s);
void ctl_cmd_set(emulator_t* emu, control_sink_t* s, const char* reg, const char* val, const char* tail);
void ctl_cmd_read(emulator_t* emu, control_sink_t* s, const char* addr_s, const char* len_s, const char* bank_s);
void ctl_cmd_bread(emulator_t* emu, control_sink_t* s, const char* addr_s, const char* len_s);
void ctl_cmd_write(emulator_t* emu, control_sink_t* s, const char* addr_s, const char* first_byte, char* rest_save);
void ctl_cmd_stuck_bits(emulator_t* emu, control_sink_t* s, const char* s0_s, const char* s1_s);
void ctl_cmd_hunt(emulator_t* emu, control_sink_t* s, const char* op, const char* val_s);
void ctl_cmd_save_mem(emulator_t* emu, control_sink_t* s, const char* path, const char* addr_s, const char* len_s);
void ctl_cmd_load_mem(emulator_t* emu, control_sink_t* s, const char* path, const char* addr_s);
void ctl_cmd_reset(emulator_t* emu, control_sink_t* s);
/* nmi — the button under the Oric: NMI taken at the next instruction. */
void ctl_cmd_nmi(emulator_t* emu, control_sink_t* s);
void ctl_cmd_hello(control_sink_t* s, const char* arg1, const char* arg2);
void ctl_cmd_keys(emulator_t* emu, control_sink_t* s, const char* text);
void ctl_cmd_break(emulator_t* emu, control_sink_t* s, const char* addr_s, const char* cond);
void ctl_cmd_unbreak(emulator_t* emu, control_sink_t* s, const char* id_s);
void ctl_cmd_watch(emulator_t* emu, control_sink_t* s, const char* addr_s, const char* mode_s);
void ctl_cmd_watch_region(emulator_t* emu, control_sink_t* s, const char* start_s, const char* end_s, const char* flags_s);
void ctl_cmd_watch_region_clear(emulator_t* emu, control_sink_t* s);
void ctl_cmd_watch_region_list(emulator_t* emu, control_sink_t* s);
void ctl_cmd_unwatch(emulator_t* emu, control_sink_t* s, const char* id_s);
void ctl_cmd_watch_list(emulator_t* emu, control_sink_t* s);
void ctl_cmd_raster(emulator_t* emu, control_sink_t* s, const char* line_s);
void ctl_cmd_unraster(emulator_t* emu, control_sink_t* s, const char* id_s);
void ctl_cmd_disasm(emulator_t* emu, control_sink_t* s, const char* addr_s, const char* n_s);
void ctl_cmd_break_list(emulator_t* emu, control_sink_t* s);
void ctl_cmd_trace(emulator_t* emu, control_sink_t* s, const char* sub, const char* rest);
void ctl_cmd_peek(emulator_t* emu, control_sink_t* s, const char* sub);
bool load_file_into(const char* path, uint8_t** out_buf, size_t* out_len, size_t max_len);
void ctl_cmd_load_tap(emulator_t* emu, control_sink_t* s, const char* path);
int control_drive_index(const char* s);
bool control_writeback_drive(emulator_t* emu, int drv);
void ctl_cmd_load_disk(emulator_t* emu, control_sink_t* s, const char* drive_s, const char* path);
void ctl_cmd_eject_disk(emulator_t* emu, control_sink_t* s, const char* drive_s);
void ctl_cmd_loci_button(emulator_t* emu, control_sink_t* s, const char* mode);
void ctl_cmd_eject_tape(emulator_t* emu, control_sink_t* s);
void ctl_cmd_load_rom(emulator_t* emu, control_sink_t* s, const char* path);
void ctl_cmd_load_sym(emulator_t* emu, control_sink_t* s, const char* path, const char* group_s);
void ctl_cmd_sym_group(emulator_t* emu, control_sink_t* s, const char* group_s, const char* onoff_s);
void ctl_cmd_state_save(emulator_t* emu, control_sink_t* s, const char* path);
void ctl_cmd_state_load(emulator_t* emu, control_sink_t* s, const char* path);

#endif /* CONTROL_INTERNAL_H */
