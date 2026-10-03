/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_modules.c
 * @brief Registre des cartes en modules : table getopt, aide, configuration et
 *        mise en route dérivées de k_card_modules[] (voir card_module.h).
 * @author bmarty <bmarty@mailo.com>
 */
#include "card_module.h"
#include "cards_list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Liste (générée depuis cards_list.h) ─────────────────────────────────── */

#define CARD_LIST_DECL(id, tick) extern const card_module_t card_##id;
CARD_MODULE_LIST(CARD_LIST_DECL)
#define CARD_LIST_PTR(id, tick) &card_##id,
const card_module_t* const k_card_modules[] = { CARD_MODULE_LIST(CARD_LIST_PTR) NULL };
const int k_card_module_count = CARD_MODULE_COUNT;

/* ── Configuration ─────────────────────────────────────────────────────── */

/* Une configuration par module, allouée une fois et gardée (statique : jamais
 * signalée comme fuite) ; chaque appel la remet aux valeurs par défaut. */
static void* s_cfgs[64];

void** card_modules_cfg_new(void) {
    for (int m = 0; m < k_card_module_count; m++) {
        const card_module_t* mod = k_card_modules[m];
        size_t sz = mod->cfg_size ? mod->cfg_size : 1;
        if (!s_cfgs[m] && !(s_cfgs[m] = malloc(sz))) return NULL;
        memset(s_cfgs[m], 0, sz);
        if (mod->cfg_defaults) mod->cfg_defaults(s_cfgs[m]);
    }
    return s_cfgs;
}

bool card_modules_set_option(void** cfgs, int code, const char* arg) {
    int m = (code - CARD_OPT_BASE) / 64, j = (code - CARD_OPT_BASE) % 64;
    if (code < CARD_OPT_BASE || m >= k_card_module_count || j >= k_card_modules[m]->nopts)
        return false;
    k_card_modules[m]->opts[j].set(cfgs[m], arg);
    return true;
}

bool card_modules_option_is_flag(const char* opt) {
    if (strncmp(opt, "--", 2) != 0) return false;
    for (int m = 0; m < k_card_module_count; m++)
        for (int j = 0; j < k_card_modules[m]->nopts; j++) {
            const card_opt_t* o = &k_card_modules[m]->opts[j];
            if (o->has_arg == no_argument && strcmp(opt + 2, o->name) == 0) return true;
        }
    return false;
}

/* ── Table getopt ──────────────────────────────────────────────────────── */

/* Les options d'un module s'insèrent juste avant leur ancre. Une ancre peut être
 * l'option d'un autre module : chaque option émise laisse d'abord passer les
 * modules ancrés sur elle (récursion bornée par `placed`). */
static struct option* s_tbl;
static int s_n;
static bool s_placed[64];

static void emit_module(int m);

static void emit_before(const char* name) {
    for (int m = 0; m < k_card_module_count; m++) {
        const char* a = k_card_modules[m]->opts_before;
        if (!s_placed[m] && a && strcmp(a, name) == 0) emit_module(m);
    }
}

static void emit_module(int m) {
    const card_module_t* mod = k_card_modules[m];
    s_placed[m] = true;
    for (int j = 0; j < mod->nopts; j++) {
        emit_before(mod->opts[j].name);
        s_tbl[s_n].name = mod->opts[j].name;
        s_tbl[s_n].has_arg = mod->opts[j].has_arg;
        s_tbl[s_n].flag = NULL;
        s_tbl[s_n].val = CARD_OPT_BASE + m * 64 + j;
        s_n++;
    }
}

const struct option* card_modules_long_options(const struct option* core) {
    if (s_tbl) return s_tbl;
    int ncore = 0, nmod = 0;
    while (core[ncore].name) ncore++;
    for (int m = 0; m < k_card_module_count; m++) nmod += k_card_modules[m]->nopts;
    s_tbl = calloc((size_t)(ncore + nmod + 1), sizeof(struct option));
    if (!s_tbl) return core;
    for (int i = 0; i < ncore; i++) {
        emit_before(core[i].name);
        s_tbl[s_n++] = core[i];
    }
    for (int m = 0; m < k_card_module_count; m++)
        if (!s_placed[m]) emit_module(m);          /* sans ancre (ou ancre absente) */
    return s_tbl;                                   /* terminée par calloc : {0} */
}

/* ── Aide ──────────────────────────────────────────────────────────────── */

static bool s_help_done[64];

static void help_module(int m);

static void help_before(const char* name) {
    for (int m = 0; m < k_card_module_count; m++) {
        const char* a = k_card_modules[m]->help_before;
        if (!s_help_done[m] && k_card_modules[m]->help && a && strcmp(a, name) == 0)
            help_module(m);
    }
}

static void help_module(int m) {
    const card_module_t* mod = k_card_modules[m];
    s_help_done[m] = true;
    if (mod->nopts > 0) help_before(mod->opts[0].name);
    fputs(mod->help, stdout);
}

void card_modules_print_help_before(const char* name) {
    if (name) {
        help_before(name);
        return;
    }
    for (int m = 0; m < k_card_module_count; m++)
        if (!s_help_done[m] && k_card_modules[m]->help) help_module(m);
    memset(s_help_done, 0, sizeof(s_help_done));   /* prête pour une autre aide */
}

/* ── Mise en route ─────────────────────────────────────────────────────── */

int card_modules_setup(emulator_t* emu, void** cfgs, card_stage_t stage) {
    for (int m = 0; m < k_card_module_count; m++) {
        const card_module_t* mod = k_card_modules[m];
        if (mod->stage == stage && mod->setup && mod->setup(emu, cfgs[m]) != 0)
            return 1;
    }
    return 0;
}
