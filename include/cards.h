/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cards.h
 * @brief Expansion card registry (F1 menu): description, explained
 *        parameters, launch options, state and cold restart
 * @author bmarty <bmarty@mailo.com>
 *
 * The F1 menu shows this registry's list as is (dynamic list): a card added
 * here appears there, a card missing from the build (LOCI backend, MIDI…)
 * does not. Each card describes its parameters (type, explanation,
 * command-line option); changing cards relaunches the emulator (cold
 * restart) with rebuilt options.
 */
#ifndef CARDS_H
#define CARDS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct emulator_s emulator_t;

#define CARD_PARAMS_MAX 5
#define CARD_VALUE_MAX  192

typedef enum {
    CARD_P_FILE,     /* host file (ROM, image): file browser */
    CARD_P_DIR,      /* host directory */
    CARD_P_TEXT,     /* free text (transport, number) */
    CARD_P_HEX,      /* hexadecimal I/O address ($0300-$03FF) */
    CARD_P_BOOL,     /* yes / no */
    CARD_P_CHOICE    /* one value among those of `def` (« a|b|c », default: a) */
} card_param_kind_t;

typedef struct {
    const char*       key;     /* configuration key (« rom », « adresse »…) */
    const char*       label;   /* displayed label */
    card_param_kind_t kind;
    const char*       cli;     /* launch option (NULL: handled separately) */
    const char*       def;     /* default value ("": none); CARD_P_CHOICE:
                                  the possible values, the first one by default */
    const char*       help;    /* explanation shown under the parameter */
} card_param_t;

typedef struct {
    const char*  id;           /* configuration identifier (« microdisc »…) */
    const char*  name;         /* displayed name */
    const char*  role;         /* what the card is for (one or two sentences) */
    const char*  group;        /* exclusive group (« disque », « midi ») or NULL */
    const char*  enable_cli;   /* option that enables the card */
    int          enable_param; /* parameter passed to enable_cli (-1: plain flag) */
    int          io_param;     /* parameter holding the I/O address (-1: fixed) */
    unsigned     io_base;      /* fixed address if io_param < 0 (0: no I/O) */
    unsigned     io_size;      /* number of addresses used */
    bool         fixed;        /* always present (display only) */
    card_param_t param[CARD_PARAMS_MAX];
    int          nparams;
} card_desc_t;

/* Registry (cards available in this build). */
int                cards_count(void);
const card_desc_t* cards_get(int i);
int                cards_find(const char* id);

/* Card choices: one entry per registry card. */
typedef struct {
    bool on;
    char value[CARD_PARAMS_MAX][CARD_VALUE_MAX];
} card_choice_t;

typedef struct {
    card_choice_t card[16];
} cards_state_t;

/* Default state: everything off, parameters at their default value. */
void cards_state_defaults(cards_state_t* st);
/* State of the running machine: launch options (@p argv) and machine
 * (cards enabled by phosphoric.cfg). */
void cards_state_from(cards_state_t* st, const emulator_t* emu, int argc, char* const argv[]);
/* Turns a card on / off, honouring its exclusive group. */
void cards_set_on(cards_state_t* st, int i, bool on);

/* Default value of @p p in @p out (for CARD_P_CHOICE: its first choice). */
void cards_param_default(const card_param_t* p, char* out, size_t outsz);
/* Next value of a CARD_P_CHOICE parameter (after the last one: the first). */
void cards_choice_next(const card_param_t* p, char* value, size_t valuesz);

/* Modem of the LOCI card (« modem » parameter): none, simulated picowifi, or
 * real picowifi plugged into the host (« port » port, empty: USB detection). */
#define LOCI_MODEM_NONE "aucun"
#define LOCI_MODEM_SIM  "simulé"
#define LOCI_MODEM_REAL "réel"

/* I/O address conflicts between active cards: message in @p out
 * (« Mageco MIDI et MEA8000 se chevauchent en $03FE »), false if none.
 * Also reports a real LOCI modem that cannot be found (no picowifi plugged in). */
bool cards_conflict(const cards_state_t* st, char* out, size_t outsz);

/* Launch options to restart with @p st: those of @p argv without any card
 * option, then those of the active cards, then --no-config-cards (the
 * configuration file must not turn a disabled card back on). NULL-terminated
 * array, allocated (cards_argv_free). */
char** cards_build_argv(const cards_state_t* st, int argc, char* const argv[], int* out_argc);
void   cards_argv_free(char** av);
/* Replaces the process with the emulator relaunched with @p av (cold
 * restart). Only returns on failure (or in the web build: unsupported). */
void   cards_exec(char** av);

/* phosphoric.cfg: « carte.<id>=oui|non » and « <id>.<key>=value ». */
bool cards_cfg_line(cards_state_t* st, const char* key, const char* val, bool* card_seen);
void cards_cfg_write(const cards_state_t* st, void* file /* FILE* */);
/* Reads the cards from a configuration file; @p seen[i]: the file states
 * explicitly whether card i is present. false if the file is missing. */
bool cards_cfg_read(const char* path, cards_state_t* st, bool seen[16]);
/* Configuration key belonging to the cards (« carte.x », « x.param »). */
bool cards_cfg_key(const char* key, size_t keylen);
/* Cards mentioned on the command line (enabling, parameter or secondary
 * option): the configuration file leaves those alone. */
void cards_cli_mentioned(int argc, char* const argv[], bool mentioned[16]);
/* Options to add at launch for the cards that @p path enables and that the
 * command line does not mention (nor any card of their group): NULL-terminated
 * list whose element 0 is argv[0], NULL if there is nothing to add. The
 * strings must stay valid (cli_opts_t keeps them). */
char** cards_config_argv(const char* path, int argc, char* const argv[], int* out_argc);

#endif /* CARDS_H */
