/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cards.h
 * @brief Registre des cartes d'extension (menu F1) : description, paramètres
 *        expliqués, options de lancement, état et relance à froid
 * @author bmarty <bmarty@mailo.com>
 *
 * Le menu F1 affiche la liste de ce registre telle quelle (liste dynamique) :
 * une carte ajoutée ici y apparaît, une carte absente de la build (backend
 * LOCI, MIDI…) n'y figure pas. Chaque carte décrit ses paramètres (type,
 * explication, option de la ligne de commande) ; changer de cartes relance
 * l'émulateur (redémarrage à froid) avec des options reconstruites.
 */
#ifndef CARDS_H
#define CARDS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct emulator_s emulator_t;

#define CARD_PARAMS_MAX 5
#define CARD_VALUE_MAX  192

typedef enum {
    CARD_P_FILE,     /* fichier de l'hôte (ROM, image) : sélecteur de fichiers */
    CARD_P_DIR,      /* dossier de l'hôte */
    CARD_P_TEXT,     /* texte libre (transport, nombre) */
    CARD_P_HEX,      /* adresse d'E/S hexadécimale ($0300-$03FF) */
    CARD_P_BOOL      /* oui / non */
} card_param_kind_t;

typedef struct {
    const char*       key;     /* clé de configuration (« rom », « adresse »…) */
    const char*       label;   /* libellé affiché */
    card_param_kind_t kind;
    const char*       cli;     /* option de lancement (NULL : traitée à part) */
    const char*       def;     /* valeur par défaut ("" : aucune) */
    const char*       help;    /* explication affichée sous le paramètre */
} card_param_t;

typedef struct {
    const char*  id;           /* identifiant de configuration (« microdisc »…) */
    const char*  name;         /* nom affiché */
    const char*  role;         /* à quoi sert la carte (une ou deux phrases) */
    const char*  group;        /* groupe exclusif (« disque », « midi ») ou NULL */
    const char*  enable_cli;   /* option qui active la carte */
    int          enable_param; /* paramètre passé à enable_cli (-1 : simple drapeau) */
    int          io_param;     /* paramètre portant l'adresse d'E/S (-1 : fixe) */
    unsigned     io_base;      /* adresse fixe si io_param < 0 (0 : pas d'E/S) */
    unsigned     io_size;      /* nombre d'adresses occupées */
    bool         fixed;        /* toujours présente (affichage seul) */
    card_param_t param[CARD_PARAMS_MAX];
    int          nparams;
} card_desc_t;

/* Registre (cartes disponibles dans cette build). */
int                cards_count(void);
const card_desc_t* cards_get(int i);
int                cards_find(const char* id);

/* Choix de cartes : une ligne par carte du registre. */
typedef struct {
    bool on;
    char value[CARD_PARAMS_MAX][CARD_VALUE_MAX];
} card_choice_t;

typedef struct {
    card_choice_t card[16];
} cards_state_t;

/* État par défaut : tout éteint, paramètres à leur valeur par défaut. */
void cards_state_defaults(cards_state_t* st);
/* État de la machine en cours : options de lancement (@p argv) et machine
 * (cartes activées par phosphoric.cfg). */
void cards_state_from(cards_state_t* st, const emulator_t* emu, int argc, char* const argv[]);
/* Active / éteint une carte en respectant son groupe exclusif. */
void cards_set_on(cards_state_t* st, int i, bool on);

/* Conflits d'adresses d'E/S entre cartes actives : message dans @p out
 * (« Mageco MIDI et MEA8000 se chevauchent en $03FE »), false si aucun. */
bool cards_conflict(const cards_state_t* st, char* out, size_t outsz);

/* Options de lancement pour relancer avec @p st : celles d'@p argv sans aucune
 * option de carte, puis celles des cartes actives, puis --no-config-cards (le
 * fichier de configuration ne doit pas rallumer une carte éteinte). Tableau
 * terminé par NULL, alloué (cards_argv_free). */
char** cards_build_argv(const cards_state_t* st, int argc, char* const argv[], int* out_argc);
void   cards_argv_free(char** av);
/* Remplace le processus par l'émulateur relancé avec @p av (redémarrage à
 * froid). Ne revient qu'en cas d'échec (ou dans la version web : non géré). */
void   cards_exec(char** av);

/* phosphoric.cfg : « carte.<id>=oui|non » et « <id>.<clé>=valeur ». */
bool cards_cfg_line(cards_state_t* st, const char* key, const char* val, bool* card_seen);
void cards_cfg_write(const cards_state_t* st, void* file /* FILE* */);
/* Lit les cartes d'un fichier de configuration ; @p seen[i] : le fichier dit
 * explicitement si la carte i est présente. false si le fichier est absent. */
bool cards_cfg_read(const char* path, cards_state_t* st, bool seen[16]);
/* Clé de configuration appartenant aux cartes (« carte.x », « x.param »). */
bool cards_cfg_key(const char* key, size_t keylen);
/* Cartes que la ligne de commande mentionne (activation, paramètre ou option
 * secondaire) : celles-là, le fichier de configuration n'y touche pas. */
void cards_cli_mentioned(int argc, char* const argv[], bool mentioned[16]);
/* Options à ajouter au lancement pour les cartes que @p path active et que la
 * ligne de commande ne mentionne pas (ni aucune carte de leur groupe) : liste
 * terminée par NULL dont l'élément 0 est argv[0], NULL s'il n'y a rien à
 * ajouter. Les chaînes doivent rester valides (cli_opts_t les garde). */
char** cards_config_argv(const char* path, int argc, char* const argv[], int* out_argc);

#endif /* CARDS_H */
