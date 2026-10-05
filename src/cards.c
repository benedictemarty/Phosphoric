/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cards.c
 * @brief Registre des cartes d'extension du menu F1 (voir include/cards.h)
 * @author bmarty <bmarty@mailo.com>
 */
#define _POSIX_C_SOURCE 200809L   /* strdup */
#include "cards.h"
#include "card_module.h"
#include "emulator.h"
#include "io/loci_emu.h"
#include "io/picowifi_detect.h"
#include "video/iom_lang.h"   /* messages de conflit dans la langue du menu */
#include "utils/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef _WIN32
#include <process.h>
#elif !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* Explications courtes, lues dans le menu : à quoi sert la carte, et ce que
 * chaque paramètre change. Les adresses sont celles de l'Oric ($03xx). */
/* Les éléments à remplacer sont en minuscules : la police du menu n'a pas de
 * majuscules accentuées. */

static const card_desc_t k_cards[] = {
    {
        "hostfs", "Hôte (hostfs)",
        "Dossier de l'ordinateur monté dans l'Oric : ses fichiers sont lus et "
        "écrits directement.",
        NULL, "-h", 0, -1, 0, 0, false,
        { { "dossier", "Dossier monté", CARD_P_DIR, "-h", ".",
            "Dossier de l'hôte vu par l'Oric." } },
        1
    },
};
#define K_CARDS ((int)(sizeof(k_cards) / sizeof(k_cards[0])))

/* Toutes les cartes : celles de k_cards, avec les fiches des cartes en modules
 * insérées avant leur ancre (desc_before), pour garder l'ordre du menu. */
#define ALL_MAX 32
static const card_desc_t* g_all[ALL_MAX];
static int g_all_n = -1;

static const char* desc_anchor(int m) { return k_card_modules[m]->desc_before; }
static int desc_count(int m) { return k_card_modules[m]->ndescs; }
static const char* desc_key(int m, int j) { return k_card_modules[m]->descs[j]->id; }
static void desc_emit(int m, int j, void* ctx) {
    (void)ctx;
    if (g_all_n < ALL_MAX) g_all[g_all_n++] = k_card_modules[m]->descs[j];
}
static void desc_emit_core(int i, void* ctx) {
    (void)ctx;
    if (g_all_n < ALL_MAX) g_all[g_all_n++] = &k_cards[i];
}

static void build_all(void) {
    if (g_all_n >= 0) return;
    const char* names[sizeof(k_cards) / sizeof(k_cards[0])];
    for (int i = 0; i < K_CARDS; i++) names[i] = k_cards[i].id;
    g_all_n = 0;
    card_modules_place(names, K_CARDS, desc_anchor, desc_count, desc_key,
                       desc_emit, desc_emit_core, NULL);
}

/* Backend LOCI présent dans ce binaire ; PHOSPHORIC_TEST_LOCI_BACKENDS (liste
 * « emul,hw ») le remplace dans les tests, liés au seul stub. */
static bool loci_backend_ok(const char* name) {
    const char* t = getenv("PHOSPHORIC_TEST_LOCI_BACKENDS");
    if (t && *t) return strstr(t, name) != NULL;
    return loci_emu_backend_available(name);
}

/* Paramètres de la carte LOCI propres à un mode (les autres : tous les modes). */
static const struct { const char* key; const char* mode; } k_loci_mode_params[] = {
    { "menu", LOCI_MODE_HLE }, { "sd", LOCI_MODE_HLE }, { "flash", LOCI_MODE_HLE },
    { "modem", LOCI_MODE_HLE }, { "port", LOCI_MODE_HLE },
    { "elf", LOCI_MODE_FW }, { "fw_flash", LOCI_MODE_FW }, { "fw_usb", LOCI_MODE_FW },
    { "port_usb", LOCI_MODE_HW },
};

/* Mode auquel est propre le paramètre @p p de @p d, NULL s'il n'en a pas. */
static const char* loci_param_mode(const card_desc_t* d, int p) {
    if (strcmp(d->id, "loci") != 0 || p < 0 || p >= d->nparams) return NULL;
    for (size_t i = 0; i < sizeof(k_loci_mode_params) / sizeof(k_loci_mode_params[0]); i++)
        if (strcmp(d->param[p].key, k_loci_mode_params[i].key) == 0) return k_loci_mode_params[i].mode;
    return NULL;
}

/* Le mode LOCI @p mode existe dans ce binaire. */
static bool loci_mode_ok(const char* mode) {
    if (strcmp(mode, LOCI_MODE_FW) == 0) return loci_backend_ok("emul");
    if (strcmp(mode, LOCI_MODE_HW) == 0) return loci_backend_ok("hw");
    return true;
}

static const card_desc_t* g_list[ALL_MAX];
static int g_n = -1;

static void build_list(void) {
    if (g_n >= 0) return;
    build_all();
    g_n = 0;
    for (int i = 0; i < g_all_n; i++) g_list[g_n++] = g_all[i];
}

int cards_count(void) { build_list(); return g_n; }
const card_desc_t* cards_get(int i) { build_list(); return (i >= 0 && i < g_n) ? g_list[i] : NULL; }
int cards_find(const char* id) {
    build_list();
    for (int i = 0; i < g_n; i++) if (strcmp(g_list[i]->id, id) == 0) return i;
    return -1;
}

static void set_value(card_choice_t* c, int p, const char* v) {
    snprintf(c->value[p], sizeof(c->value[p]), "%s", v ? v : "");
}

void cards_state_defaults(cards_state_t* st) {
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < cards_count(); i++) {
        const card_desc_t* d = cards_get(i);
        st->card[i].on = d->fixed;
        for (int p = 0; p < d->nparams; p++)
            cards_param_default(&d->param[p], st->card[i].value[p], CARD_VALUE_MAX);
    }
}

void cards_set_on(cards_state_t* st, int i, bool on) {
    const card_desc_t* d = cards_get(i);
    if (!d || d->fixed) return;
    if (on && d->group) {
        for (int j = 0; j < cards_count(); j++) {
            const card_desc_t* o = cards_get(j);
            if (j != i && o->group && strcmp(o->group, d->group) == 0) st->card[j].on = false;
        }
    }
    st->card[i].on = on;
}

/* Options de carte sans argument (les autres en prennent un ; une valeur
 * collée « --opt=v » est reconnue partout). */
static bool option_takes_arg(const char* opt) {
    static const char* const flags[] = {
        "--loci", "--serial-v23", "--serial-irq-on-rdrf",
        "--serial-tcp-backpressure", "--no-config-cards", NULL
    };
    for (int i = 0; flags[i]; i++) if (strcmp(opt, flags[i]) == 0) return false;
    return !card_modules_option_is_flag(opt);
}

/* Formes longues des options courtes des cartes. */
static const char* canon(const char* opt) {
    if (strcmp(opt, "--hostfs") == 0) return "-h";
    if (strcmp(opt, "--rom") == 0) return "-r";
    return opt;
}

/* Options secondaires qui suivent leur carte sans être des paramètres du menu
 * (--loci-usb, --loci-web…, --serial-trace…) : gardées si la carte reste
 * active, retirées sinon. NULL si l'option n'appartient à aucune carte. */
static const char* extra_owner(const char* opt) {
    if (strncmp(opt, "--loci-", 7) == 0) return "loci";
    if (strncmp(opt, "--serial-", 9) == 0) return "acia";
    return NULL;
}

static bool is_card_option(const char* opt) {
    opt = canon(opt);
    if (strcmp(opt, "--no-config-cards") == 0) return true;
    build_all();
    for (int i = 0; i < g_all_n; i++) {
        const card_desc_t* d = g_all[i];
        if (d->enable_cli && strcmp(opt, d->enable_cli) == 0) return true;
        for (int p = 0; p < d->nparams; p++)
            if (d->param[p].cli && strcmp(opt, d->param[p].cli) == 0) return true;
    }
    return false;
}

/* « --opt=valeur » ou « --opt valeur » : nom de l'option et valeur. */
static const char* split_opt(const char* a, char* name, size_t namesz) {
    const char* eq = strncmp(a, "--", 2) == 0 ? strchr(a, '=') : NULL;
    size_t n = eq ? (size_t)(eq - a) : strlen(a);
    if (n >= namesz) n = namesz - 1;
    memcpy(name, a, n);
    name[n] = '\0';
    return eq ? eq + 1 : NULL;
}

static int param_find(const card_desc_t* d, const char* key) {
    for (int p = 0; p < d->nparams; p++) if (strcmp(d->param[p].key, key) == 0) return p;
    return -1;
}

void cards_param_default(const card_param_t* p, char* out, size_t outsz) {
    const char* d = p->def ? p->def : "";
    snprintf(out, outsz, "%.*s", (int)(p->kind == CARD_P_CHOICE ? strcspn(d, "|") : strlen(d)), d);
}

/* La valeur @p v est celle par défaut du paramètre @p p. */
static bool is_default(const card_param_t* p, const char* v) {
    char def[CARD_VALUE_MAX];
    cards_param_default(p, def, sizeof(def));
    return strcmp(v, def) == 0;
}

static void choice_next(const card_param_t* p, char* value, size_t valuesz) {
    const char* c = p->def ? p->def : "";
    const char* first = c;
    size_t vlen = strlen(value);
    while (*c) {
        const char* bar = strchr(c, '|');
        size_t len = bar ? (size_t)(bar - c) : strlen(c);
        if (len == vlen && strncmp(c, value, len) == 0) {
            const char* next = bar ? bar + 1 : first;
            size_t nlen = strcspn(next, "|");
            snprintf(value, valuesz, "%.*s", (int)nlen, next);
            return;
        }
        if (!bar) break;
        c = bar + 1;
    }
    snprintf(value, valuesz, "%.*s", (int)strcspn(first, "|"), first);   /* inconnue */
}

void cards_choice_next(const card_param_t* p, char* value, size_t valuesz) {
    const bool loci_mode = strcmp(p->key, "mode") == 0;
    for (int i = 0; i < 8; i++) {   /* au plus une valeur par choix */
        choice_next(p, value, valuesz);
        if (!loci_mode || loci_mode_ok(value)) return;
    }
}

/* ── Carte LOCI : mode ─────────────────────────────────────────────────── */

static int loci_param(const char* key) {
    int il = cards_find("loci");
    return il < 0 ? -1 : param_find(cards_get(il), key);
}

/* Mode de la carte LOCI dans @p st (LOCI_MODE_HLE si la carte manque). */
static const char* loci_mode(const cards_state_t* st) {
    int il = cards_find("loci"), pm = loci_param("mode");
    return il >= 0 && pm >= 0 ? st->card[il].value[pm] : LOCI_MODE_HLE;
}

static bool loci_mode_is(const cards_state_t* st, const char* mode) {
    return strcmp(loci_mode(st), mode) == 0;
}

bool cards_param_applies(const card_desc_t* d, const card_choice_t* c, int p) {
    const char* m = loci_param_mode(d, p);
    int pm = m ? param_find(d, "mode") : -1;
    return !m || pm < 0 || strcmp(c->value[pm], m) == 0;
}

/* Modem de la carte LOCI : indices de la carte et de ses paramètres « modem » /
 * « port » ; false si la carte (ou le paramètre) manque dans cette build. */
static bool loci_modem_params(int* il, int* pm, int* pp) {
    *il = cards_find("loci");
    if (*il < 0) return false;
    *pm = param_find(cards_get(*il), "modem");
    *pp = param_find(cards_get(*il), "port");
    return *pm >= 0 && *pp >= 0;
}

/* Port du picowifi réel choisi pour LOCI : le port renseigné, sinon celui que
 * la détection USB trouve. false si aucun. */
static bool loci_modem_real_port(const card_choice_t* c, int pp, char* out, size_t outsz) {
    if (c->value[pp][0]) { snprintf(out, outsz, "%s", c->value[pp]); return true; }
    return picowifi_detect(NULL, out, outsz);
}

/* Transport série @p spec vu comme modem LOCI : « simulé » pour picowifi sans
 * identifiants, « réel » (+ port) pour le port série à 115200 8N1. NULL sinon. */
static const char* loci_modem_of_spec(const char* spec, char* port, size_t portsz) {
    static const char com[] = "com:115200,8,N,1,";
    port[0] = '\0';
    if (!spec) return NULL;
    if (strcmp(spec, "picowifi") == 0) return LOCI_MODEM_SIM;
    if (strncmp(spec, com, sizeof(com) - 1) == 0 && spec[sizeof(com) - 1]) {
        char found[256];
        const char* dev = spec + sizeof(com) - 1;
        /* Port détecté : laissé vide, la détection le retrouvera (même s'il change). */
        if (!(picowifi_detect(NULL, found, sizeof(found)) && strcmp(found, dev) == 0))
            snprintf(port, portsz, "%s", dev);
        return LOCI_MODEM_REAL;
    }
    return NULL;
}

/* LOCI + ACIA réduite à un modem picowifi ($0380, sans autre réglage) : c'est
 * le modem de la carte LOCI, pas une carte ACIA à part. */
static void loci_modem_normalize(cards_state_t* st) {
    int il, pm, pp, ia = cards_find("acia");
    if (!loci_modem_params(&il, &pm, &pp) || ia < 0) return;
    if (!st->card[il].on || !st->card[ia].on || !loci_mode_is(st, LOCI_MODE_HLE)) return;
    const card_desc_t* a = cards_get(ia);
    for (int p = 1; p < a->nparams; p++) {
        const char* v = st->card[ia].value[p];
        const bool addr_ok = p == a->io_param && strcasecmp(v, "0380") == 0;
        if (!addr_ok && !is_default(&a->param[p], v)) return;
    }
    char port[CARD_VALUE_MAX];
    const char* m = loci_modem_of_spec(st->card[ia].value[0], port, sizeof(port));
    if (!m) return;
    set_value(&st->card[il], pm, m);
    set_value(&st->card[il], pp, port);
    for (int p = 0; p < a->nparams; p++)
        cards_param_default(&a->param[p], st->card[ia].value[p], CARD_VALUE_MAX);
    st->card[ia].on = false;
}

/* La carte LOCI fournit un modem (ses options --serial-* restent utiles). */
static bool loci_modem_on(const cards_state_t* st) {
    int il, pm, pp;
    return loci_modem_params(&il, &pm, &pp) && st->card[il].on && loci_mode_is(st, LOCI_MODE_HLE) &&
           strcmp(st->card[il].value[pm], LOCI_MODEM_NONE) != 0;
}

/* Port de la LOCI-USB : celui renseigné (@p v), sinon celui
 * que la détection USB trouve. false si aucun. */
static bool loci_hw_port(const char* v, char* out, size_t outsz) {
    if (v[0]) { snprintf(out, outsz, "%s", v); return true; }
    return loci_usb_detect(NULL, out, outsz);
}

/* Port de la LOCI-USB égal à celui détecté : laissé vide, la détection le retrouvera
 * (même s'il change d'un branchement à l'autre). */
static void loci_hw_normalize(cards_state_t* st) {
    int il = cards_find("loci"), pp = loci_param("port_usb");
    char found[256];
    if (il < 0 || pp < 0 || !st->card[il].value[pp][0]) return;
    if (loci_usb_detect(NULL, found, sizeof(found)) && strcmp(found, st->card[il].value[pp]) == 0)
        st->card[il].value[pp][0] = '\0';
}

void cards_state_from(cards_state_t* st, const emulator_t* emu, int argc, char* const argv[]) {
    cards_state_defaults(st);
    /* 1. Ligne de commande : chaque option de carte, avec sa valeur telle quelle. */
    for (int a = 1; a < argc; a++) {
        char name[48];
        const char* inl = split_opt(argv[a], name, sizeof(name));
        { char t[48]; snprintf(t, sizeof(t), "%s", canon(name)); memcpy(name, t, sizeof(t)); }
        const char* owner = extra_owner(name);
        if (!is_card_option(name)) {
            if (owner) {          /* option secondaire : la carte est active */
                int i = cards_find(owner);
                if (i >= 0) cards_set_on(st, i, true);
                if (!inl && option_takes_arg(name) && a + 1 < argc) a++;
            }
            continue;
        }
        const char* val = inl;
        if (!val && option_takes_arg(name) && a + 1 < argc) val = argv[++a];
        for (int i = 0; i < cards_count(); i++) {
            const card_desc_t* d = cards_get(i);
            if (d->enable_cli && strcmp(name, d->enable_cli) == 0) {
                cards_set_on(st, i, true);
                if (d->enable_param >= 0 && val) set_value(&st->card[i], d->enable_param, val);
            }
            for (int p = 0; p < d->nparams; p++) {
                if (!d->param[p].cli || strcmp(name, d->param[p].cli) != 0) continue;
                if (d->param[p].kind == CARD_P_BOOL) set_value(&st->card[i], p, "oui");
                else if (val) set_value(&st->card[i], p, val);
                /* Une option propre à la carte l'active (--loci-sdimg implique --loci) ;
                 * propre à un mode, elle le choisit (--loci-hw : usb). */
                if (strcmp(d->id, "loci") == 0) {
                    const char* m = loci_param_mode(d, p);
                    cards_set_on(st, i, true);
                    if (m && strcmp(m, LOCI_MODE_HLE) != 0)
                        set_value(&st->card[i], param_find(d, "mode"), m);
                }
            }
        }
    }
    /* 2. Machine en cours : ce que phosphoric.cfg a ajouté (interface disque). */
    if (emu) {
        int i;
        if (emu->card_on[CARD_IDX_microdisc] && (i = cards_find("microdisc")) >= 0) {
            cards_set_on(st, i, true);
            if (emu->diskrom_path) set_value(&st->card[i], 0, emu->diskrom_path);
        }
        if (emu->card_on[CARD_IDX_jasmin] && (i = cards_find("jasmin")) >= 0) {
            cards_set_on(st, i, true);
            if (emu->jasmin_rom_path) set_value(&st->card[i], 0, emu->jasmin_rom_path);
        }
        if (emu->card_on[CARD_IDX_loci] && !emu->loci_external && (i = cards_find("loci")) >= 0) {
            cards_set_on(st, i, true);
            const char* rom = emu->rom_path ? emu->rom_path : "";
            set_value(&st->card[i], loci_param("menu"), strstr(rom, "locirom") ? "oui" : "non");
        }
        /* Modem LOCI venu de phosphoric.cfg (ACIA sans carte ACIA au menu). */
        int il, pm, pp, ia = cards_find("acia");
        if (loci_modem_params(&il, &pm, &pp) && ia >= 0 && st->card[il].on &&
            !st->card[ia].on && emu->card_on[CARD_IDX_acia]) {
            char port[CARD_VALUE_MAX];
            const char* m = loci_modem_of_spec(emu->serial_spec, port, sizeof(port));
            if (m) { set_value(&st->card[il], pm, m); set_value(&st->card[il], pp, port); }
        }
    }
    loci_modem_normalize(st);
    loci_hw_normalize(st);
}

bool cards_conflict(const cards_state_t* st, char* out, size_t outsz) {
    unsigned lo[16], hi[16];
    for (int i = 0; i < cards_count(); i++) {
        const card_desc_t* d = cards_get(i);
        lo[i] = hi[i] = 0;
        if (!st->card[i].on || !d->io_size) continue;
        unsigned base = d->io_base;
        if (d->io_param >= 0) base = (unsigned)strtoul(st->card[i].value[d->io_param], NULL, 16);
        if (!base) continue;
        lo[i] = base;
        hi[i] = base + d->io_size - 1;
        for (int j = 0; j < i; j++) {
            if (!hi[j]) continue;
            if (lo[i] <= hi[j] && lo[j] <= hi[i]) {
                snprintf(out, outsz, iom_tr("%s et %s se chevauchent en $%04X"),
                         iom_tr(cards_get(j)->name), iom_tr(d->name), lo[i] > lo[j] ? lo[i] : lo[j]);
                return true;
            }
        }
    }
    int il = cards_find("loci"), pm, pp, ia = cards_find("acia");
    if (il >= 0 && st->card[il].on) {
        const char* mode = loci_mode(st);
        char dev[256];
        if (!loci_mode_ok(mode)) {
            snprintf(out, outsz, iom_tr("LOCI : mode « %s » absent de ce binaire"), mode);
            return true;
        }
        if (strcmp(mode, LOCI_MODE_FW) == 0 && !st->card[il].value[loci_param("elf")][0]) {
            snprintf(out, outsz, "%s", iom_tr("LOCI firmware : indiquer le fichier ELF du firmware"));
            return true;
        }
        if (strcmp(mode, LOCI_MODE_HW) == 0 &&
            !loci_hw_port(st->card[il].value[loci_param("port_usb")], dev, sizeof(dev))) {
            snprintf(out, outsz, "%s", iom_tr("LOCI-USB : aucune détectée (indiquer le port)"));
            return true;
        }
    }
    if (loci_modem_on(st) && loci_modem_params(&il, &pm, &pp)) {
        char dev[256];
        if (ia >= 0 && st->card[ia].on) {
            snprintf(out, outsz, "%s", iom_tr("Modem LOCI et ACIA 6551 : une seule ligne série à la fois"));
            return true;
        }
        if (strcmp(st->card[il].value[pm], LOCI_MODEM_REAL) == 0 &&
            !loci_modem_real_port(&st->card[il], pp, dev, sizeof(dev))) {
            snprintf(out, outsz, "%s", iom_tr("Modem LOCI réel : aucun picowifi USB détecté (indiquer le port)"));
            return true;
        }
    }
    if (out && outsz) out[0] = '\0';
    return false;
}

static void push(char*** av, int* n, int* cap, const char* s) {
    if (*n + 2 > *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *av = realloc(*av, (size_t)*cap * sizeof(char*));
    }
    (*av)[(*n)++] = strdup(s);
    (*av)[*n] = NULL;
}

char** cards_build_argv(const cards_state_t* st, int argc, char* const argv[], int* out_argc) {
    char** av = NULL;
    int n = 0, cap = 0;
    const int iloci = cards_find("loci");
    const bool loci_hle = iloci >= 0 && st->card[iloci].on && loci_mode_is(st, LOCI_MODE_HLE);
    const bool loci_menu = loci_hle && strcmp(st->card[iloci].value[loci_param("menu")], "oui") == 0;
    const char* basic_rom = "roms/basic11b.rom";
    push(&av, &n, &cap, argc > 0 ? argv[0] : "oric1-emu");
    /* 1. Options d'origine, sans celles des cartes (ni -r si LOCI la fixe). */
    for (int a = 1; a < argc; a++) {
        char name[48];
        const char* inl = split_opt(argv[a], name, sizeof(name));
        { char t[48]; snprintf(t, sizeof(t), "%s", canon(name)); memcpy(name, t, sizeof(t)); }
        const bool card = is_card_option(name);
        const bool rom = strcmp(name, "-r") == 0;
        const bool takes = !inl && option_takes_arg(name) && a + 1 < argc;
        if (card) { if (takes) a++; continue; }
        const char* owner = extra_owner(name);
        if (owner) {
            int i = cards_find(owner);
            const bool keep = i >= 0 && (st->card[i].on ||
                              (strcmp(owner, "acia") == 0 && loci_modem_on(st)));
            if (keep) push(&av, &n, &cap, argv[a]);
            if (takes) { a++; if (keep) push(&av, &n, &cap, argv[a]); }
            continue;
        }
        if (rom && a + 1 < argc) {
            const char* r = argv[a + 1];
            a++;
            if (loci_menu) continue;                       /* remplacée ci-dessous */
            push(&av, &n, &cap, "-r");
            /* LOCI éteinte (ou servie par son firmware) : sa ROM de menu n'a plus
             * de sens → BASIC 1.1. */
            push(&av, &n, &cap, strstr(r, "locirom") && !loci_hle ? basic_rom : r);
            continue;
        }
        push(&av, &n, &cap, argv[a]);
    }
    if (loci_menu) { push(&av, &n, &cap, "-r"); push(&av, &n, &cap, "roms/loci/locirom"); }
    /* 2. Cartes actives : option d'activation puis paramètres renseignés. */
    for (int i = 0; i < cards_count(); i++) {
        const card_desc_t* d = cards_get(i);
        const card_choice_t* c = &st->card[i];
        if (!c->on || d->fixed || !d->enable_cli) continue;
        /* LOCI : --loci pour le modèle intégré ; en firmware et usb, --loci-emu
         * et --loci-hw (paramètres) l'activent. */
        const bool is_loci = i == iloci;
        if (d->enable_param < 0 && (!is_loci || loci_hle)) push(&av, &n, &cap, d->enable_cli);
        for (int p = 0; p < d->nparams; p++) {
            const card_param_t* pp = &d->param[p];
            if (!pp->cli || !cards_param_applies(d, c, p)) continue;
            if (is_loci && strcmp(pp->key, "port_usb") == 0) {
                char dev[CARD_VALUE_MAX];
                if (loci_hw_port(c->value[p], dev, sizeof(dev))) {
                    push(&av, &n, &cap, pp->cli);
                    push(&av, &n, &cap, dev);
                } else {
                    log_warning("LOCI-USB : aucune (« %s… ») détectée, LOCI n'est pas "
                                "branchée (indiquer le port : loci.port_usb=/dev/ttyACM0)",
                                LOCI_USB_PRODUCT_PREFIX);
                }
                continue;
            }
            if (p == d->enable_param) {
                push(&av, &n, &cap, pp->cli);
                push(&av, &n, &cap, c->value[p]);
            } else if (pp->kind == CARD_P_BOOL) {
                if (strcmp(c->value[p], "oui") == 0) push(&av, &n, &cap, pp->cli);
            } else if (c->value[p][0] && !is_default(pp, c->value[p])) {
                push(&av, &n, &cap, pp->cli);
                push(&av, &n, &cap, c->value[p]);
            }
        }
    }
    /* 3. Modem de la carte LOCI : l'ACIA en $0380 (sauf carte ACIA active). */
    int il, pm, pp, ia = cards_find("acia");
    if (loci_modem_params(&il, &pm, &pp) && loci_hle && !(ia >= 0 && st->card[ia].on)) {
        const char* m = st->card[il].value[pm];
        char dev[256], spec[300];
        if (strcmp(m, LOCI_MODEM_SIM) == 0) {
            push(&av, &n, &cap, "--serial");
            push(&av, &n, &cap, "picowifi");
        } else if (strcmp(m, LOCI_MODEM_REAL) == 0 &&
                   loci_modem_real_port(&st->card[il], pp, dev, sizeof(dev))) {
            picowifi_serial_spec(dev, spec, sizeof(spec));
            push(&av, &n, &cap, "--serial");
            push(&av, &n, &cap, spec);
        } else if (strcmp(m, LOCI_MODEM_REAL) == 0) {
            log_warning("Modem LOCI réel : aucun picowifi USB (« %s ») détecté, LOCI démarre "
                        "sans modem (indiquer le port : loci.port=/dev/ttyACM0)",
                        PICOWIFI_USB_PRODUCT);
        }
    }
    push(&av, &n, &cap, "--no-config-cards");
    if (out_argc) *out_argc = n;
    return av;
}

void cards_argv_free(char** av) {
    if (!av) return;
    for (char** p = av; *p; p++) free(*p);
    free(av);
}

/* Anciennes fiches LOCI (2.24 et avant : « LOCI firmware », 2.25.0 : « LOCI
 * réelle »), devenues des modes de la carte LOCI ; 2.26.0 : mode « réelle » et
 * clé « pont », devenus « usb » et « port_usb ». */
static const struct { const char* old_key; const char* key; } k_cfg_legacy[] = {
    { "loci_emu.elf", "loci.elf" }, { "loci_emu.flash", "loci.fw_flash" },
    { "loci_emu.usb", "loci.fw_usb" }, { "loci_hw.port", "loci.port_usb" },
    { "loci.pont", "loci.port_usb" },
};

/* Carte que désigne « carte.<id> » (ancienne fiche LOCI : loci). */
static const char* cfg_card_id(const char* id) {
    return strcmp(id, "loci_emu") == 0 || strcmp(id, "loci_hw") == 0 ? "loci" : id;
}

bool cards_cfg_line(cards_state_t* st, const char* key, const char* val, bool* card_seen) {
    for (size_t k = 0; k < sizeof(k_cfg_legacy) / sizeof(k_cfg_legacy[0]); k++)
        if (strcmp(key, k_cfg_legacy[k].old_key) == 0) key = k_cfg_legacy[k].key;
    if (strcmp(key, "loci.mode") == 0 && strcmp(val, "réelle") == 0) val = LOCI_MODE_HW;
    if (strcmp(key, "carte.loci_emu") == 0 || strcmp(key, "carte.loci_hw") == 0) {
        int i = cards_find("loci");
        if (i < 0) return false;
        if (strcasecmp(val, "oui") == 0) {   /* « non » : ne dit rien de la carte LOCI */
            cards_set_on(st, i, true);
            set_value(&st->card[i], loci_param("mode"),
                      strcmp(key + 6, "loci_emu") == 0 ? LOCI_MODE_FW : LOCI_MODE_HW);
            if (card_seen) *card_seen = true;
        }
        return true;
    }
    if (strncmp(key, "carte.", 6) == 0) {
        int i = cards_find(key + 6);
        if (i < 0) return false;
        cards_set_on(st, i, strcasecmp(val, "oui") == 0);
        if (card_seen) *card_seen = true;
        return true;
    }
    const char* dot = strchr(key, '.');
    if (!dot) return false;
    char id[32];
    size_t n = (size_t)(dot - key);
    if (n >= sizeof(id)) return false;
    memcpy(id, key, n);
    id[n] = '\0';
    int i = cards_find(id);
    if (i < 0) return false;
    const card_desc_t* d = cards_get(i);
    for (int p = 0; p < d->nparams; p++) {
        if (strcmp(d->param[p].key, dot + 1) == 0) {
            set_value(&st->card[i], p, val);
            return true;
        }
    }
    return false;
}

void cards_cfg_write(const cards_state_t* st, void* file) {
    FILE* f = (FILE*)file;
    for (int i = 0; i < cards_count(); i++) {
        const card_desc_t* d = cards_get(i);
        if (d->fixed) continue;
        fprintf(f, "carte.%s=%s\n", d->id, st->card[i].on ? "oui" : "non");
        if (!st->card[i].on) continue;
        for (int p = 0; p < d->nparams; p++)
            if (st->card[i].value[p][0] && !is_default(&d->param[p], st->card[i].value[p]) &&
                cards_param_applies(d, &st->card[i], p))
                fprintf(f, "%s.%s=%s\n", d->id, d->param[p].key, st->card[i].value[p]);
    }
}

bool cards_cfg_key(const char* key, size_t keylen) {
    char k[64];
    if (keylen >= sizeof(k)) return false;
    memcpy(k, key, keylen);
    k[keylen] = '\0';
    if (strncmp(k, "carte.", 6) == 0) return cards_find(cfg_card_id(k + 6)) >= 0;
    char* dot = strchr(k, '.');
    if (!dot) return false;
    *dot = '\0';
    return cards_find(cfg_card_id(k)) >= 0;
}

bool cards_cfg_read(const char* path, cards_state_t* st, bool seen[16]) {
    cards_state_defaults(st);
    for (int i = 0; i < 16; i++) seen[i] = false;
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = '\0';
        char* key = line;
        while (*key == ' ') key++;
        char* end = eq;
        while (end > key && end[-1] == ' ') *--end = '\0';
        char* val = eq + 1;
        while (*val == ' ') val++;
        bool card_seen = false;
        if (cards_cfg_line(st, key, val, &card_seen) && card_seen) {
            int i = cards_find(cfg_card_id(key + 6));
            if (i >= 0) seen[i] = true;
        }
    }
    fclose(f);
    return true;
}

void cards_cli_mentioned(int argc, char* const argv[], bool mentioned[16]) {
    for (int i = 0; i < 16; i++) mentioned[i] = false;
    for (int a = 1; a < argc; a++) {
        char name[48];
        const char* inl = split_opt(argv[a], name, sizeof(name));
        { char t[48]; snprintf(t, sizeof(t), "%s", canon(name)); memcpy(name, t, sizeof(t)); }
        const char* owner = extra_owner(name);
        const bool card = is_card_option(name);
        if (!card && !owner) continue;
        if (!inl && option_takes_arg(name) && a + 1 < argc) a++;
        if (owner) { int i = cards_find(owner); if (i >= 0) mentioned[i] = true; continue; }
        for (int i = 0; i < cards_count(); i++) {
            const card_desc_t* d = cards_get(i);
            if (d->enable_cli && strcmp(name, d->enable_cli) == 0) mentioned[i] = true;
            for (int p = 0; p < d->nparams; p++)
                if (d->param[p].cli && strcmp(name, d->param[p].cli) == 0) mentioned[i] = true;
        }
    }
}

char** cards_config_argv(const char* path, int argc, char* const argv[], int* out_argc) {
    cards_state_t from_cfg, adopted;
    bool seen[16], mentioned[16];
    if (!cards_cfg_read(path, &from_cfg, seen)) return NULL;
    cards_cli_mentioned(argc, argv, mentioned);
    cards_state_defaults(&adopted);
    bool any = false;
    for (int i = 0; i < cards_count(); i++) {
        const card_desc_t* d = cards_get(i);
        if (!seen[i] || mentioned[i] || d->fixed || !from_cfg.card[i].on) continue;
        bool group_taken = false;   /* la ligne de commande a choisi une carte du groupe */
        for (int j = 0; d->group && j < cards_count(); j++) {
            const card_desc_t* o = cards_get(j);
            if (mentioned[j] && o->group && strcmp(o->group, d->group) == 0) group_taken = true;
        }
        if (group_taken) continue;
        adopted.card[i] = from_cfg.card[i];
        any = true;
    }
    /* La ligne de commande choisit déjà la ligne série : pas de modem LOCI. */
    int il, pm, pp, ia = cards_find("acia");
    if (loci_modem_params(&il, &pm, &pp) && ia >= 0 && mentioned[ia])
        set_value(&adopted.card[il], pm, LOCI_MODEM_NONE);
    if (!any) return NULL;
    char* argv0[] = { argc > 0 ? argv[0] : "oric1-emu", NULL };
    int n = 0;
    char** av = cards_build_argv(&adopted, 1, argv0, &n);
    /* --no-config-cards final retiré ; -r du menu LOCI retiré si la ligne de
     * commande fixe déjà la ROM. */
    bool cli_rom = false;
    for (int a = 1; a < argc; a++)
        if (strcmp(argv[a], "-r") == 0 || strcmp(argv[a], "--rom") == 0 ||
            strncmp(argv[a], "--rom=", 6) == 0) cli_rom = true;
    int w = 1;
    for (int r = 1; r < n; r++) {
        if (strcmp(av[r], "--no-config-cards") == 0) { free(av[r]); continue; }
        if (cli_rom && strcmp(av[r], "-r") == 0 && r + 1 < n) {
            free(av[r]); free(av[r + 1]); r++; continue;
        }
        av[w++] = av[r];
    }
    av[w] = NULL;
    if (out_argc) *out_argc = w;
    return av;
}

void cards_exec(char** av) {
#if defined(__EMSCRIPTEN__)
    (void)av;
    log_error("cartes : relance impossible dans la version web");
#elif defined(_WIN32)
    fflush(NULL);
    _execv(av[0], (const char* const*)av);
    log_error("cartes : relance impossible (%s)", av[0]);
#else
    char self[1024];
    fflush(NULL);
#ifdef __linux__
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) { self[n] = '\0'; execv(self, av); }
#elif defined(__APPLE__)
    uint32_t sz = sizeof(self);
    if (_NSGetExecutablePath(self, &sz) == 0) execv(self, av);
#endif
    (void)self;
    execvp(av[0], av);
    log_error("cartes : relance impossible (%s)", av[0]);
#endif
}
