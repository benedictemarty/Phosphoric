/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_module.h
 * @brief Expansion cards as modules: one file per card, one line in the
 *        list (include/cards_list.h). ADR 0006, docs/specs/CARD_MODULES.md.
 * @author bmarty <bmarty@mailo.com>
 *
 * A module gathers everything a card brings: its F1 menu entry, its
 * launch options (and their help), its configuration, its setup, its bus
 * contract (io_device_t) and its audio source. The core derives from them the
 * getopt table, the help, the menu, the bus and the mixing.
 *
 * Ordering: migrated cards keep their historical place thanks to anchors
 * (« before such option / such card / such device »), so that the help,
 * the ambiguous-option messages, the menu and the order of the .ost sections
 * stay byte-identical. A new card may leave its anchors NULL: it then goes
 * to the end of the list. The modules' ticks follow those of the core, in
 * list order (ADR 0003), as direct calls (card_ticks.h).
 */
#ifndef CARD_MODULE_H
#define CARD_MODULE_H

#include <getopt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cards.h"
#include "io/io_device.h"

typedef struct emulator_s emulator_t;
struct cli_opts_s;                     /* options du cœur (cli/cli_opts.h) */

/* Launch option of a card. `set` receives the card's configuration and the
 * argument (NULL for an option without argument). */
typedef struct {
    const char* name;      /* without « -- » */
    int         has_arg;   /* no_argument / required_argument / optional_argument */
    void      (*set)(void* cfg, const char* arg);
} card_opt_t;

/* Setup stages where the core calls the cards (in list order), at the
 * exact place their code used to occupy in main.c. */
typedef enum {
    CARD_STAGE_MACHINE,    /* main_setup_machine, juste après emulator_init */
    CARD_STAGE_SERIAL,     /* main_setup_serial_cards, après l'ACIA 6551 */
    CARD_STAGE_SPEECH,     /* main_setup_disks_speech, après le Jasmin */
    CARD_STAGE_COUNT
} card_stage_t;

typedef struct card_module_s {
    const card_desc_t* const* descs; /* fiche(s) du menu F1 (une puce, plusieurs cartes) */
    int                ndescs;
    const char*        desc_before;  /* id de la fiche qu'elles précèdent dans le menu */

    const card_opt_t*  opts;
    int                nopts;
    const char*        opts_before;  /* option (without --) they precede in the table */
    const char*        help;         /* help lines, « \n » included */
    const char*        help_before;  /* option whose help follows theirs */

    size_t             cfg_size;
    void             (*cfg_defaults)(void* cfg);

    /* Appelé par emulator_init pour chaque carte, présente ou non (état de
     * repos : adresse par défaut, lignes d'IRQ câblées). NULL : rien. */
    void             (*init)(emulator_t* emu);
    card_stage_t       stage;
    /* 0 : carte absente ou mise en route ; 1 : erreur (déjà journalisée), le
     * cœur nettoie l'émulateur et s'arrête. @p core : options du cœur
     * (--serial-trace…). */
    int              (*setup)(emulator_t* emu, const void* cfg, const struct cli_opts_s* core);
    /* Appelé par emulator_cleanup (fermeture des transports…). NULL : rien. */
    void             (*teardown)(emulator_t* emu);

    const io_device_t* bus;          /* NULL: no I/O registers */
    const char*        bus_before;   /* name of the device it precedes on the bus */
    /* Any sound is an audio source registered by setup
     * (audio_add_source, audio/audio.h). */
} card_module_t;

/* List of the modules, generated from cards_list.h (card_modules.c). */
extern const card_module_t* const k_card_modules[];
extern const int k_card_module_count;

/* getopt codes of the module options: CARD_OPT_BASE + module * 64 + option. */
#define CARD_OPT_BASE 0x4000

/* Configuration of each module, allocated and set to its default values. */
void** card_modules_cfg_new(void);
/* Applies the option with code @p code (>= CARD_OPT_BASE); false if unknown. */
bool   card_modules_set_option(void** cfgs, int code, const char* arg);
/* Options of the getopt table @p core (terminated by {0}) extended with those of
 * the modules, inserted before their anchor. Allocated once, kept. */
const struct option* card_modules_long_options(const struct option* core);
/* Help: writes the blocks of the modules anchored before option @p name; NULL
 * writes those without an anchor (end of the option list). */
void   card_modules_print_help_before(const char* name);
/* Crochets init / teardown de toutes les cartes, dans l'ordre de la liste. */
void   card_modules_init(emulator_t* emu);
void   card_modules_teardown(emulator_t* emu);
/* Mise en route des modules de l'étape @p stage ; 1 si l'une échoue. */
int    card_modules_setup(emulator_t* emu, const struct cli_opts_s* core, card_stage_t stage);
/* Vrai si @p opt (« --nom ») est une option de module sans argument. */
bool   card_modules_option_is_flag(const char* opt);
/* Placement par ancres (menu, bus) : émet les éléments du cœur (emit_core) en
 * plaçant avant chacun les éléments des modules ancrés sur lui (emit_module),
 * puis les modules sans ancre, puis ceux dont l'ancre est introuvable. */
void   card_modules_place(const char* const* core_names, int ncore,
                          const char* (*anchor)(int m), int (*count)(int m),
                          const char* (*key)(int m, int j),
                          void (*emit_module)(int m, int j, void* ctx),
                          void (*emit_core)(int i, void* ctx), void* ctx);

#endif /* CARD_MODULE_H */
