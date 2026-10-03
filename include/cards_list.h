/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cards_list.h
 * @brief LA liste des cartes d'extension en modules : une ligne par carte
 *        (ADR 0006, docs/specs/CARD_MODULES.md).
 * @author bmarty <bmarty@mailo.com>
 *
 * X(id, tick) :
 *   - id   : la carte est src/cards/card_<id>.c, qui définit `card_<id>`
 *            (card_module_t) ;
 *   - tick : 1 si elle avance à chaque cycle CPU (fonction `card_<id>_tick`),
 *            0 sinon.
 * L'ordre de la liste est celui des ticks des cartes, après ceux du cœur
 * (ADR 0003). Le Makefile compile src/cards/card_*.c : ajouter une carte, c'est
 * écrire son fichier et ajouter sa ligne ici.
 */
#ifndef CARDS_LIST_H
#define CARDS_LIST_H

#define CARD_MODULE_LIST(X) \
    X(dtl2000, 1) \
    X(mageco,  1) \
    X(sp0256,  1) \
    X(mea8000, 1) \
    X(ula_ng,  0)

/* Index de chaque carte : emu->card_on[CARD_IDX_<id>] dit si elle est présente. */
enum {
#define CARD_LIST_ENUM(id, tick) CARD_IDX_##id,
    CARD_MODULE_LIST(CARD_LIST_ENUM)
#undef CARD_LIST_ENUM
    CARD_MODULE_COUNT
};

#endif /* CARDS_LIST_H */
