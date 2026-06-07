/**
 * @file keyboard.c
 * @brief ORIC-1 keyboard matrix emulation with SDL2 key mapping
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-beta.3
 *
 * The ORIC keyboard is an 8-column x 8-row matrix (64 keys).
 * Column select via VIA Port B bits 0-2 -> 74LS138 decoder.
 * Row data read via PSG Port A (register 14), active low.
 * ROM scans keyboard via VIA PB3 (set when a key matches).
 *
 * Two mapping modes:
 *
 * QWERTY (positional): Each SDL keycode maps directly to an ORIC matrix
 * position. Works well when the PC and ORIC keyboards have similar layout.
 *
 * AZERTY (symbolic): Uses SDL_TEXTINPUT to capture the actual character
 * typed, then maps it to the ORIC key combination that produces the same
 * character. Works for any PC keyboard layout (AZERTY, QWERTZ, etc.).
 * The character -> ORIC mapping is derived from the ORIC-1 BASIC 1.0 ROM
 * keyboard decode tables at $FF70 (unshifted) and $FFB0 (shifted).
 *
 * Key mapping tables based on Oricutron by Peter Gordon (GPL v2).
 */

#include "io/keyboard.h"
#include <string.h>

void oric_keyboard_init(oric_keyboard_t* kb) {
    memset(kb, 0, sizeof(*kb));
    memset(kb->matrix, 0xFF, sizeof(kb->matrix));
    kb->layout = ORIC_KB_QWERTY;
}

void oric_keyboard_reset(oric_keyboard_t* kb) {
    oric_kb_layout_t saved = kb->layout;
    memset(kb, 0, sizeof(*kb));
    memset(kb->matrix, 0xFF, sizeof(kb->matrix));
    kb->layout = saved;
}

void oric_keyboard_set_layout(oric_keyboard_t* kb, oric_kb_layout_t layout) {
    kb->layout = layout;
}

/* ================================================================
 * Character -> ORIC matrix mapping (used by both SDL and press_char)
 * ================================================================
 *
 * Derived from ROM keyboard tables at $FF70 (unshifted) and $FFB0 (shifted).
 * Each entry: { col, row, need_oric_shift }
 *   - col = hardware column (VIA ORB[0:2], 74LS138 output index)
 *   - row = hardware row    (PSG R14 bit index)
 *
 * Storage: matrix[col] has bit `row` cleared when (col, row) is pressed.
 * The literal values in the table below pre-date this clarification —
 * they were correct numerically but the field names were swapped.
 */

typedef struct {
    int8_t col, row;
    bool shift;
} char_entry_t;

#define U(c,r) {c, r, false}    /* Unshifted: just press this key */
#define S(c,r) {c, r, true}     /* Shifted: press this key + ORIC Shift */
#define X      {-1, -1, false}  /* Unmapped */

static const char_entry_t char_map[128] = {
    /* 0x00-0x1F: control characters - unmapped */
    X,X,X,X,X,X,X,X, X,X,X,X,X,X,X,X,
    X,X,X,X,X,X,X,X, X,X,X,X,X,X,X,X,
    /* 0x20 ' ' */ U(4,0),
    /* 0x21 '!' */ S(0,5),   /* Shift + 1 */
    /* 0x22 '"' */ S(3,7),   /* Shift + ' (apostrophe key) */
    /* 0x23 '#' */ S(0,7),   /* Shift + 3 */
    /* 0x24 '$' */ S(2,3),   /* Shift + 4 */
    /* 0x25 '%' */ S(0,2),   /* Shift + 5 */
    /* 0x26 '&' */ S(0,0),   /* Shift + 7 */
    /* 0x27 '\''*/ U(3,7),   /* ' (apostrophe key) */
    /* 0x28 '(' */ S(3,1),   /* Shift + 9 */
    /* 0x29 ')' */ S(7,2),   /* Shift + 0 */
    /* 0x2A '*' */ S(7,0),   /* Shift + 8 */
    /* 0x2B '+' */ S(7,7),   /* Shift + = */
    /* 0x2C ',' */ U(4,1),
    /* 0x2D '-' */ U(3,3),
    /* 0x2E '.' */ U(4,2),
    /* 0x2F '/' */ U(7,3),
    /* 0x30 '0' */ U(7,2),
    /* 0x31 '1' */ U(0,5),
    /* 0x32 '2' */ U(2,6),
    /* 0x33 '3' */ U(0,7),
    /* 0x34 '4' */ U(2,3),
    /* 0x35 '5' */ U(0,2),
    /* 0x36 '6' */ U(2,1),
    /* 0x37 '7' */ U(0,0),
    /* 0x38 '8' */ U(7,0),
    /* 0x39 '9' */ U(3,1),
    /* 0x3A ':' */ S(3,2),   /* Shift + ; */
    /* 0x3B ';' */ U(3,2),
    /* 0x3C '<' */ S(4,1),   /* Shift + , */
    /* 0x3D '=' */ U(7,7),
    /* 0x3E '>' */ S(4,2),   /* Shift + . */
    /* 0x3F '?' */ S(7,3),   /* Shift + / */
    /* 0x40 '@' */ S(2,6),   /* Shift + 2 */
    /* 0x41-0x5A: majuscules ASCII → même position matrice que minuscules (U, pas S).
     * La ROM BASIC active le SHIFT LOCK au démarrage : sans Shift physique ORIC,
     * ces touches produisent des majuscules à l'écran.
     * Si le SHIFT LOCK est désactivé par le programme, l'injection donne des
     * minuscules. press_char() ne contrôle pas l'état du SHIFT LOCK. */
    U(6,5),U(2,2),U(2,7),U(1,7),U(6,3),U(1,3),U(6,2),U(6,1),
    U(5,1),U(1,0),U(3,0),U(7,1),U(2,0),U(0,1),U(5,2),U(5,3),
    U(1,6),U(1,2),U(6,6),U(1,1),U(5,0),U(0,3),U(6,7),U(0,6),
    U(6,0),U(2,5),
    /* 0x5B '[' */ U(5,7),
    /* 0x5C '\\'*/ U(3,6),
    /* 0x5D ']' */ U(5,6),
    /* 0x5E '^' */ S(2,1),   /* Shift + 6 */
    /* 0x5F '_' */ S(3,3),   /* Shift + - */
    /* 0x60 '`' */ X,
    /* 0x61-0x7A: lowercase letters -> same as uppercase (ORIC has no case distinction in matrix) */
    U(6,5),U(2,2),U(2,7),U(1,7),U(6,3),U(1,3),U(6,2),U(6,1),
    U(5,1),U(1,0),U(3,0),U(7,1),U(2,0),U(0,1),U(5,2),U(5,3),
    U(1,6),U(1,2),U(6,6),U(1,1),U(5,0),U(0,3),U(6,7),U(0,6),
    U(6,0),U(2,5),
    /* 0x7B '{' */ S(5,7),
    /* 0x7C '|' */ S(3,6),
    /* 0x7D '}' */ S(5,6),
    /* 0x7E '~' */ X,
    /* 0x7F DEL */ X
};

/* ORIC Left Shift position (hardware col, hardware row) */
#define ORIC_LSHIFT_COL 4
#define ORIC_LSHIFT_ROW 4
/* RETURN key position (hardware col, hardware row) */
#define ORIC_RETURN_COL 7
#define ORIC_RETURN_ROW 5

#undef U
#undef S
#undef X

bool oric_keyboard_press_char(oric_keyboard_t* kb, char c) {
    if (c == '\n' || c == '\r') {
        /* RETURN key */
        kb->matrix[ORIC_RETURN_COL] &= ~(1 << ORIC_RETURN_ROW);
        return true;
    }
    if (c == 0x1B) {
        /* Sprint 34av : ESC key (Oric matrix col=1, row=5). Pressé via
         * --type-keys '\e' notamment pour naviguer le TUI LOCI. */
        kb->matrix[1] &= ~(1 << 5);
        return true;
    }
    unsigned char uc = (unsigned char)c;
    if (uc > 127) return false;
    const char_entry_t* entry = &char_map[uc];
    if (entry->col < 0) return false;
    kb->matrix[entry->col] &= ~(1 << entry->row);
    if (entry->shift)
        kb->matrix[ORIC_LSHIFT_COL] &= ~(1 << ORIC_LSHIFT_ROW);
    return true;
}

void oric_keyboard_release_all(oric_keyboard_t* kb) {
    memset(kb->matrix, 0xFF, sizeof(kb->matrix));
}

#ifdef HAS_SDL2

/* ================================================================
 * QWERTY positional mapping (existing, for QWERTY keyboard users)
 * ================================================================ */

/**
 * ORIC keyboard matrix mapping (QWERTY layout from Oricutron)
 *
 * Header: PSG R14 mask testing each hardware row (active-low).
 * Side label: hardware column (VIA ORB[0:2] / 74LS138 output index).
 *
 *              Row0(FE) Row1(FD) Row2(FB) Row3(F7) Row4(EF)  Row5(DF)  Row6(BF)  Row7(7F)
 * Col 0:       7        n        5        v        RCTRL     1         x         3
 * Col 1:       j        t        r        f        (none)    ESC       q         d
 * Col 2:       m        6        b        4        LCTRL     z         2         c
 * Col 3:       k        9        ;        -        FUNCT     (none)    \         '
 * Col 4:       SPACE    ,        .        UP       LSHIFT    LEFT      DOWN      RIGHT
 * Col 5:       u        i        o        p        FUNCT     BKSP      ]         [
 * Col 6:       y        h        g        e        FUNCT     a         s         w
 * Col 7:       8        l        0        /        RSHIFT    RETURN    (none)    =
 *
 * Table layout: qwerty_tab[col * 8 + row] = SDL keycode.
 */
static const SDL_Keycode qwerty_tab[64] = {
    /* Col 0 */ SDLK_7,     SDLK_n,     SDLK_5,        SDLK_v,     SDLK_RCTRL,    SDLK_1,        SDLK_x,            SDLK_3,
    /* Col 1 */ SDLK_j,     SDLK_t,     SDLK_r,        SDLK_f,     0,             SDLK_ESCAPE,   SDLK_q,            SDLK_d,
    /* Col 2 */ SDLK_m,     SDLK_6,     SDLK_b,        SDLK_4,     SDLK_LCTRL,    SDLK_z,        SDLK_2,            SDLK_c,
    /* Col 3 */ SDLK_k,     SDLK_9,     SDLK_SEMICOLON,SDLK_MINUS, 0,             0,             SDLK_BACKSLASH,    SDLK_QUOTE,
    /* Col 4 */ SDLK_SPACE, SDLK_COMMA, SDLK_PERIOD,   SDLK_UP,    SDLK_LSHIFT,   SDLK_LEFT,     SDLK_DOWN,         SDLK_RIGHT,
    /* Col 5 */ SDLK_u,     SDLK_i,     SDLK_o,        SDLK_p,     SDLK_LALT,     SDLK_BACKSPACE,SDLK_RIGHTBRACKET, SDLK_LEFTBRACKET,
    /* Col 6 */ SDLK_y,     SDLK_h,     SDLK_g,        SDLK_e,     SDLK_RALT,     SDLK_a,        SDLK_s,            SDLK_w,
    /* Col 7 */ SDLK_8,     SDLK_l,     SDLK_0,        SDLK_SLASH, SDLK_RSHIFT,   SDLK_RETURN,   SDLK_BACKQUOTE,    SDLK_EQUALS
};

static bool handle_qwerty(oric_keyboard_t* kb, const SDL_Event* event) {
    if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)
        return false;

    SDL_Keycode key = event->key.keysym.sym;
    bool down = (event->type == SDL_KEYDOWN);

    for (int i = 0; i < 64; i++) {
        if (qwerty_tab[i] == key) {
            int col = i / 8;
            int row = i % 8;
            if (down) kb->matrix[col] &= ~(1 << row);
            else      kb->matrix[col] |= (1 << row);
            return true;
        }
    }
    return false;
}

/**
 * Non-printable keys handled via SDL scancode (not TEXTINPUT).
 * These produce control codes or have special matrix positions.
 *
 * Entry fields: { SDL scancode, hw col (ORB[0:2]), hw row (PSG R14 bit) }.
 * All four arrow keys share hardware column 4 (the LSHIFT/FUNCT column).
 */
static const struct { SDL_Scancode sc; int8_t col, row; } special_keys[] = {
    { SDL_SCANCODE_UP,        4, 3 },
    { SDL_SCANCODE_DOWN,      4, 6 },
    { SDL_SCANCODE_LEFT,      4, 5 },
    { SDL_SCANCODE_RIGHT,     4, 7 },
    { SDL_SCANCODE_RETURN,    7, 5 },
    { SDL_SCANCODE_ESCAPE,    1, 5 },
    { SDL_SCANCODE_BACKSPACE, 5, 5 },
    { SDL_SCANCODE_DELETE,    5, 5 },
    { SDL_SCANCODE_LCTRL,     2, 4 },
    { SDL_SCANCODE_RCTRL,     0, 4 },
    { SDL_SCANCODE_TAB,       3, 4 },  /* FUNCT */
    { 0, -1, -1 }  /* sentinel */
};

/* --- Pressed key tracking --- */

static void sym_add_pressed(oric_keyboard_t* kb, uint32_t scancode,
                            int8_t col, int8_t row, bool shift) {
    if (kb->pressed_count >= ORIC_KB_MAX_PRESSED) return;
    kb->pressed[kb->pressed_count].scancode = scancode;
    kb->pressed[kb->pressed_count].col = col;
    kb->pressed[kb->pressed_count].row = row;
    kb->pressed[kb->pressed_count].shift = shift;
    kb->pressed_count++;
}

static void sym_remove_pressed(oric_keyboard_t* kb, uint32_t scancode) {
    for (int i = 0; i < kb->pressed_count; i++) {
        if (kb->pressed[i].scancode == scancode) {
            kb->pressed[i] = kb->pressed[kb->pressed_count - 1];
            kb->pressed_count--;
            return;
        }
    }
}

/**
 * Rebuild matrix from all active pressed keys.
 * This handles shift reference counting automatically.
 */
static void sym_rebuild_matrix(oric_keyboard_t* kb) {
    memset(kb->matrix, 0xFF, sizeof(kb->matrix));
    for (int i = 0; i < kb->pressed_count; i++) {
        int8_t col = kb->pressed[i].col;
        int8_t row = kb->pressed[i].row;
        if (col >= 0 && col < 8 && row >= 0 && row < 8)
            kb->matrix[col] &= ~(1 << row);
        if (kb->pressed[i].shift)
            kb->matrix[ORIC_LSHIFT_COL] &= ~(1 << ORIC_LSHIFT_ROW);
    }
}

static bool handle_symbolic(oric_keyboard_t* kb, const SDL_Event* event) {
    /* --- SDL_TEXTINPUT: map character to ORIC key combo --- */
    if (event->type == SDL_TEXTINPUT) {
        unsigned char c = (unsigned char)event->text.text[0];
        if (c > 127) return false;  /* Non-ASCII: ignore */

        const char_entry_t* entry = &char_map[c];
        if (entry->col < 0) return false;  /* Unmapped character */

        if (kb->has_pending) {
            /* Remove any previous entry for this scancode (key repeat) */
            sym_remove_pressed(kb, kb->pending_scancode);
            sym_add_pressed(kb, kb->pending_scancode,
                           entry->col, entry->row, entry->shift);
            sym_rebuild_matrix(kb);
            kb->has_pending = false;
        }
        return true;
    }

    /* --- SDL_KEYDOWN --- */
    if (event->type == SDL_KEYDOWN) {
        SDL_Scancode sc = event->key.keysym.scancode;

        /* Check special keys (arrows, ESC, Return, DEL, CTRL) */
        for (int i = 0; special_keys[i].sc != 0; i++) {
            if (special_keys[i].sc == sc) {
                /* Avoid duplicates on key repeat */
                sym_remove_pressed(kb, (uint32_t)sc);
                sym_add_pressed(kb, (uint32_t)sc,
                               special_keys[i].col, special_keys[i].row, false);
                sym_rebuild_matrix(kb);
                return true;
            }
        }

        /* Ignore Shift/Alt/AltGr (handled by TEXTINPUT character mapping) */
        if (sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT ||
            sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT)
            return false;

        /* Store scancode as pending, waiting for TEXTINPUT to tell us
         * which character this key produces on the user's layout */
        kb->pending_scancode = (uint32_t)sc;
        kb->has_pending = true;
        return false;
    }

    /* --- SDL_KEYUP --- */
    if (event->type == SDL_KEYUP) {
        SDL_Scancode sc = event->key.keysym.scancode;

        /* Clear pending if this key is released before TEXTINPUT arrived */
        if (kb->has_pending && kb->pending_scancode == (uint32_t)sc)
            kb->has_pending = false;

        sym_remove_pressed(kb, (uint32_t)sc);
        sym_rebuild_matrix(kb);
        return true;
    }

    return false;
}

/* ================================================================
 * Public API
 * ================================================================ */

bool oric_keyboard_handle_sdl_event(oric_keyboard_t* kb, const SDL_Event* event) {
    if (kb->layout == ORIC_KB_AZERTY)
        return handle_symbolic(kb, event);
    return handle_qwerty(kb, event);
}

#endif /* HAS_SDL2 */
