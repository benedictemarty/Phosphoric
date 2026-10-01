/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file iomenu.c
 * @brief Input/output peripherals menu (F1): model, keys, drawing.
 * @author bmarty <bmarty@mailo.com>
 *
 * Layout and navigation taken from the Neo6502TeleStrat menu
 * (src/osd/osd_menu.h, zlib/libpng licence), rewritten for an 80 × 40 grid
 * and the Oric's peripherals. See include/video/iomenu.h.
 */
#include "video/iomenu.h"
#include "video/iom_font.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  Text surface
 * ═══════════════════════════════════════════════════════════════════════ */

static void s_clear(iom_surface_t* s, uint8_t attr) {
    memset(s->ch, ' ', sizeof(s->ch));
    memset(s->attr, attr, sizeof(s->attr));
    memset(s->big, 0, sizeof(s->big));
}

static void s_fill(iom_surface_t* s, int row, int col, int rows, int cols, uint8_t attr) {
    for (int r = row; r < row + rows; r++) {
        if (r < 0 || r >= IOM_ROWS) continue;
        for (int c = col; c < col + cols; c++) {
            if (c < 0 || c >= IOM_COLS) continue;
            s->ch[r][c] = ' ';
            s->attr[r][c] = attr;
            s->big[r][c] = 0;
        }
    }
}

static void s_putc(iom_surface_t* s, int row, int col, uint8_t ch, uint8_t attr) {
    if (row < 0 || row >= IOM_ROWS || col < 0 || col >= IOM_COLS) return;
    s->ch[row][col] = ch;
    s->attr[row][col] = attr;
    s->big[row][col] = 0;
}

/* Next character of a UTF-8 string, mapped to the font encoding
 * (Latin-1; dashes, ellipsis and arrow have a glyph of their own). */
static uint8_t next_char(const char** p) {
    const uint8_t* s = (const uint8_t*)*p;
    uint32_t c = s[0];
    int n = 1;
    if (c >= 0xC0 && c < 0xE0 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        n = 2;
    } else if (c >= 0xE0 && c < 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        n = 3;
    } else if (c >= 0x80) {
        c = '?';
    }
    *p += n;
    if (c == 0x2014 || c == 0x2013) return IOM_EMDASH;
    if (c == 0x2026) return IOM_ELLIPSIS;
    if (c == 0x2192) return IOM_ARROW_R;
    return c < 256 ? (uint8_t)c : '?';
}

int iom_puts(iom_surface_t* s, int row, int col, const char* str, uint8_t attr, int max) {
    int n = 0;
    while (*str && (max < 0 || n < max) && col + n < IOM_COLS) {
        s_putc(s, row, col + n, next_char(&str), attr);
        n++;
    }
    return n;
}

static int u_strlen(const char* str) {
    int n = 0;
    while (*str) { next_char(&str); n++; }
    return n;
}

static int s_puts_big(iom_surface_t* s, int row, int col, const char* str, uint8_t attr) {
    int n = 0;
    while (*str && col + n + 1 < IOM_COLS) {
        uint8_t c = next_char(&str);
        s_putc(s, row, col + n, c, attr);
        s_putc(s, row, col + n + 1, c, attr);
        s->big[row][col + n] = 1;
        s->big[row][col + n + 1] = 2;
        n += 2;
    }
    return n;
}

static void s_frame(iom_surface_t* s, int row, int col, int rows, int cols, uint8_t attr) {
    for (int c = col + 1; c < col + cols - 1; c++) {
        s_putc(s, row, c, IOM_HLINE, attr);
        s_putc(s, row + rows - 1, c, IOM_HLINE, attr);
    }
    for (int r = row + 1; r < row + rows - 1; r++) {
        s_putc(s, r, col, IOM_VLINE, attr);
        s_putc(s, r, col + cols - 1, IOM_VLINE, attr);
    }
    s_putc(s, row, col, IOM_TL, attr);
    s_putc(s, row, col + cols - 1, IOM_TR, attr);
    s_putc(s, row + rows - 1, col, IOM_BL, attr);
    s_putc(s, row + rows - 1, col + cols - 1, IOM_BR, attr);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  File picker
 * ═══════════════════════════════════════════════════════════════════════ */

/* "media": where the web page drops loaded files (/media). */
static const char* const default_dirs[] = { "tapes", "disks", "snapshots", "media", "demos/ula-ng", ".", NULL };

static bool has_ext(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static int file_cmp(const void* a, const void* b) {
    return strcasecmp(((const iom_file_t*)a)->name, ((const iom_file_t*)b)->name);
}

static iom_file_kind_t kind_of(int item) {
    if (item == IOM_ITEM_TAPE) return IOM_FILE_TAP;
    if (item == IOM_ITEM_SNAPSHOT) return IOM_FILE_OST;
    return IOM_FILE_DSK;
}

/* ROMs, images, firmwares: directories browsed for a card parameter. */
static const char* const card_dirs[] = { "roms", "roms/loci", "disks", "tapes", "media", ".", NULL };

static void scan_files(iom_menu_t* m, iom_file_kind_t kind) {
    static const char* const ext[] = { ".dsk", ".tap", ".ost", "" };
    const char* const* dirs = kind == IOM_FILE_ANY ? card_dirs : m->dirs ? m->dirs : default_dirs;
    m->nfiles = 0;
    for (int d = 0; dirs[d]; d++) {
        DIR* dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent* de;
        while ((de = readdir(dp)) != NULL && m->nfiles < IOM_FILES) {
            if (kind == IOM_FILE_ANY ? de->d_name[0] == '.' : !has_ext(de->d_name, ext[kind])) continue;
            iom_file_t* f = &m->files[m->nfiles];
            if (snprintf(f->path, sizeof(f->path), "%s/%s", dirs[d], de->d_name) >= (int)sizeof(f->path))
                continue;
            struct stat ds;
            if (kind == IOM_FILE_ANY && (stat(f->path, &ds) != 0 || !S_ISREG(ds.st_mode))) continue;
            snprintf(f->name, sizeof(f->name), "%.*s", (int)sizeof(f->name) - 1, de->d_name);
            struct stat sb;
            f->size = (stat(f->path, &sb) == 0) ? (uint32_t)sb.st_size : 0;
            /* Same name in two directories: keep the first one. */
            bool dup = false;
            for (int k = 0; k < m->nfiles; k++)
                if (strcmp(m->files[k].name, f->name) == 0) { dup = true; break; }
            if (!dup) m->nfiles++;
        }
        closedir(dp);
    }
    qsort(m->files, (size_t)m->nfiles, sizeof(iom_file_t), file_cmp);
}

static void browse_clamp(iom_menu_t* m) {
    const int n = m->nfiles + 1;
    if (m->browse_cursor < 0) m->browse_cursor = 0;
    if (m->browse_cursor >= n) m->browse_cursor = n - 1;
    if (m->browse_cursor < m->browse_scroll) m->browse_scroll = m->browse_cursor;
    if (m->browse_cursor >= m->browse_scroll + IOM_BROWSE_VISIBLE)
        m->browse_scroll = m->browse_cursor - IOM_BROWSE_VISIBLE + 1;
}

static void open_browser_card(iom_menu_t* m) {
    scan_files(m, IOM_FILE_ANY);
    m->browse_target = IOM_BROWSE_CARD;
    m->browse_cursor = 0;
    m->browse_scroll = 0;
    const char* cur = m->cards.card[m->card_sel].value[m->param_cursor - 1];
    for (int k = 0; k < m->nfiles; k++)
        if (cur[0] && strcmp(m->files[k].path, cur) == 0) m->browse_cursor = k + 1;
    browse_clamp(m);
    m->browsing = true;
}

static void open_browser(iom_menu_t* m, int item) {
    scan_files(m, kind_of(item));
    m->browse_target = item;
    m->browse_cursor = 0;
    m->browse_scroll = 0;
    /* Cursor on the media currently inserted. */
    const char* cur = item < IOM_ITEM_TAPE ? m->st.drive[item]
                    : item == IOM_ITEM_TAPE ? m->st.tape : m->st.snapshot_last;
    for (int k = 0; k < m->nfiles; k++)
        if (cur[0] && strcmp(m->files[k].name, cur) == 0) m->browse_cursor = k + 1;
    browse_clamp(m);
    m->browsing = true;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Life cycle and keys
 * ═══════════════════════════════════════════════════════════════════════ */

void iom_init(iom_menu_t* m) {
    memset(m, 0, sizeof(*m));
    m->cursor = IOM_ITEM_RESUME;
    m->st.version = "";
    m->st.machine = "";
}

void iom_open(iom_menu_t* m) {
    m->open = true;
    m->browsing = false;
    m->page = IOM_PAGE_MAIN;
    m->editing = false;
    m->cursor = IOM_ITEM_RESUME;
    m->sub = 0;
    m->message[0] = '\0';
}

void iom_close(iom_menu_t* m) {
    m->open = false;
    m->browsing = false;
}

void iom_set_dirs(iom_menu_t* m, const char* const* dirs) { m->dirs = dirs; }

void iom_message(iom_menu_t* m, bool error, const char* text) {
    snprintf(m->message, sizeof(m->message), "%s", text);
    m->message_error = error;
}

static bool is_drive(int item) { return item >= IOM_ITEM_DRIVE0 && item < IOM_ITEM_TAPE; }

static iom_action_t browse_key(iom_menu_t* m, int key) {
    iom_action_t a = { IOM_ACT_NONE, 0, "" };
    const int n = m->nfiles + 1;
    switch (key) {
    case IOM_KEY_UP:   m->browse_cursor = (m->browse_cursor + n - 1) % n; break;
    case IOM_KEY_DOWN: m->browse_cursor = (m->browse_cursor + 1) % n; break;
    case IOM_KEY_PGUP: m->browse_cursor -= IOM_BROWSE_VISIBLE; break;
    case IOM_KEY_PGDN: m->browse_cursor += IOM_BROWSE_VISIBLE; break;
    case IOM_KEY_HOME: m->browse_cursor = 0; break;
    case IOM_KEY_END:  m->browse_cursor = n - 1; break;
    case IOM_KEY_ESC:
    case IOM_KEY_LEFT:
        m->browsing = false;
        break;
    case IOM_KEY_ENTER: {
        const int item = m->browse_target;
        const bool none = m->browse_cursor == 0;
        if (item == IOM_BROWSE_CARD) {   /* card parameter: chosen path */
            snprintf(m->cards.card[m->card_sel].value[m->param_cursor - 1], CARD_VALUE_MAX, "%s",
                     none ? "" : m->files[m->browse_cursor - 1].path);
            m->browsing = false;
            return a;
        }
        if (!none)
            snprintf(a.path, sizeof(a.path), "%s", m->files[m->browse_cursor - 1].path);
        if (is_drive(item)) {
            a.type = none ? IOM_ACT_DISK_EJECT : IOM_ACT_DISK_INSERT;
            a.target = item - IOM_ITEM_DRIVE0;
            /* Same image already in another drive: refused (two drives
             * would write the same image). */
            if (!none) {
                for (int d = 0; d < 4; d++) {
                    if (d != a.target && m->st.drive[d][0] &&
                        strcmp(m->st.drive[d], m->files[m->browse_cursor - 1].name) == 0) {
                        char msg[96];
                        snprintf(msg, sizeof(msg), "Image déjà dans le lecteur %c", 'A' + d);
                        iom_message(m, true, msg);
                        a.type = IOM_ACT_NONE;
                        return a;
                    }
                }
            }
        } else if (item == IOM_ITEM_TAPE) {
            a.type = none ? IOM_ACT_TAPE_EJECT : IOM_ACT_TAPE_INSERT;
        } else {
            a.type = none ? IOM_ACT_SNAPSHOT_SAVE : IOM_ACT_SNAPSHOT_LOAD;
        }
        m->browsing = false;
        return a;
    }
    default:
        /* Letter: jump to the next file starting with that initial. */
        if (key > ' ' && key < 0x100 && m->nfiles > 0) {
            for (int k = 1; k <= m->nfiles; k++) {
                const int idx = (m->browse_cursor - 1 + k + m->nfiles) % m->nfiles;
                if (toupper((unsigned char)m->files[idx].name[0]) == toupper(key)) {
                    m->browse_cursor = idx + 1;
                    break;
                }
            }
        }
        break;
    }
    browse_clamp(m);
    return a;
}

/* ── Expansion cards ────────────────────────────────────────────────── */

static bool cards_changed(const iom_menu_t* m) {
    return memcmp(&m->cards, &m->cards_orig, sizeof(m->cards)) != 0;
}

/* Parameter editing: printable characters, Del / Backspace erases, Enter
 * confirms (address checked), Esc cancels. */
void iom_text(iom_menu_t* m, const char* utf8) {
    if (!m->open || !m->editing) return;
    size_t n = strlen(m->edit);
    while (*utf8 && n + 4 < sizeof(m->edit)) {
        unsigned char c = (unsigned char)*utf8++;
        if (c >= 0x20 && c != 0x7F) m->edit[n++] = (char)c;   /* UTF-8 copied as is */
    }
    m->edit[n] = '\0';
}

static void edit_key(iom_menu_t* m, int key) {
    char* v = m->cards.card[m->card_sel].value[m->param_cursor - 1];
    const card_param_t* p = &cards_get(m->card_sel)->param[m->param_cursor - 1];
    size_t n = strlen(m->edit);
    switch (key) {
    case IOM_KEY_DEL:
        while (n > 0 && ((unsigned char)m->edit[n - 1] & 0xC0) == 0x80) n--;   /* UTF-8 continuation byte */
        if (n > 0) n--;
        m->edit[n] = '\0';
        break;
    case IOM_KEY_ESC:
        m->editing = false;
        break;
    case IOM_KEY_ENTER:
        if (p->kind == CARD_P_HEX && m->edit[0]) {
            char* end;
            unsigned long a = strtoul(m->edit, &end, 16);
            if (*end || a < 0x0300 || a > 0x03FF) {
                iom_message(m, true, "Adresse d'E/S hexadécimale entre 0300 et 03FF");
                return;
            }
        }
        snprintf(v, CARD_VALUE_MAX, "%s", m->edit);
        m->editing = false;
        break;
    default:
        break;   /* characters arrive via iom_text (typed text) */
    }
}

static iom_action_t cards_key(iom_menu_t* m, int key) {
    iom_action_t a = { IOM_ACT_NONE, 0, "" };
    const int n = cards_count() + 2;   /* cards, Apply, Cancel */
    int c = m->card_cursor;
    switch (key) {
    case IOM_KEY_UP:   c = (c + n - 1) % n; break;
    case IOM_KEY_DOWN: c = (c + 1) % n; break;
    case IOM_KEY_HOME: c = 0; break;
    case IOM_KEY_END:  c = n - 1; break;
    case IOM_KEY_LEFT:
    case IOM_KEY_ESC:  m->page = IOM_PAGE_MAIN; break;
    case IOM_KEY_ENTER:
        if (c < cards_count()) {
            m->card_sel = c;
            m->param_cursor = 0;
            m->page = IOM_PAGE_CARD;
        } else if (c == cards_count()) {
            char why[96];
            if (m->cards_readonly)
                iom_message(m, true, "Version web : les cartes se choisissent au lancement");
            else if (!cards_changed(m))
                iom_message(m, false, "Aucun changement de cartes");
            else if (cards_conflict(&m->cards, why, sizeof(why)))
                iom_message(m, true, why);
            else
                a.type = IOM_ACT_CARDS_APPLY;
        } else {
            m->cards = m->cards_orig;
            iom_message(m, false, "Cartes : retour à la machine en cours");
        }
        break;
    default: break;
    }
    m->card_cursor = c;
    return a;
}

static void card_key(iom_menu_t* m, int key) {
    const card_desc_t* d = cards_get(m->card_sel);
    const int n = 1 + d->nparams;
    int c = m->param_cursor;
    switch (key) {
    case IOM_KEY_UP:   c = (c + n - 1) % n; break;
    case IOM_KEY_DOWN: c = (c + 1) % n; break;
    case IOM_KEY_LEFT:
    case IOM_KEY_ESC:  m->page = IOM_PAGE_CARDS; break;
    case IOM_KEY_DEL:   /* parameter: back to the default value */
        if (c > 0 && !m->cards_readonly)
            snprintf(m->cards.card[m->card_sel].value[c - 1], CARD_VALUE_MAX, "%s", d->param[c - 1].def);
        break;
    case IOM_KEY_ENTER:
        if (m->cards_readonly) {
            iom_message(m, true, "Version web : les cartes se choisissent au lancement");
        } else if (c == 0) {
            if (d->fixed) iom_message(m, false, "Carte toujours présente");
            else cards_set_on(&m->cards, m->card_sel, !m->cards.card[m->card_sel].on);
        } else {
            const card_param_t* p = &d->param[c - 1];
            char* v = m->cards.card[m->card_sel].value[c - 1];
            if (p->kind == CARD_P_BOOL) {
                snprintf(v, CARD_VALUE_MAX, "%s", strcmp(v, "oui") == 0 ? "non" : "oui");
            } else if (p->kind == CARD_P_FILE) {
                m->param_cursor = c;
                open_browser_card(m);
            } else {
                snprintf(m->edit, sizeof(m->edit), "%s", v);
                m->editing = true;
            }
        }
        break;
    default: break;
    }
    if (m->page == IOM_PAGE_CARD) m->param_cursor = c;
}

iom_action_t iom_key(iom_menu_t* m, int key) {
    iom_action_t a = { IOM_ACT_NONE, 0, "" };
    if (!m->open) return a;
    if (m->browsing) return browse_key(m, key);
    if (m->editing) { edit_key(m, key); return a; }
    if (m->page == IOM_PAGE_CARDS) return cards_key(m, key);
    if (m->page == IOM_PAGE_CARD) { card_key(m, key); return a; }
    int c = m->cursor;
    switch (key) {
    case IOM_KEY_UP:   c = (c + IOM_ITEMS - 1) % IOM_ITEMS; break;
    case IOM_KEY_DOWN: c = (c + 1) % IOM_ITEMS; break;
    case IOM_KEY_HOME: c = 0; break;
    case IOM_KEY_END:  c = IOM_ITEMS - 1; break;
    case IOM_KEY_LEFT:
        if (c <= IOM_ITEM_TAPE) m->sub = 0;
        else if (c == IOM_ITEM_KEYBOARD) c = IOM_ITEM_JOYSTICK;
        else if (c > IOM_ITEM_RESET) c--;
        break;
    case IOM_KEY_RIGHT:
        if (c <= IOM_ITEM_TAPE && !(is_drive(c) && m->st.no_drive_protect)) m->sub = 1;
        else if (c == IOM_ITEM_JOYSTICK) c = IOM_ITEM_KEYBOARD;
        else if (c >= IOM_ITEM_RESET && c < IOM_ITEM_RESUME) c++;
        break;
    case IOM_KEY_ESC:
        a.type = IOM_ACT_RESUME;
        break;
    case IOM_KEY_DEL:
        if (is_drive(c)) { a.type = IOM_ACT_DISK_EJECT; a.target = c - IOM_ITEM_DRIVE0; }
        else if (c == IOM_ITEM_TAPE) a.type = IOM_ACT_TAPE_EJECT;
        break;
    case IOM_KEY_ENTER:
        if (is_drive(c)) {
            if (c - IOM_ITEM_DRIVE0 >= m->st.drives) {
                iom_message(m, true, m->st.disk_iface
                    ? "Lecteur absent sur cette interface"
                    : "Pas d'interface disque (--disk-rom ou --jasmin-rom au lancement)");
            } else if (m->sub == 1 && !m->st.no_drive_protect) {
                a.type = IOM_ACT_DISK_PROTECT;
                a.target = c - IOM_ITEM_DRIVE0;
            } else {
                open_browser(m, c);
            }
        } else if (c == IOM_ITEM_TAPE) {
            if (m->sub == 1) a.type = IOM_ACT_TAPE_REWIND;
            else open_browser(m, c);
        } else if (c == IOM_ITEM_CARDS) {
            m->page = IOM_PAGE_CARDS;
            m->card_cursor = 0;
        } else if (c == IOM_ITEM_SNAPSHOT)   open_browser(m, c);
        else if (c == IOM_ITEM_PRINTER)   a.type = IOM_ACT_PRINTER_CYCLE;
        else if (c == IOM_ITEM_JOYSTICK)  a.type = IOM_ACT_JOYSTICK_CYCLE;
        else if (c == IOM_ITEM_KEYBOARD)  a.type = IOM_ACT_KEYBOARD_TOGGLE;
        else if (c == IOM_ITEM_TAPE_FAST) a.type = IOM_ACT_TAPE_FAST_TOGGLE;
        else if (c == IOM_ITEM_RESET)     a.type = IOM_ACT_RESET;
        else if (c == IOM_ITEM_SAVE)      a.type = IOM_ACT_SAVE_CONFIG;
        else                              a.type = IOM_ACT_RESUME;
        break;
    default: break;
    }
    if (c > IOM_ITEM_TAPE) m->sub = 0;
    m->cursor = c;
    return a;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Drawing
 * ═══════════════════════════════════════════════════════════════════════ */

#define A_BG        IOM_ATTR(IOM_WHITE, IOM_BLACK)
#define A_PANEL     IOM_ATTR(IOM_WHITE, IOM_BLUE | IOM_DITHER)
#define A_PANEL_DIM IOM_ATTR(IOM_CYAN, IOM_BLUE | IOM_DITHER)
#define A_PANEL_ACC IOM_ATTR(IOM_YELLOW, IOM_BLUE | IOM_DITHER)
#define A_PANEL_OK  IOM_ATTR(IOM_GREEN, IOM_BLUE | IOM_DITHER)
#define A_PANEL_ERR IOM_ATTR(IOM_RED, IOM_BLUE | IOM_DITHER)
#define A_EDGE      IOM_ATTR(IOM_CYAN, IOM_BLUE | IOM_DITHER)
#define A_SEL       IOM_ATTR(IOM_WHITE, IOM_BLUE)
#define A_SEL_ACC   IOM_ATTR(IOM_YELLOW, IOM_BLUE)
#define A_SEL_DIM   IOM_ATTR(IOM_CYAN, IOM_BLUE)
#define A_SEL_OK    IOM_ATTR(IOM_GREEN, IOM_BLUE)
#define A_SEL_ERR   IOM_ATTR(IOM_RED, IOM_BLUE)

/* Attribute set of a line, depending on whether it holds the selection. */
typedef struct { uint8_t base, dim, acc, ok, err; } row_attrs_t;
static row_attrs_t attrs_for(bool sel) {
    row_attrs_t r = { sel ? A_SEL : A_PANEL, sel ? A_SEL_DIM : A_PANEL_DIM, sel ? A_SEL_ACC : A_PANEL_ACC,
                      sel ? A_SEL_OK : A_PANEL_OK, sel ? A_SEL_ERR : A_PANEL_ERR };
    return r;
}

static void panel(iom_surface_t* s, int row, int col, int rows, int cols, uint8_t icon, const char* title) {
    s_fill(s, row, col, rows, cols, A_PANEL);
    s_frame(s, row, col, rows, cols, A_EDGE);
    int c = col + 2;
    s_putc(s, row, c++, ' ', A_EDGE);
    if (icon) {
        s_putc(s, row, c++, icon, A_PANEL_ACC);
        s_putc(s, row, c++, (uint8_t)(icon + 1), A_PANEL_ACC);
        s_putc(s, row, c++, ' ', A_EDGE);
    }
    c += iom_puts(s, row, c, title, A_PANEL, -1);
    s_putc(s, row, c, ' ', A_EDGE);
}

/* Full selection bar, ▶ marker. */
static void item_bar(iom_surface_t* s, int row, int col, int cols, bool sel) {
    s_fill(s, row, col, 1, cols, sel ? A_SEL : A_PANEL);
    if (sel) s_putc(s, row, col, IOM_TRI_R, A_SEL_ACC);
}

static void button(iom_surface_t* s, int row, int col, int cols, const char* label, bool sel) {
    const uint8_t attr = sel ? IOM_ATTR(IOM_BLACK, IOM_CYAN) : IOM_ATTR(IOM_WHITE, IOM_BLUE | IOM_DITHER);
    s_fill(s, row, col, 1, cols, attr);
    iom_puts(s, row, col + (cols - u_strlen(label)) / 2, label, attr, -1);
}

static void size_str(char* buf, size_t n, uint32_t size) {
    if (size >= 1024 * 1024) snprintf(buf, n, "%u,%u Mo", size >> 20, (unsigned)((size % (1u << 20)) * 10 >> 20));
    else snprintf(buf, n, "%u Ko", (size + 1023) >> 10);
}

/* Status dot + label; returns the width written. */
static int state(iom_surface_t* s, int row, int col, bool on, const char* text, const row_attrs_t* ra) {
    s_putc(s, row, col, on ? IOM_DOT : IOM_CROSS, on ? ra->ok : ra->err);
    return 2 + iom_puts(s, row, col + 2, text, on ? ra->ok : ra->err, -1);
}

static void draw_media(const iom_menu_t* m, iom_surface_t* s) {
    const iom_state_t* st = &m->st;
    char title[48], buf[96];
    if (st->disk_iface) snprintf(title, sizeof(title), "Disquettes — %s", st->disk_iface);
    else snprintf(title, sizeof(title), "Disquettes (pas d'interface disque)");
    panel(s, 5, 2, 13, 76, IOM_FLOP_L, title);
    for (int d = 0; d < 4; d++) {
        const int row = 7 + 2 * d;
        const bool on = !m->browsing && m->cursor == IOM_ITEM_DRIVE0 + d;
        item_bar(s, row, 4, 72, on && m->sub == 0);
        if (on && m->sub == 1 && !st->no_drive_protect) item_bar(s, row, 57, 19, true);
        const row_attrs_t ra = attrs_for(on && m->sub == 0), rp = attrs_for(on && m->sub == 1);
        snprintf(buf, sizeof(buf), "%c", 'A' + d);
        iom_puts(s, row, 6, buf, ra.acc, -1);
        if (d >= st->drives) {
            iom_puts(s, row, 9, "— absent —", ra.dim, -1);
            continue;
        }
        if (st->drive[d][0]) iom_puts(s, row, 9, st->drive[d], ra.base, 46);
        else iom_puts(s, row, 9, "— vide —", ra.dim, -1);
        if (st->no_drive_protect) continue;
        if (st->drive_ro[d]) {
            s_putc(s, row, 59, IOM_LOCK, rp.err);
            iom_puts(s, row, 61, "protégée", rp.dim, -1);
        } else {
            s_putc(s, row, 59, IOM_DOT, rp.ok);
            iom_puts(s, row, 61, "écriture", rp.dim, -1);
        }
    }
    const int row = 15;
    const bool on = !m->browsing && m->cursor == IOM_ITEM_TAPE;
    item_bar(s, row, 4, 72, on && m->sub == 0);
    if (on && m->sub == 1) item_bar(s, row, 57, 19, true);
    const row_attrs_t ra = attrs_for(on && m->sub == 0), rp = attrs_for(on && m->sub == 1);
    s_putc(s, row, 6, IOM_TAPE_L, ra.acc);
    s_putc(s, row, 7, IOM_TAPE_R, ra.acc);
    if (st->tape[0]) {
        iom_puts(s, row, 9, st->tape, ra.base, 34);
        s_putc(s, row, 45, st->tape_motor ? IOM_TRI_R : IOM_FULL, st->tape_motor ? ra.acc : ra.dim);
        snprintf(buf, sizeof(buf), "%3d %%", st->tape_percent);
        iom_puts(s, row, 47, buf, ra.dim, -1);
        for (int i = 0; i < 4; i++)
            s_putc(s, row, 53 + i, i * 25 < st->tape_percent ? IOM_FULL : IOM_SHADE, ra.dim);
        s_putc(s, row, 59, IOM_ENTER, rp.acc);
        iom_puts(s, row, 61, "rembobiner", rp.dim, -1);
    } else {
        iom_puts(s, row, 9, "— pas de cassette —", ra.dim, -1);
    }
}

/* Text wrapped at spaces over @p width columns, at most @p rows lines. */
static void wrap(iom_surface_t* s, int row, int col, int width, int rows, const char* text, uint8_t attr) {
    char line[160];
    int r = 0;
    while (*text && r < rows) {
        while (*text == ' ') text++;
        const char* end = text;
        const char* cut = NULL;
        int w = 0;
        while (*end) {
            const char* p = end;
            next_char(&p);
            if (*end == ' ') cut = end;
            if (++w > width) break;
            end = p;
        }
        if (*end && cut && cut > text) end = cut;
        size_t n = (size_t)(end - text);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, text, n);
        line[n] = '\0';
        iom_puts(s, row + r++, col, line, attr, width);
        text = end;
    }
}

static void draw_cards(const iom_menu_t* m, iom_surface_t* s) {
    const iom_state_t* st = &m->st;
    const bool on = !m->browsing && m->cursor == IOM_ITEM_CARDS;
    panel(s, 19, 2, 8, 76, IOM_CART_L, on ? "Cartes d'extension — Entrée : choisir, régler"
                                          : "Cartes d'extension");
    if (on) s_frame(s, 19, 2, 8, 76, IOM_ATTR(IOM_YELLOW, IOM_BLUE | IOM_DITHER));
    for (int i = 0; i < st->cards && i < IOM_CARDS; i++) {
        const int row = 20 + i / 2, col = (i % 2) ? 41 : 4;
        const iom_card_t* k = &st->card[i];
        s_putc(s, row, col, k->present ? IOM_DOT : IOM_CROSS, k->present ? A_PANEL_OK : A_PANEL_DIM);
        iom_puts(s, row, col + 2, k->name, k->present ? A_PANEL_ACC : A_PANEL_DIM, 13);
        iom_puts(s, row, col + 16, k->present ? k->detail : "—", A_PANEL_DIM, 20);
    }
}

static void draw_devices(const iom_menu_t* m, iom_surface_t* s) {
    const iom_state_t* st = &m->st;
    char buf[96];
    panel(s, 27, 2, 6, 76, 0, "Périphériques");
    {   /* Snapshots */
        const bool on = !m->browsing && m->cursor == IOM_ITEM_SNAPSHOT;
        const row_attrs_t ra = attrs_for(on);
        item_bar(s, 28, 4, 72, on);
        iom_puts(s, 28, 6, "Instantanés", ra.acc, -1);
        int n = iom_puts(s, 28, 19, "enregistrer ou reprendre la machine", ra.base, -1);
        if (st->snapshot_last[0]) {
            snprintf(buf, sizeof(buf), "→ %s", st->snapshot_last);
            iom_puts(s, 28, 20 + n, buf, ra.dim, 76 - 20 - n);
        }
    }
    {   /* Printer (parallel port, full width: the output file) */
        const bool on = !m->browsing && m->cursor == IOM_ITEM_PRINTER;
        const row_attrs_t ra = attrs_for(on);
        item_bar(s, 29, 4, 72, on);
        iom_puts(s, 29, 6, "Imprimante", ra.acc, -1);
        static const char* const pr[] = { "coupée", "texte (LPRINT, LLIST)", "traceur MCP-40" };
        int n = state(s, 29, 19, st->printer != IOM_PRINTER_OFF, pr[st->printer], &ra);
        if (st->printer != IOM_PRINTER_OFF && st->printer_file[0]) {
            snprintf(buf, sizeof(buf), "→ %s", st->printer_file);
            iom_puts(s, 29, 20 + n, buf, ra.dim, 76 - 20 - n);
        }
    }
    {   /* Joystick | Keyboard */
        const bool on_j = !m->browsing && m->cursor == IOM_ITEM_JOYSTICK;
        const bool on_k = !m->browsing && m->cursor == IOM_ITEM_KEYBOARD;
        const row_attrs_t rj = attrs_for(on_j), rk = attrs_for(on_k);
        item_bar(s, 30, 4, 36, on_j);
        item_bar(s, 30, 41, 35, on_k);
        iom_puts(s, 30, 6, "Joystick", rj.acc, -1);
        static const char* const jo[] = { "aucun", "clavier (flèches)", "manette" };
        state(s, 30, 19, st->joystick != IOM_JOY_NONE, jo[st->joystick], &rj);
        iom_puts(s, 30, 43, "Clavier", rk.acc, -1);
        s_putc(s, 30, 53, IOM_DOT, rk.ok);
        iom_puts(s, 30, 55, st->azerty ? "AZERTY" : "QWERTY", rk.ok, -1);
    }
    {   /* Cassette: loading mode at startup (-f), taken into account at the
         * next start (phosphoric.cfg) */
        const bool on = !m->browsing && m->cursor == IOM_ITEM_TAPE_FAST;
        const row_attrs_t ra = attrs_for(on);
        item_bar(s, 31, 4, 72, on);
        iom_puts(s, 31, 6, "Cassette", ra.acc, -1);
        int n = state(s, 31, 19, st->tape_fast, st->tape_fast ? "injection directe (-f)"
                                                           : "CLOAD par la ROM corrigée", &ra);
        iom_puts(s, 31, 20 + n, "au lancement", ra.dim, -1);
    }
}

static void draw_browser(const iom_menu_t* m, iom_surface_t* s) {
    const int item = m->browse_target;
    const bool drive = is_drive(item), tape = item == IOM_ITEM_TAPE;
    char buf[96];
    if (item == IOM_BROWSE_CARD)
        snprintf(buf, sizeof(buf), "%s — %s", cards_get(m->card_sel)->name,
                 cards_get(m->card_sel)->param[m->param_cursor - 1].label);
    else if (drive) snprintf(buf, sizeof(buf), "Disquette pour le lecteur %c", 'A' + item);
    else if (tape) snprintf(buf, sizeof(buf), "Cassette (la même : rembobinée)");
    else snprintf(buf, sizeof(buf), "Instantanés (reprendre : la machine revient à cet instant)");
    const int top = 7, left = 6, width = 68, height = IOM_BROWSE_VISIBLE + 4;
    s_fill(s, top + 1, left + 2, height, width, IOM_ATTR(IOM_WHITE, IOM_BLUE | IOM_DITHER));  /* shadow */
    s_fill(s, top, left, height, width, IOM_ATTR(IOM_WHITE, IOM_BLACK));
    s_frame(s, top, left, height, width, IOM_ATTR(IOM_YELLOW, IOM_BLACK));
    const uint8_t icon = drive ? IOM_FLOP_L : tape ? IOM_TAPE_L : IOM_USB_L;
    s_putc(s, top, left + 2, ' ', IOM_ATTR(IOM_YELLOW, IOM_BLACK));
    s_putc(s, top, left + 3, icon, IOM_ATTR(IOM_YELLOW, IOM_BLACK));
    s_putc(s, top, left + 4, (uint8_t)(icon + 1), IOM_ATTR(IOM_YELLOW, IOM_BLACK));
    const int tl = iom_puts(s, top, left + 6, buf, IOM_ATTR(IOM_WHITE, IOM_BLACK), width - 9);
    s_putc(s, top, left + 6 + tl, ' ', IOM_ATTR(IOM_YELLOW, IOM_BLACK));
    const int n = m->nfiles + 1;
    for (int k = 0; k < IOM_BROWSE_VISIBLE && m->browse_scroll + k < n; k++) {
        const int idx = m->browse_scroll + k, row = top + 2 + k;
        const bool sel = idx == m->browse_cursor;
        const uint8_t base = sel ? IOM_ATTR(IOM_BLACK, IOM_CYAN) : IOM_ATTR(IOM_WHITE, IOM_BLACK);
        const uint8_t dim = sel ? IOM_ATTR(IOM_BLUE, IOM_CYAN) : IOM_ATTR(IOM_CYAN, IOM_BLACK);
        s_fill(s, row, left + 2, 1, width - 5, base);
        if (idx == 0) {
            iom_puts(s, row, left + 4, item == IOM_BROWSE_CARD ? "Aucun fichier (vide)"
                                     : drive ? "Éjecter la disquette" : tape ? "Éjecter la cassette"
                                             : "Enregistrer un nouvel instantané", dim, -1);
            continue;
        }
        const iom_file_t* f = &m->files[idx - 1];
        iom_puts(s, row, left + 4, item == IOM_BROWSE_CARD ? f->path : f->name, base, 44);
        char sz[16];
        size_str(sz, sizeof(sz), f->size);
        iom_puts(s, row, left + width - 5 - u_strlen(sz), sz, dim, -1);
        for (int d = 0; drive && d < 4; d++) {
            if (d == item || strcmp(m->st.drive[d], f->name) != 0) continue;
            snprintf(buf, sizeof(buf), "en %c", 'A' + d);
            iom_puts(s, row, left + width - 17, buf,
                     sel ? IOM_ATTR(IOM_RED, IOM_CYAN) : IOM_ATTR(IOM_YELLOW, IOM_BLACK), -1);
        }
    }
    if (m->nfiles == 0 && item == IOM_BROWSE_CARD)
        iom_puts(s, top + 4, left + 4, "Aucun fichier (dossiers roms, roms/loci, disks, tapes, .)",
                 IOM_ATTR(IOM_RED, IOM_BLACK), width - 8);
    else if (m->nfiles == 0)
        iom_puts(s, top + 4, left + 4, drive ? "Aucune image .dsk (dossiers disks, tapes, snapshots, .)"
                                       : tape ? "Aucune cassette .tap (dossiers tapes, disks, .)"
                                              : "Aucun instantané .ost (dossiers snapshots, .)",
                 IOM_ATTR(IOM_RED, IOM_BLACK), width - 8);
    if (n > IOM_BROWSE_VISIBLE) {   /* scroll bar */
        const int bar = left + width - 2;
        for (int k = 0; k < IOM_BROWSE_VISIBLE; k++) s_putc(s, top + 2 + k, bar, IOM_SHADE, IOM_ATTR(IOM_BLUE, IOM_BLACK));
        const int thumb = m->browse_scroll * (IOM_BROWSE_VISIBLE - 1) / (n - IOM_BROWSE_VISIBLE);
        s_putc(s, top + 2 + thumb, bar, IOM_FULL, IOM_ATTR(IOM_CYAN, IOM_BLACK));
    }
}

/* Displayed value of a parameter (« — » if empty, « oui »/« non »). */
static const char* param_text(const card_param_t* p, const char* v) {
    (void)p;
    return v[0] ? v : "—";
}

static void draw_cards_page(const iom_menu_t* m, iom_surface_t* s) {
    const int n = cards_count();
    char buf[128];
    panel(s, 5, 2, n + 4, 76, IOM_CART_L, m->cards_readonly ? "Cartes d'extension (lecture seule)"
                                                            : "Cartes d'extension");
    for (int i = 0; i < n; i++) {
        const card_desc_t* d = cards_get(i);
        const card_choice_t* c = &m->cards.card[i];
        const int row = 7 + i;
        const bool sel = !m->browsing && m->card_cursor == i;
        const row_attrs_t ra = attrs_for(sel);
        item_bar(s, row, 4, 72, sel);
        s_putc(s, row, 6, c->on ? IOM_DOT : IOM_CROSS, c->on ? ra.ok : ra.dim);
        iom_puts(s, row, 8, d->name, c->on ? ra.acc : ra.dim, 17);
        const bool changed = c->on != m->cards_orig.card[i].on ||
                             memcmp(c->value, m->cards_orig.card[i].value, sizeof(c->value)) != 0;
        iom_puts(s, row, 26, d->fixed ? "toujours" : c->on ? "présente" : "absente",
                 c->on ? ra.ok : ra.dim, -1);
        if (changed) iom_puts(s, row, 35, "*", ra.err, -1);
        if (c->on && d->nparams > 0 && (d->enable_param >= 0 || d->io_param >= 0)) {
            const int p = d->enable_param >= 0 ? d->enable_param : d->io_param;
            snprintf(buf, sizeof(buf), "%s", param_text(&d->param[p], c->value[p]));
            iom_puts(s, row, 37, buf, ra.base, 38);
        } else if (d->group) {
            snprintf(buf, sizeof(buf), "(une seule carte « %s »)", d->group);
            iom_puts(s, row, 37, buf, ra.dim, 38);
        }
    }
    /* Role of the card under the cursor. */
    const int top = 9 + n;
    panel(s, top, 2, 6, 76, 0, "Rôle de la carte");
    if (m->card_cursor < n) wrap(s, top + 1, 4, 72, 4, cards_get(m->card_cursor)->role, A_PANEL);
    else wrap(s, top + 1, 4, 72, 4, m->card_cursor == n
              ? "Relance l'émulateur avec ces cartes (redémarrage à froid : la mémoire est "
                "effacée). Les cartes marquées * ont changé. « Enregistrer la configuration » "
                "les garde pour les prochains lancements."
              : "Revient aux cartes de la machine en cours.", A_PANEL);
    char why[96];
    if (cards_conflict(&m->cards, why, sizeof(why))) {
        s_putc(s, top + 6, 3, IOM_CROSS, IOM_ATTR(IOM_RED, IOM_BLACK));
        iom_puts(s, top + 6, 5, why, IOM_ATTR(IOM_RED, IOM_BLACK), 72);
    }
    button(s, 34, 6, 32, "Appliquer et redémarrer", !m->browsing && m->card_cursor == n);
    button(s, 34, 42, 32, "Annuler les changements", !m->browsing && m->card_cursor == n + 1);
}

static void draw_card_page(const iom_menu_t* m, iom_surface_t* s) {
    const card_desc_t* d = cards_get(m->card_sel);
    const card_choice_t* c = &m->cards.card[m->card_sel];
    char buf[160];
    panel(s, 5, 2, 7 + 2 * d->nparams + 2, 76, IOM_CART_L, d->name);
    wrap(s, 6, 4, 72, 3, d->role, A_PANEL_DIM);
    {   /* Presence */
        const bool sel = !m->browsing && m->param_cursor == 0;
        const row_attrs_t ra = attrs_for(sel);
        item_bar(s, 10, 4, 72, sel);
        iom_puts(s, 10, 6, "Carte", ra.acc, -1);
        state(s, 10, 33, c->on, d->fixed ? "toujours présente" : c->on ? "présente" : "absente", &ra);
    }
    for (int p = 0; p < d->nparams; p++) {
        const int row = 12 + 2 * p;
        const bool sel = !m->browsing && m->param_cursor == p + 1;
        const row_attrs_t ra = attrs_for(sel);
        item_bar(s, row, 4, 72, sel);
        iom_puts(s, row, 6, d->param[p].label, c->on ? ra.acc : ra.dim, 26);
        if (sel && m->editing) {
            int n = iom_puts(s, row, 33, m->edit, ra.base, 41);
            s_putc(s, row, 33 + n, IOM_FULL, ra.acc);
        } else {
            iom_puts(s, row, 33, param_text(&d->param[p], c->value[p]), ra.base, 42);
        }
    }
    /* Explanation of the parameter (or of the presence) under the cursor. */
    const int top = 14 + 2 * d->nparams;
    panel(s, top, 2, 8, 76, 0, m->param_cursor == 0 ? "La carte" : d->param[m->param_cursor - 1].label);
    if (m->param_cursor == 0) {
        if (d->fixed) snprintf(buf, sizeof(buf), "Toujours présente : rien à régler ici.");
        else if (d->group)
            snprintf(buf, sizeof(buf), "Entrée : présente / absente. Une seule carte du groupe "
                     "« %s » à la fois : en choisir une retire l'autre.", d->group);
        else snprintf(buf, sizeof(buf), "Entrée : présente / absente.");
        wrap(s, top + 1, 4, 72, 6, buf, A_PANEL);
    } else {
        const card_param_t* p = &d->param[m->param_cursor - 1];
        wrap(s, top + 1, 4, 72, 5, p->help, A_PANEL);
        snprintf(buf, sizeof(buf), "Par défaut : %s.  Entrée : %s.  Suppr : valeur par défaut.",
                 p->def[0] ? p->def : "vide",
                 p->kind == CARD_P_BOOL ? "oui / non" : p->kind == CARD_P_FILE ? "choisir le fichier"
                 : "saisir");
        wrap(s, top + 6, 4, 72, 1, buf, A_PANEL_DIM);
    }
}

void iom_draw(const iom_menu_t* m, iom_surface_t* s) {
    const iom_state_t* st = &m->st;
    char buf[96];
    s_clear(s, A_BG);

    /* Header */
    s_fill(s, 0, 0, 3, IOM_COLS, IOM_ATTR(IOM_WHITE, IOM_BLUE));
    s_puts_big(s, 1, 3, "PHOSPHORIC", IOM_ATTR(IOM_WHITE, IOM_BLUE));
    iom_puts(s, 1, 25, st->machine, IOM_ATTR(IOM_YELLOW, IOM_BLUE), 22);
    snprintf(buf, sizeof(buf), "Périphériques E/S   v%s", st->version);
    iom_puts(s, 1, IOM_COLS - 3 - u_strlen(buf), buf, IOM_ATTR(IOM_CYAN, IOM_BLUE), -1);
    for (int c = 0; c < IOM_COLS; c++) s_putc(s, 3, c, IOM_HLINE, IOM_ATTR(IOM_CYAN, IOM_BLACK));

    if (m->page == IOM_PAGE_CARDS) {
        draw_cards_page(m, s);
    } else if (m->page == IOM_PAGE_CARD) {
        draw_card_page(m, s);
    } else {
        draw_media(m, s);
        draw_cards(m, s);
        draw_devices(m, s);
        /* Buttons */
        static const char* const labels[3] = { "Redémarrer (RESET)", "Enregistrer la configuration", "Reprendre" };
        static const int bcol[3] = { 2, 27, 58 }, bw[3] = { 22, 29, 20 };
        for (int i = 0; i < 3; i++)
            button(s, 34, bcol[i], bw[i], labels[i], !m->browsing && m->cursor == IOM_ITEM_RESET + i);
    }

    /* Message of the last result */
    if (m->message[0]) {
        s_putc(s, 36, 3, m->message_error ? IOM_CROSS : IOM_CHECK,
               IOM_ATTR(m->message_error ? IOM_RED : IOM_GREEN, IOM_BLACK));
        iom_puts(s, 36, 5, m->message, IOM_ATTR(m->message_error ? IOM_RED : IOM_YELLOW, IOM_BLACK), 73);
    }

    /* Footer: key help */
    s_fill(s, 37, 0, 3, IOM_COLS, IOM_ATTR(IOM_WHITE, IOM_BLUE | IOM_DITHER));
    const uint8_t key = IOM_ATTR(IOM_BLACK, IOM_CYAN), txt = IOM_ATTR(IOM_WHITE, IOM_BLUE | IOM_DITHER);
    static const char* const help_main[4][2] = { { " Flèches ", "choisir" }, { " Entrée ", "activer" },
                                                 { " Suppr ", "éjecter" }, { " Échap ", "reprendre" } };
    static const char* const help_browse[4][2] = { { " Flèches ", "choisir" }, { " Entrée ", "valider" },
                                                   { " Lettre ", "aller à" }, { " Échap ", "retour" } };
    static const char* const help_cards[4][2] = { { " Flèches ", "choisir" }, { " Entrée ", "détails" },
                                                  { " ", "" }, { " Échap ", "retour" } };
    static const char* const help_card[4][2] = { { " Flèches ", "choisir" }, { " Entrée ", "modifier" },
                                                 { " Suppr ", "défaut" }, { " Échap ", "retour" } };
    static const char* const help_edit[4][2] = { { " Clavier ", "saisir" }, { " Entrée ", "valider" },
                                                 { " Suppr ", "effacer" }, { " Échap ", "annuler" } };
    const char* const (*help)[2] = m->browsing ? help_browse : m->editing ? help_edit
                                 : m->page == IOM_PAGE_CARD ? help_card
                                 : m->page == IOM_PAGE_CARDS ? help_cards : help_main;
    int c = 2;
    for (int i = 0; i < 4; i++) {
        if (!help[i][1][0]) continue;
        c += iom_puts(s, 38, c, help[i][0], key, -1) + 1;
        c += iom_puts(s, 38, c, help[i][1], txt, -1) + 3;
    }
    iom_puts(s, 39, IOM_COLS - 30, "F1 : ouvrir / fermer ce menu", IOM_ATTR(IOM_CYAN, IOM_BLUE | IOM_DITHER), -1);

    if (m->browsing) draw_browser(m, s);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  RGB888 rasterisation (doubled lines; checkerboard dithering on the grid
 *  line, like the Neo6502)
 * ═══════════════════════════════════════════════════════════════════════ */

void iom_rasterize(const iom_surface_t* s, uint8_t* rgb) {
    for (int line = 0; line < IOM_ROWS * 8; line++) {
        const int row = line >> 3, y = line & 7;
        uint8_t* out = rgb + (size_t)(line * 2) * IOM_WIDTH * 3;
        for (int col = 0; col < IOM_COLS; col++) {
            uint8_t bits = iom_font[s->ch[row][col]][y];
            if (s->big[row][col]) {   /* large letters: pixels doubled in width */
                uint16_t wide = 0;
                for (int i = 0; i < 8; i++)
                    if (bits >> i & 1) wide |= (uint16_t)(3u << (2 * i));
                bits = s->big[row][col] == 1 ? (uint8_t)(wide & 0xFF) : (uint8_t)(wide >> 8);
            }
            const uint8_t a = s->attr[row][col];
            const int ink = a & 7, paper = (a >> 4) & 7;
            const bool dither = (a & 0x80) != 0;
            for (int x = 0; x < 8; x++) {
                const int px = col * 8 + x;
                int color;
                if (bits >> x & 1) color = ink;
                else if (dither && ((px + line) & 1)) color = IOM_BLACK;   /* every other pixel */
                else color = paper;
                uint8_t* p = out + (size_t)px * 3;
                p[0] = (color & 1) ? 255 : 0;
                p[1] = (color & 2) ? 255 : 0;
                p[2] = (color & 4) ? 255 : 0;
            }
        }
        memcpy(out + (size_t)IOM_WIDTH * 3, out, (size_t)IOM_WIDTH * 3);   /* doubled line */
    }
}
