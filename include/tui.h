/**
 * @file tui.h
 * @brief Optional ncurses TUI debugger
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 *
 * Compiled only when TUI=1 is passed to make.
 * Provides a multi-pane view (registers, disassembly, memory,
 * breakpoints/watchpoints) with single-key commands and a
 * command-line mode via ':'.
 */

#ifndef TUI_H
#define TUI_H

#include <stdbool.h>

typedef struct emulator_s emulator_t;

#ifdef HAS_TUI
bool tui_init(void);
void tui_cleanup(void);
void tui_repl(emulator_t* emu);
#else
static inline bool tui_init(void) { return false; }
static inline void tui_cleanup(void) {}
static inline void tui_repl(emulator_t* emu) { (void)emu; }
#endif

#endif /* TUI_H */
