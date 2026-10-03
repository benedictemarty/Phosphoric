/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_ticks.h
 * @brief Per-cycle tick of the cards as modules, generated from cards_list.h.
 * @author bmarty <bmarty@mailo.com>
 *
 * Included by src/io/io_bus.c only. Called on EVERY cycle: the presence
 * test reads a flag at a fixed position (emu->card_on[CARD_IDX_<id>]) and
 * the call is direct. It is the same code as a card wired into io_bus.c;
 * a pointer table walked at run time cost 2 % per frame with a single
 * card present (measured in G1).
 */
#ifndef CARD_TICKS_H
#define CARD_TICKS_H

#include "emulator.h"
#include "cards_list.h"

#define CARD_TICK_DECL_0(id)
#define CARD_TICK_DECL_1(id) void card_##id##_tick(emulator_t* emu, int cycles);
#define CARD_TICK_DECL(id, tick) CARD_TICK_DECL_##tick(id)
CARD_MODULE_LIST(CARD_TICK_DECL)

#define CARD_TICK_CALL_0(id)
#define CARD_TICK_CALL_1(id) \
    if (__builtin_expect(emu->card_on[CARD_IDX_##id], 0)) card_##id##_tick(emu, cycles);
#define CARD_TICK_CALL(id, tick) CARD_TICK_CALL_##tick(id)

static inline void card_modules_tick(emulator_t* emu, int cycles) {
    (void)emu; (void)cycles;
    CARD_MODULE_LIST(CARD_TICK_CALL)
}

#endif /* CARD_TICKS_H */
