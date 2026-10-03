/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cards_list.h
 * @brief THE list of the expansion cards as modules: one line per card
 *        (ADR 0006, docs/specs/CARD_MODULES.md).
 * @author bmarty <bmarty@mailo.com>
 *
 * X(id, tick):
 *   - id   : the card is src/cards/card_<id>.c, which defines `card_<id>`
 *            (card_module_t);
 *   - tick : 1 if it advances on every CPU cycle (function `card_<id>_tick`),
 *            0 otherwise.
 * The list order is the order of the cards' ticks, after those of the core
 * (ADR 0003). The Makefile compiles src/cards/card_*.c: adding a card means
 * writing its file and adding its line here.
 */
#ifndef CARDS_LIST_H
#define CARDS_LIST_H

#define CARD_MODULE_LIST(X) \
    X(dtl2000, 1) \
    X(mageco,  1) \
    X(sp0256,  1) \
    X(mea8000, 1) \
    X(ula_ng,  0)

/* Index of each card: emu->card_on[CARD_IDX_<id>] tells whether it is present. */
enum {
#define CARD_LIST_ENUM(id, tick) CARD_IDX_##id,
    CARD_MODULE_LIST(CARD_LIST_ENUM)
#undef CARD_LIST_ENUM
    CARD_MODULE_COUNT
};

#endif /* CARDS_LIST_H */
