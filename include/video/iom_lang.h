/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file iom_lang.h
 * @brief Langue du menu F1 : français (texte source) ou anglais
 * @author bmarty <bmarty@mailo.com>
 *
 * Les textes du menu, de ses messages et des fiches de cartes sont écrits en
 * français ; iom_tr() rend leur traduction anglaise quand la langue est EN
 * (table dans iom_lang.c, recherche sur la chaîne entière). Les textes fixes
 * passent par iom_puts/wrap, qui traduisent eux-mêmes ; un texte formaté
 * traduit son format (et ses arguments textuels) avant snprintf. Un texte
 * absent de la table reste en français.
 */
#ifndef IOM_LANG_H
#define IOM_LANG_H

#include <stdbool.h>

typedef enum { IOM_LANG_FR = 0, IOM_LANG_EN } iom_lang_t;

/* FR au lancement ; phosphoric.cfg (langue=en) ou le bouton du menu la changent. */
void        iom_lang_set(iom_lang_t lang);
iom_lang_t  iom_lang(void);
/* « fr » / « en » (phosphoric.cfg : langue=). */
const char* iom_lang_code(iom_lang_t lang);
bool        iom_lang_parse(const char* code, iom_lang_t* out);
/* Traduction de @p fr dans la langue courante (@p fr lui-même en FR ou si
 * absent de la table). */
const char* iom_tr(const char* fr);
/* Nombre d'entrées de la table, et l'entrée @p i (tests). */
int         iom_lang_entries(void);
void        iom_lang_entry(int i, const char** fr, const char** en);

#endif /* IOM_LANG_H */
