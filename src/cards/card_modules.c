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

/* ── Placement by anchors (shared by options, help, menu and bus) ─────────
 *
 * Each module is placed just before the item its anchor names. Three passes:
 *   1. walking the core items, the modules anchored on each one are placed
 *      before it;
 *   2. then the modules without an anchor, in list order;
 *   3. then those whose anchor does not exist.
 * Placing a module first places the modules anchored on one of its items: an
 * anchor may therefore name another module, whatever the list order. */
typedef struct {
    const char* (*anchor)(int m);                  /* anchor of module m (NULL: none) */
    int         (*count)(int m);                   /* number of items of module m */
    const char* (*key)(int m, int j);              /* name of its item j */
    void        (*emit)(int m, int j, void* ctx);  /* emits its item j */
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

/* ── getopt table ──────────────────────────────────────────────────────── */

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
    return s_tbl;                                   /* terminated by calloc: {0} */
}

/* ── Help ──────────────────────────────────────────────────────────────── */

/* The help is placed in blocks (card_help_t), each with its anchor: a block is
 * identified by the first option it announces (« --name » at the start of a line),
 * so that the blocks anchored on it come first. The blocks already written are
 * recorded, reset at the end of the help. */
static bool s_help_done[64][4];

static void help_key(const char* text, char* out, size_t outsz) {
    const char* p = strstr(text, "--");
    size_t n = p ? strcspn(p + 2, " =[\n") : 0;
    if (n >= outsz) n = outsz - 1;
    if (p) memcpy(out, p + 2, n);
    out[n] = '\0';
}

static void help_unit(int m, int k);

static void help_before(const char* name) {
    for (int m = 0; m < k_card_module_count; m++)
        for (int k = 0; k < k_card_modules[m]->nhelps && k < 4; k++) {
            const char* a = k_card_modules[m]->helps[k].before;
            if (!s_help_done[m][k] && a && strcmp(a, name) == 0) help_unit(m, k);
        }
}

static void help_unit(int m, int k) {
    char key[64];
    s_help_done[m][k] = true;
    help_key(k_card_modules[m]->helps[k].text, key, sizeof(key));
    if (key[0]) help_before(key);
    fputs(k_card_modules[m]->helps[k].text, stdout);
}

void card_modules_print_help_before(const char* name) {
    if (name) {
        help_before(name);
        return;
    }
    for (int pass = 0; pass < 2; pass++)              /* no anchor, then anchor not found */
        for (int m = 0; m < k_card_module_count; m++)
            for (int k = 0; k < k_card_modules[m]->nhelps && k < 4; k++)
                if (!s_help_done[m][k] && (pass == 1 || !k_card_modules[m]->helps[k].before))
                    help_unit(m, k);
    memset(s_help_done, 0, sizeof(s_help_done));    /* ready for another help */
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

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

/* ── Generic placement, for the menu (cards.c) and the bus (io_bus.c) ──── */

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
