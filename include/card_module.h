/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_module.h
 * @brief Cartes d'extension en modules : un fichier par carte, une ligne dans
 *        la liste (include/cards_list.h). ADR 0006, docs/specs/CARD_MODULES.md.
 * @author bmarty <bmarty@mailo.com>
 *
 * Un module réunit tout ce qu'une carte apporte : sa fiche du menu F1, ses
 * options de lancement (et leur aide), sa configuration, sa mise en route, son
 * contrat de bus (io_device_t) et sa source audio. Le cœur en dérive la table
 * getopt, l'aide, le menu, le bus et le mixage.
 *
 * Ordres : les cartes migrées gardent leur place historique grâce à des ancres
 * (« avant telle option / telle carte / tel périphérique »), pour que l'aide,
 * les messages d'options ambiguës, le menu et l'ordre des sections .ost restent
 * identiques à l'octet. Une nouvelle carte peut laisser ses ancres à NULL : elle
 * va alors en fin de liste. Les ticks des modules suivent ceux du cœur, dans
 * l'ordre de la liste (ADR 0003), par appels directs (card_ticks.h).
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

/* Option de lancement d'une carte. `set` reçoit la configuration de la carte et
 * l'argument (NULL pour une option sans argument). */
typedef struct {
    const char* name;      /* sans « -- » */
    int         has_arg;   /* no_argument / required_argument / optional_argument */
    void      (*set)(void* cfg, const char* arg);
} card_opt_t;

/* Bloc d'aide d'une carte, placé juste avant l'aide de l'option @p before
 * (NULL : en fin de liste des options). Une carte peut en avoir plusieurs quand
 * ses options sont dispersées dans l'aide (ACIA). */
typedef struct {
    const char* text;      /* lignes d'aide, « \n » compris */
    const char* before;
} card_help_t;

/* Étapes de mise en route où le cœur appelle les cartes (dans l'ordre de la
 * liste), à la place exacte qu'occupait leur code dans main.c. */
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
    const char*        opts_before;  /* option (sans --) qu'elles précèdent dans la table */
    const card_help_t* helps;        /* blocs d'aide (au moins un si la carte a des options) */
    int                nhelps;

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

    const io_device_t* bus;          /* NULL : pas de registres d'E/S */
    const char*        bus_before;   /* nom du périphérique qu'elle précède sur le bus */
    /* Le son éventuel est une source audio enregistrée par setup
     * (audio_add_source, audio/audio.h). */
} card_module_t;

/* Liste des modules, générée depuis cards_list.h (card_modules.c). */
extern const card_module_t* const k_card_modules[];
extern const int k_card_module_count;

/* Codes getopt des options de modules : CARD_OPT_BASE + module * 64 + option. */
#define CARD_OPT_BASE 0x4000

/* Configuration de chaque module, allouée et mise à ses valeurs par défaut. */
void** card_modules_cfg_new(void);
/* Applique l'option de code @p code (>= CARD_OPT_BASE) ; false si inconnue. */
bool   card_modules_set_option(void** cfgs, int code, const char* arg);
/* Options de la table getopt @p core (terminée par {0}) augmentées de celles des
 * modules, insérées avant leur ancre. Allouée une fois, gardée. */
const struct option* card_modules_long_options(const struct option* core);
/* Aide : écrit les blocs des modules ancrés avant l'option @p name ; NULL écrit
 * ceux qui n'ont pas d'ancre (fin de la liste des options). */
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
