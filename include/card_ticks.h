/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_ticks.h
 * @brief Tick par cycle des cartes en modules, généré depuis cards_list.h.
 * @author bmarty <bmarty@mailo.com>
 *
 * Inclus par src/io/io_bus.c seulement. Appelé à CHAQUE cycle : le test de
 * présence lit un drapeau à position fixe (emu->card_on[CARD_IDX_<id>]) et
 * l'appel est direct. C'est le même code qu'une carte câblée dans io_bus.c ;
 * une table de pointeurs parcourue à l'exécution coûtait 2 % par trame avec
 * une seule carte présente (mesuré en G1).
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
