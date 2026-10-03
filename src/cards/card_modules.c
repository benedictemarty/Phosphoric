/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_modules.c
 * @brief Registry of the cards as modules: getopt table, help, configuration
 *        and setup derived from k_card_modules[] (see card_module.h).
 * @author bmarty <bmarty@mailo.com>
 */
#include "card_module.h"
#include "cards_list.h"
#include "cli/cli_opts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── List (generated from cards_list.h) ──────────────────────────────────── */

#define CARD_LIST_DECL(id, tick) extern const card_module_t card_##id;
CARD_MODULE_LIST(CARD_LIST_DECL)
#define CARD_LIST_PTR(id, tick) &card_##id,
const card_module_t* const k_card_modules[] = { CARD_MODULE_LIST(CARD_LIST_PTR) NULL };
const int k_card_module_count = CARD_MODULE_COUNT;

/* ── Placement par ancres (commun aux options, à l'aide, au menu, au bus) ──
 *
 * Chaque module se place juste avant l'élément que nomme son ancre. Trois passes :
 *   1. en parcourant les éléments du cœur, on place avant chacun les modules
 *      ancrés sur lui ;
 *   2. puis les modules sans ancre, dans l'ordre de la liste ;
 *   3. puis ceux dont l'ancre n'existe pas.
 * Placer un module place d'abord les modules ancrés sur l'un de ses éléments : une
 * ancre peut donc nommer un autre module, quel que soit l'ordre de la liste. */
typedef struct {
    const char* (*anchor)(int m);                  /* ancre du module m (NULL : aucune) */
    int         (*count)(int m);                   /* nombre d'éléments du module m */
    const char* (*key)(int m, int j);              /* nom de son élément j */
    void        (*emit)(int m, int j, void* ctx);  /* émet son élément j */
    void*         ctx;
    bool          placed[64];
} placer_t;

static void place_module(placer_t* pl, int m);

static void place_before(placer_t* pl, const char* name) {
    for (int m = 0; m < k_card_module_count; m++) {
        const char* a = pl->anchor(m);
        if (!pl->placed[m] && a && strcmp(a, name) == 0) place_module(pl, m);
    }
}

static void place_module(placer_t* pl, int m) {
    pl->placed[m] = true;
    for (int j = 0; j < pl->count(m); j++) {
        place_before(pl, pl->key(m, j));
        pl->emit(m, j, pl->ctx);
    }
}

static void place_rest(placer_t* pl) {
    for (int m = 0; m < k_card_module_count; m++)
        if (!pl->placed[m] && !pl->anchor(m) && pl->count(m) > 0) place_module(pl, m);
    for (int m = 0; m < k_card_module_count; m++)
        if (!pl->placed[m] && pl->count(m) > 0) place_module(pl, m);
}

/* ── Configuration ─────────────────────────────────────────────────────── */

/* One configuration per module, allocated once and kept (static: never
 * reported as a leak); each call resets it to the default values. */
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

static struct option* s_tbl;
static int s_n;

static const char* opt_anchor(int m) { return k_card_modules[m]->opts_before; }
static int         opt_count(int m)  { return k_card_modules[m]->nopts; }
static const char* opt_key(int m, int j) { return k_card_modules[m]->opts[j].name; }
static void opt_emit(int m, int j, void* ctx) {
    (void)ctx;
    const card_opt_t* o = &k_card_modules[m]->opts[j];
    s_tbl[s_n].name = o->name;
    s_tbl[s_n].has_arg = o->has_arg;
    s_tbl[s_n].flag = NULL;
    s_tbl[s_n].val = CARD_OPT_BASE + m * 64 + j;
    s_n++;
}

const struct option* card_modules_long_options(const struct option* core) {
    if (s_tbl) return s_tbl;
    int ncore = 0, nmod = 0;
    while (core[ncore].name) ncore++;
    for (int m = 0; m < k_card_module_count; m++) nmod += k_card_modules[m]->nopts;
    s_tbl = calloc((size_t)(ncore + nmod + 1), sizeof(struct option));
    if (!s_tbl) return core;
    placer_t pl = { opt_anchor, opt_count, opt_key, opt_emit, NULL, { false } };
    for (int i = 0; i < ncore; i++) {
        place_before(&pl, core[i].name);
        s_tbl[s_n++] = core[i];
    }
    place_rest(&pl);
    return s_tbl;                                   /* terminée par calloc : {0} */
}

/* ── Help ──────────────────────────────────────────────────────────────── */

/* Un module = un bloc d'aide, repéré par sa première option. Les blocs déjà
 * écrits sont notés dans un placeur unique, remis à zéro en fin d'aide. */
static const char* help_anchor(int m) {
    return k_card_modules[m]->help ? k_card_modules[m]->help_before : NULL;
}
static int help_count(int m) { return k_card_modules[m]->help ? 1 : 0; }
static const char* help_key(int m, int j) {
    (void)j;
    return k_card_modules[m]->nopts > 0 ? k_card_modules[m]->opts[0].name : "";
}
static void help_emit(int m, int j, void* ctx) {
    (void)j; (void)ctx;
    fputs(k_card_modules[m]->help, stdout);
}
static placer_t s_help = { help_anchor, help_count, help_key, help_emit, NULL, { false } };

void card_modules_print_help_before(const char* name) {
    if (name) {
        place_before(&s_help, name);
        return;
    }
    place_rest(&s_help);
    memset(s_help.placed, 0, sizeof(s_help.placed));   /* prête pour une autre aide */
}

/* ── Cycle de vie ──────────────────────────────────────────────────────── */

void card_modules_init(emulator_t* emu) {
    for (int m = 0; m < k_card_module_count; m++)
        if (k_card_modules[m]->init) k_card_modules[m]->init(emu);
}

void card_modules_teardown(emulator_t* emu) {
    for (int m = 0; m < k_card_module_count; m++)
        if (k_card_modules[m]->teardown) k_card_modules[m]->teardown(emu);
}

int card_modules_setup(emulator_t* emu, const struct cli_opts_s* core, card_stage_t stage) {
    for (int m = 0; m < k_card_module_count; m++) {
        const card_module_t* mod = k_card_modules[m];
        if (mod->stage == stage && mod->setup &&
            mod->setup(emu, core->card_cfg[m], core) != 0)
            return 1;
    }
    return 0;
}

/* ── Placement générique, pour le menu (cards.c) et le bus (io_bus.c) ──── */

void card_modules_place(const char* const* core_names, int ncore,
                        const char* (*anchor)(int m), int (*count)(int m),
                        const char* (*key)(int m, int j),
                        void (*emit_module)(int m, int j, void* ctx),
                        void (*emit_core)(int i, void* ctx), void* ctx) {
    placer_t pl = { anchor, count, key, emit_module, ctx, { false } };
    for (int i = 0; i < ncore; i++) {
        place_before(&pl, core_names[i]);
        emit_core(i, ctx);
    }
    place_rest(&pl);
}
