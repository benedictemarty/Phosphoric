/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file iomenu.h
 * @brief Input/output peripherals menu (F1), full screen.
 * @author bmarty <bmarty@mailo.com>
 *
 * Inspired by the OSD menu of Neo6502TeleStrat (src/osd/osd.h, osd_menu.h,
 * zlib/libpng licence): same layout (header, dithered panels with rounded
 * frames, selection bar, buttons, key help), same navigation and same modal
 * file picker, adapted to the Oric's peripherals.
 *
 * The module knows neither SDL nor emulator_t: the caller fills the state
 * (iom_state_t), passes abstract key codes (IOM_KEY_*) and executes the
 * returned actions (iom_action_t). Drawing happens on a grid of
 * 80 × 40 cells of 8 × 8 (font iom_font.h), rendered as RGB888 640 × 640: each
 * pixel line is doubled, like the Neo6502's 960 × 544 output. Testable
 * without a screen (tests/unit/test_iomenu.c).
 */
#ifndef IOMENU_H
#define IOMENU_H

#include <stdbool.h>
#include <stdint.h>
#include "cards.h"

#define IOM_COLS   80
#define IOM_ROWS   40
#define IOM_WIDTH  (IOM_COLS * 8)        /* 640 pixels */
#define IOM_HEIGHT (IOM_ROWS * 8 * 2)    /* 640 lines (doubled lines) */

/* Colours (bit 0 red, 1 green, 2 blue), as on the Oric. */
enum { IOM_BLACK = 0, IOM_RED, IOM_GREEN, IOM_YELLOW, IOM_BLUE, IOM_MAGENTA, IOM_CYAN, IOM_WHITE };
#define IOM_DITHER 0x08   /* dithered paper: paper colour on every other pixel */
/* Attribute: bits 0-2 ink, bits 4-6 paper, bit 7 dithered paper. */
#define IOM_ATTR(ink, paper) ((uint8_t)((ink) | (((paper) & 7) << 4) | (((paper) & IOM_DITHER) ? 0x80 : 0)))

typedef struct {
    uint8_t ch[IOM_ROWS][IOM_COLS];
    uint8_t attr[IOM_ROWS][IOM_COLS];
    uint8_t big[IOM_ROWS][IOM_COLS];   /* large letters: 1 = left half, 2 = right */
} iom_surface_t;

/* ── Main page items ───────────────────────────────────────────────────── */
enum {
    IOM_ITEM_DRIVE0 = 0,       /* 0-3: drives A-D */
    IOM_ITEM_TAPE = 4,
    IOM_ITEM_CARDS,            /* cards panel: Enter → cards page */
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

/* Abstract key codes (the caller translates SDL). Letters are passed
 * as they are (' ' < code < 0x100): jump to initial in the file picker. */
enum {
    IOM_KEY_UP = 0x101, IOM_KEY_DOWN, IOM_KEY_LEFT, IOM_KEY_RIGHT, IOM_KEY_ENTER,
    IOM_KEY_ESC, IOM_KEY_DEL, IOM_KEY_HOME, IOM_KEY_END, IOM_KEY_PGUP, IOM_KEY_PGDN
};

typedef enum {
    IOM_ACT_NONE = 0,
    IOM_ACT_DISK_INSERT,       /* target = drive, path */
    IOM_ACT_DISK_EJECT,        /* target = drive */
    IOM_ACT_DISK_PROTECT,      /* target = drive: toggles protection */
    IOM_ACT_TAPE_INSERT,       /* path */
    IOM_ACT_TAPE_EJECT,
    IOM_ACT_TAPE_REWIND,
    IOM_ACT_SNAPSHOT_SAVE,     /* new snapshot */
    IOM_ACT_SNAPSHOT_LOAD,     /* path */
    IOM_ACT_PRINTER_CYCLE,     /* off → text → MCP-40 → off */
    IOM_ACT_JOYSTICK_CYCLE,    /* none → keyboard → gamepad → none */
    IOM_ACT_KEYBOARD_TOGGLE,   /* QWERTY ↔ AZERTY */
    IOM_ACT_TAPE_FAST_TOGGLE,  /* fast loading ↔ real speed */
    IOM_ACT_RESET,
    IOM_ACT_SAVE_CONFIG,
    IOM_ACT_RESUME,
    IOM_ACT_CARDS_APPLY        /* restart with the chosen cards (m->cards) */
} iom_act_type_t;

#define IOM_PATH_MAX 256
#define IOM_NAME_MAX 64

typedef struct {
    iom_act_type_t type;
    int  target;
    char path[IOM_PATH_MAX];
} iom_action_t;

/* ── Displayed state, filled by the caller (iomenu_glue.c) ─────────────── */
#define IOM_CARDS 12

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
    const char* disk_iface;    /* « Microdisc », « Jasmin », « LOCI »; NULL: no drive */
    bool no_drive_protect;     /* the interface has no per-drive write-protect (LOCI) */
    int  drives;               /* usable drives (0 without interface) */
    char drive[4][IOM_NAME_MAX];   /* "": empty */
    bool drive_ro[4];
    char tape[IOM_NAME_MAX];   /* "": no cassette */
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

typedef enum { IOM_FILE_DSK, IOM_FILE_TAP, IOM_FILE_OST, IOM_FILE_ANY } iom_file_kind_t;

/* Menu pages. */
enum { IOM_PAGE_MAIN = 0, IOM_PAGE_CARDS, IOM_PAGE_CARD };
/* browse_target of the file browser opened for a card parameter. */
#define IOM_BROWSE_CARD 100

typedef struct {
    char name[IOM_NAME_MAX];
    char path[IOM_PATH_MAX];
    uint32_t size;
} iom_file_t;

typedef struct {
    bool open;
    iom_state_t st;
    int  cursor;               /* IOM_ITEM_* item */
    int  sub;                  /* column: 0 = media, 1 = protection / rewind */
    /* File picker (modal). */
    bool browsing;
    int  browse_target;        /* item that opened it */
    int  browse_cursor;        /* 0 = special action (eject / new snapshot) */
    int  browse_scroll;
    iom_file_t files[IOM_FILES];
    int  nfiles;
    const char* const* dirs;   /* directories browsed (NULL: default directories) */
    char message[96];
    bool message_error;
    /* Expansion cards: dynamic list from the registry (cards.h), choices
     * being edited, applied by restarting (IOM_ACT_CARDS_APPLY). */
    int  page;                 /* IOM_PAGE_* */
    int  card_cursor;          /* cards page: card, then 2 buttons */
    int  card_sel;             /* card page: card shown */
    int  param_cursor;         /* card page: 0 = presence, 1.. = parameters */
    cards_state_t cards;       /* current choices */
    cards_state_t cards_orig;  /* running machine (Cancel) */
    bool cards_readonly;       /* web build: no restart */
    bool editing;              /* editing a text parameter */
    char edit[CARD_VALUE_MAX];
} iom_menu_t;

void iom_init(iom_menu_t* m);
void iom_open(iom_menu_t* m);
void iom_close(iom_menu_t* m);
/* Directories browsed by the file picker (NULL-terminated list, kept by
 * reference). Default: tapes, disks, snapshots, demos/ula-ng, current directory. */
void iom_set_dirs(iom_menu_t* m, const char* const* dirs);
/* Handles a key; returns the action to execute (IOM_ACT_NONE otherwise). */
iom_action_t iom_key(iom_menu_t* m, int key);
void iom_message(iom_menu_t* m, bool error, const char* text);
/* Text typed (UTF-8) while editing a card parameter (m->editing). */
void iom_text(iom_menu_t* m, const char* utf8);

/* Drawing: grid, then RGB888 IOM_WIDTH × IOM_HEIGHT. */
void iom_draw(const iom_menu_t* m, iom_surface_t* s);
void iom_rasterize(const iom_surface_t* s, uint8_t* rgb);

/* Surface helper (exposed for the tests): UTF-8 text → cells. */
int  iom_puts(iom_surface_t* s, int row, int col, const char* str, uint8_t attr, int max);

#endif /* IOMENU_H */
