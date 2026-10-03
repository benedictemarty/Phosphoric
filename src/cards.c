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
#define TRANSPORTS_SERIE \
    "loopback (écho local), tcp:hôte:port, modem:hôte:port (appels entrants), " \
    "pty (pseudo-terminal), com:bauds,bits,parité,stop,périphérique (port série " \
    "réel), file:entrée[:sortie]"

static const card_desc_t k_cards[] = {
    {
        "microdisc", "Microdisc",
        "Contrôleur de disquettes Oric (WD1793), 4 lecteurs 3\". Les disquettes "
        "s'insèrent ensuite dans la section Disquettes du menu.",
        "disque", "--disk-rom", 0, -1, 0x0310, 9, false,   /* $0310-$0313, $0314, $0318 */
        { { "rom", "ROM du contrôleur", CARD_P_FILE, "--disk-rom", "roms/microdis.rom",
            "Micrologiciel du Microdisc (microdis.rom) : démarrage du DOS et accès disque." } },
        1
    },
    {
        "jasmin", "Jasmin",
        "Contrôleur de disquettes Jasmin (WD177x), autre standard Oric (TDOS).",
        "disque", "--jasmin-rom", 0, -1, 0x03F4, 12, false,
        { { "rom", "ROM de démarrage", CARD_P_FILE, "--jasmin-rom", "roms/jasmin.rom",
            "ROM de démarrage Jasmin (2 Ko, jasmin.rom), servie en $F800." } },
        1
    },
    {
        "loci", "LOCI",
        "Cartouche LOCI (modèle intégré) : menu de fichiers, émulation Microdisc et "
        "cassette depuis une carte SD ou une clé USB, ACIA en $0380.",
        "disque", "--loci", -1, -1, 0x03A0, 32, false,
        { { "menu", "Démarrer sur le menu LOCI", CARD_P_BOOL, NULL, "oui",
            "oui : l'Oric démarre sur le menu de la carte (ROM roms/loci/locirom) ; "
            "non : BASIC direct, LOCI reste disponible." },
          { "sd", "Image de carte SD", CARD_P_FILE, "--loci-sdimg", "",
            "Image FAT16/32 lue par LOCI : ses .dsk et .tap apparaissent dans son menu. "
            "Vide : aucune." },
          { "flash", "Dossier flash interne", CARD_P_DIR, "--loci-flash", "",
            "Dossier de l'hôte servant de mémoire flash interne (fichiers 0: du LOCI). "
            "Vide : aucun." } },
        3
    },
    {
        "acia", "ACIA 6551",
        "Port série (MOS 6551) : modem, terminal, Minitel, BBS. Avec LOCI, l'ACIA de "
        "la carte est en $0380.",
        NULL, "--serial", 0, 1, 0, 4, false,
        { { "transport", "Ligne série", CARD_P_TEXT, "--serial", "loopback",
            "Où va la ligne : " TRANSPORTS_SERIE ", picowifi[:ssid[:mot_de_passe]] "
            "(modem Wi-Fi émulé)." },
          { "adresse", "Adresse d'E/S", CARD_P_HEX, "--acia-addr", "031C",
            "031C : carte série Oric standard ; 0380 : ACIA de la carte LOCI." },
          { "bauds", "Vitesse (bauds)", CARD_P_TEXT, "--serial-baud", "",
            "Cadence réaliste quand l'ACIA prend son horloge à l'extérieur ; vide : "
            "transfert instantané." },
          { "tampon", "Tampon de réception", CARD_P_TEXT, "--serial-buffer", "",
            "N octets mis en attente à l'arrivée (évite de perdre des octets) ; vide : "
            "aucun." },
          { "v23", "Mode V23 (1200/75)", CARD_P_BOOL, "--serial-v23", "non",
            "oui : vitesses asymétriques du Minitel et de Prestel." } },
        5
    },
    {
        "hostfs", "Hôte (hostfs)",
        "Dossier de l'ordinateur monté dans l'Oric : ses fichiers sont lus et "
        "écrits directement.",
        NULL, "-h", 0, -1, 0, 0, false,
        { { "dossier", "Dossier monté", CARD_P_DIR, "-h", ".",
            "Dossier de l'hôte vu par l'Oric." } },
        1
    },
    {
        "loci_emu", "LOCI firmware",
        "LOCI co-simulé : le vrai firmware RP2040 tourne dans l'émulateur "
        "(développement du firmware).",
        "disque", "--loci-emu", 0, -1, 0x03A0, 32, false,
        { { "elf", "Firmware (ELF)", CARD_P_FILE, "--loci-emu", "",
            "Fichier loci-firmware.elf compilé pour RP2040." },
          { "flash", "Image flash", CARD_P_FILE, "--loci-emu-flash", "",
            "Mémoire flash persistante du firmware ; vide : <ELF>.flash ; « - » : "
            "volatile." },
          { "usb", "Image de clé USB", CARD_P_FILE, "--loci-usb-image", "",
            "Image FAT servie au firmware comme clé USB ; vide : aucune." } },
        3
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

/* Disponibilité dans cette build : la co-simulation LOCI exige le backend
 * « emul » (make LOCI_EMU=1). */
static bool card_available(const card_desc_t* c) {
    if (strcmp(c->id, "loci_emu") == 0) return strcmp(loci_emu_backend_name(), "emul") == 0;
    return true;
}

static const card_desc_t* g_list[ALL_MAX];
static int g_n = -1;

static void build_list(void) {
    if (g_n >= 0) return;
    build_all();
    g_n = 0;
    for (int i = 0; i < g_all_n; i++)
        if (card_available(g_all[i])) g_list[g_n++] = g_all[i];
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
        for (int p = 0; p < d->nparams; p++) set_value(&st->card[i], p, d->param[p].def);
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
    if (strncmp(opt, "--loci-emu", 10) == 0 || strcmp(opt, "--loci-usb-image") == 0 ||
        strcmp(opt, "--loci-cdc") == 0)
        return "loci_emu";
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
                /* Une option propre à la carte l'active (--loci-sdimg implique --loci). */
                if (strcmp(d->id, "loci") == 0) cards_set_on(st, i, true);
            }
        }
    }
    /* 2. Machine en cours : ce que phosphoric.cfg a ajouté (interface disque). */
    if (emu) {
        int i;
        if (emu->has_microdisc && (i = cards_find("microdisc")) >= 0) {
            cards_set_on(st, i, true);
            if (emu->diskrom_path) set_value(&st->card[i], 0, emu->diskrom_path);
        }
        if (emu->has_jasmin && (i = cards_find("jasmin")) >= 0) {
            cards_set_on(st, i, true);
            if (emu->jasmin_rom_path) set_value(&st->card[i], 0, emu->jasmin_rom_path);
        }
        if (emu->has_loci && !emu->loci_external && (i = cards_find("loci")) >= 0) {
            cards_set_on(st, i, true);
            const char* rom = emu->rom_path ? emu->rom_path : "";
            set_value(&st->card[i], 0, strstr(rom, "locirom") ? "oui" : "non");
        }
    }
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
                snprintf(out, outsz, "%s et %s se chevauchent en $%04X",
                         cards_get(j)->name, d->name, lo[i] > lo[j] ? lo[i] : lo[j]);
                return true;
            }
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
    const bool loci_menu = iloci >= 0 && st->card[iloci].on &&
                           strcmp(st->card[iloci].value[0], "oui") == 0;
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
            const bool keep = i >= 0 && st->card[i].on;
            if (keep) push(&av, &n, &cap, argv[a]);
            if (takes) { a++; if (keep) push(&av, &n, &cap, argv[a]); }
            continue;
        }
        if (rom && a + 1 < argc) {
            const char* r = argv[a + 1];
            a++;
            if (loci_menu) continue;                       /* remplacée ci-dessous */
            push(&av, &n, &cap, "-r");
            /* LOCI éteinte : sa ROM de menu n'a plus de sens → BASIC 1.1. */
            push(&av, &n, &cap, strstr(r, "locirom") && (iloci < 0 || !st->card[iloci].on)
                                ? basic_rom : r);
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
        if (d->enable_param < 0) push(&av, &n, &cap, d->enable_cli);
        for (int p = 0; p < d->nparams; p++) {
            const card_param_t* pp = &d->param[p];
            if (!pp->cli) continue;
            if (p == d->enable_param) {
                push(&av, &n, &cap, pp->cli);
                push(&av, &n, &cap, c->value[p]);
            } else if (pp->kind == CARD_P_BOOL) {
                if (strcmp(c->value[p], "oui") == 0) push(&av, &n, &cap, pp->cli);
            } else if (c->value[p][0] && strcmp(c->value[p], pp->def) != 0) {
                push(&av, &n, &cap, pp->cli);
                push(&av, &n, &cap, c->value[p]);
            }
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

bool cards_cfg_line(cards_state_t* st, const char* key, const char* val, bool* card_seen) {
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
            if (st->card[i].value[p][0] && strcmp(st->card[i].value[p], d->param[p].def) != 0)
                fprintf(f, "%s.%s=%s\n", d->id, d->param[p].key, st->card[i].value[p]);
    }
}

bool cards_cfg_key(const char* key, size_t keylen) {
    char k[64];
    if (keylen >= sizeof(k)) return false;
    memcpy(k, key, keylen);
    k[keylen] = '\0';
    if (strncmp(k, "carte.", 6) == 0) return cards_find(k + 6) >= 0;
    char* dot = strchr(k, '.');
    if (!dot) return false;
    *dot = '\0';
    return cards_find(k) >= 0;
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
        if (cards_cfg_line(st, key, val, &card_seen) && card_seen)
            seen[cards_find(key + 6)] = true;
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
