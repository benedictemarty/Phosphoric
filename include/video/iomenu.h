/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file iomenu.h
 * @brief Menu des périphériques d'entrée/sortie (F1), plein écran.
 * @author bmarty <bmarty@mailo.com>
 *
 * Inspiré du menu OSD de Neo6502TeleStrat (src/osd/osd.h, osd_menu.h, licence
 * zlib/libpng) : même présentation (en-tête, panneaux tramés à cadre arrondi,
 * barre de sélection, boutons, aide des touches), même navigation et même
 * sélecteur de fichiers modal, adaptés aux périphériques de l'Oric.
 *
 * Le module ne connaît ni SDL ni emulator_t : l'appelant remplit l'état
 * (iom_state_t), transmet des codes touches abstraits (IOM_KEY_*) et exécute les
 * actions renvoyées (iom_action_t). Le dessin se fait sur une grille de
 * 80 × 40 cellules 8 × 8 (police iom_font.h), rendue en RGB888 640 × 640 : chaque
 * ligne de pixels est doublée, comme la sortie 960 × 544 du Neo6502. Testable
 * sans écran (tests/unit/test_iomenu.c).
 */
#ifndef IOMENU_H
#define IOMENU_H

#include <stdbool.h>
#include <stdint.h>

#define IOM_COLS   80
#define IOM_ROWS   40
#define IOM_WIDTH  (IOM_COLS * 8)        /* 640 pixels */
#define IOM_HEIGHT (IOM_ROWS * 8 * 2)    /* 640 lignes (lignes doublées) */

/* Couleurs (bit 0 rouge, 1 vert, 2 bleu), comme l'Oric. */
enum { IOM_BLACK = 0, IOM_RED, IOM_GREEN, IOM_YELLOW, IOM_BLUE, IOM_MAGENTA, IOM_CYAN, IOM_WHITE };
#define IOM_DITHER 0x08   /* fond tramé : la couleur de fond un pixel sur deux */
/* Attribut : bits 0-2 encre, bits 4-6 fond, bit 7 fond tramé. */
#define IOM_ATTR(ink, paper) ((uint8_t)((ink) | (((paper) & 7) << 4) | (((paper) & IOM_DITHER) ? 0x80 : 0)))

typedef struct {
    uint8_t ch[IOM_ROWS][IOM_COLS];
    uint8_t attr[IOM_ROWS][IOM_COLS];
    uint8_t big[IOM_ROWS][IOM_COLS];   /* grandes lettres : 1 = moitié gauche, 2 = droite */
} iom_surface_t;

/* ── Éléments de la page principale ────────────────────────────────────── */
enum {
    IOM_ITEM_DRIVE0 = 0,       /* 0-3 : lecteurs A-D */
    IOM_ITEM_TAPE = 4,
    IOM_ITEM_SNAPSHOT,
    IOM_ITEM_PRINTER,
    IOM_ITEM_JOYSTICK,
    IOM_ITEM_KEYBOARD,
    IOM_ITEM_TAPE_FAST,
    IOM_ITEM_RESET,
    IOM_ITEM_SAVE,
    IOM_ITEM_RESUME,
    IOM_ITEMS
};

/* Codes touches abstraits (l'appelant traduit SDL). Les lettres sont passées
 * telles quelles (' ' < code < 0x100) : saut à l'initiale dans le sélecteur. */
enum {
    IOM_KEY_UP = 0x101, IOM_KEY_DOWN, IOM_KEY_LEFT, IOM_KEY_RIGHT, IOM_KEY_ENTER,
    IOM_KEY_ESC, IOM_KEY_DEL, IOM_KEY_HOME, IOM_KEY_END, IOM_KEY_PGUP, IOM_KEY_PGDN
};

typedef enum {
    IOM_ACT_NONE = 0,
    IOM_ACT_DISK_INSERT,       /* target = lecteur, path */
    IOM_ACT_DISK_EJECT,        /* target = lecteur */
    IOM_ACT_DISK_PROTECT,      /* target = lecteur : bascule la protection */
    IOM_ACT_TAPE_INSERT,       /* path */
    IOM_ACT_TAPE_EJECT,
    IOM_ACT_TAPE_REWIND,
    IOM_ACT_SNAPSHOT_SAVE,     /* nouvel instantané */
    IOM_ACT_SNAPSHOT_LOAD,     /* path */
    IOM_ACT_PRINTER_CYCLE,     /* coupée → texte → MCP-40 → coupée */
    IOM_ACT_JOYSTICK_CYCLE,    /* aucun → clavier → manette → aucun */
    IOM_ACT_KEYBOARD_TOGGLE,   /* QWERTY ↔ AZERTY */
    IOM_ACT_TAPE_FAST_TOGGLE,  /* chargement rapide ↔ vitesse réelle */
    IOM_ACT_RESET,
    IOM_ACT_SAVE_CONFIG,
    IOM_ACT_RESUME
} iom_act_type_t;

#define IOM_PATH_MAX 256
#define IOM_NAME_MAX 64

typedef struct {
    iom_act_type_t type;
    int  target;
    char path[IOM_PATH_MAX];
} iom_action_t;

/* ── État affiché, rempli par l'appelant (iomenu_glue.c) ───────────────── */
#define IOM_CARDS 10

typedef struct {
    char name[24];             /* « Microdisc », « ACIA 6551 »… */
    bool present;
    char detail[40];           /* « $0310  microdis.rom », « $0380  picowifi »… */
} iom_card_t;

typedef enum { IOM_PRINTER_OFF = 0, IOM_PRINTER_TEXT, IOM_PRINTER_MCP40 } iom_printer_t;
typedef enum { IOM_JOY_NONE = 0, IOM_JOY_KEYS, IOM_JOY_GAMEPAD } iom_joy_t;

typedef struct {
    const char* version;       /* « 2.4.0 » */
    const char* machine;       /* « Oric Atmos », « Oric-1 » */
    const char* disk_iface;    /* « Microdisc », « Jasmin » ; NULL : pas de lecteur */
    int  drives;               /* lecteurs utilisables (0 sans interface) */
    char drive[4][IOM_NAME_MAX];   /* "" : vide */
    bool drive_ro[4];
    char tape[IOM_NAME_MAX];   /* "" : pas de cassette */
    int  tape_percent;
    bool tape_motor;
    bool tape_fast;
    iom_printer_t printer;
    char printer_file[IOM_NAME_MAX];
    iom_joy_t joystick;
    bool azerty;
    char snapshot_last[IOM_NAME_MAX];
    iom_card_t card[IOM_CARDS];
    int  cards;
} iom_state_t;

/* ── Menu ───────────────────────────────────────────────────────────────── */
#define IOM_FILES          256
#define IOM_BROWSE_VISIBLE 20

typedef enum { IOM_FILE_DSK, IOM_FILE_TAP, IOM_FILE_OST } iom_file_kind_t;

typedef struct {
    char name[IOM_NAME_MAX];
    char path[IOM_PATH_MAX];
    uint32_t size;
} iom_file_t;

typedef struct {
    bool open;
    iom_state_t st;
    int  cursor;               /* élément IOM_ITEM_* */
    int  sub;                  /* colonne : 0 = média, 1 = protection / rembobinage */
    /* Sélecteur de fichiers (modal). */
    bool browsing;
    int  browse_target;        /* élément qui l'a ouvert */
    int  browse_cursor;        /* 0 = action spéciale (éjecter / nouvel instantané) */
    int  browse_scroll;
    iom_file_t files[IOM_FILES];
    int  nfiles;
    const char* const* dirs;   /* dossiers explorés (NULL : dossiers par défaut) */
    char message[96];
    bool message_error;
} iom_menu_t;

void iom_init(iom_menu_t* m);
void iom_open(iom_menu_t* m);
void iom_close(iom_menu_t* m);
/* Dossiers explorés par le sélecteur (liste terminée par NULL, gardée par
 * référence). Défaut : tapes, disks, snapshots, demos/ula-ng, dossier courant. */
void iom_set_dirs(iom_menu_t* m, const char* const* dirs);
/* Traite une touche ; renvoie l'action à exécuter (IOM_ACT_NONE sinon). */
iom_action_t iom_key(iom_menu_t* m, int key);
void iom_message(iom_menu_t* m, bool error, const char* text);

/* Dessin : grille, puis RGB888 IOM_WIDTH × IOM_HEIGHT. */
void iom_draw(const iom_menu_t* m, iom_surface_t* s);
void iom_rasterize(const iom_surface_t* s, uint8_t* rgb);

/* Utilitaire de surface (exposé pour les tests) : texte UTF-8 → cellules. */
int  iom_puts(iom_surface_t* s, int row, int col, const char* str, uint8_t attr, int max);

#endif /* IOMENU_H */
