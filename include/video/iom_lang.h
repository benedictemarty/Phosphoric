/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file iom_lang.h
 * @brief F1 menu language: French (source text) or English
 * @author bmarty <bmarty@mailo.com>
 *
 * The texts of the menu, of its messages and of the card pages are written in
 * French; iom_tr() returns their English translation when the language is EN
 * (table in iom_lang.c, lookup on the whole string). Fixed texts go through
 * iom_puts/wrap, which translate them; a formatted text translates its format
 * (and its text arguments) before snprintf. A text missing from the table stays
 * in French.
 */
#ifndef IOM_LANG_H
#define IOM_LANG_H

#include <stdbool.h>

typedef enum { IOM_LANG_FR = 0, IOM_LANG_EN } iom_lang_t;

/* FR at launch; phosphoric.cfg (langue=en) or the menu button change it. */
void        iom_lang_set(iom_lang_t lang);
iom_lang_t  iom_lang(void);
/* « fr » / « en » (phosphoric.cfg: langue=). */
const char* iom_lang_code(iom_lang_t lang);
bool        iom_lang_parse(const char* code, iom_lang_t* out);
/* Translation of @p fr in the current language (@p fr itself in FR or when
 * missing from the table). */
const char* iom_tr(const char* fr);
/* Number of table entries, and entry @p i (tests). */
int         iom_lang_entries(void);
void        iom_lang_entry(int i, const char** fr, const char** en);

#endif /* IOM_LANG_H */
