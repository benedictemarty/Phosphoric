/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_iomenu.c
 * @brief Tests of the I/O peripherals menu (F1): navigation, actions,
 *        file picker, drawing and rasterisation.
 * @author bmarty <bmarty@mailo.com>
 *
 * IOM_PPM=file.ppm also writes the rendering of the main page (preview).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "video/iomenu.h"
#include "video/iom_font.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    int before = tests_failed; \
    printf("  %-52s", #name); \
    name(); \
    if (tests_failed == before) { tests_passed++; printf("PASS\n"); } \
} while (0)
#define FAIL_AT() do { printf("FAIL\n    %s:%d\n", __FILE__, __LINE__); tests_failed++; return; } while (0)
#define ASSERT_TRUE(x) do { if (!(x)) FAIL_AT(); } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) { \
    printf("FAIL\n    %s:%d: %lld != %lld\n", __FILE__, __LINE__, (long long)(a), (long long)(b)); \
    tests_failed++; return; } } while (0)

static iom_menu_t m;          /* large (file list): off the stack */
static iom_surface_t surf;

static void fill_state(iom_menu_t* mm) {
    iom_state_t* st = &mm->st;
    st->version = "2.4.0";
    st->machine = "Oric Atmos";
    st->disk_iface = "Microdisc";
    st->drives = 4;
    snprintf(st->drive[0], sizeof(st->drive[0]), "Citadelle.dsk");
    st->drive_ro[1] = true;
    snprintf(st->drive[1], sizeof(st->drive[1]), "SEDORIC3.dsk");
    snprintf(st->tape, sizeof(st->tape), "AIGLE.TAP");
    st->tape_percent = 40;
    st->tape_fast = true;
    st->printer = IOM_PRINTER_TEXT;
    snprintf(st->printer_file, sizeof(st->printer_file), "impression.txt");
    st->joystick = IOM_JOY_KEYS;
    st->cards = 2;
    snprintf(st->card[0].name, 24, "Microdisc");
    st->card[0].present = true;
    snprintf(st->card[0].detail, 40, "$0310  microdis.rom");
    snprintf(st->card[1].name, 24, "LOCI");
    st->card[1].present = false;
}

/* Looks for an ASCII text on a row of the surface. */
static bool row_has(const iom_surface_t* s, int row, const char* text) {
    size_t n = strlen(text);
    for (int c = 0; c + (int)n <= IOM_COLS; c++)
        if (memcmp(&s->ch[row][c], text, n) == 0) return true;
    return false;
}
static bool surf_has(const iom_surface_t* s, const char* text) {
    for (int r = 0; r < IOM_ROWS; r++) if (row_has(s, r, text)) return true;
    return false;
}

TEST(test_open_starts_on_resume) {
    iom_init(&m);
    ASSERT_TRUE(!m.open);
    iom_open(&m);
    ASSERT_TRUE(m.open);
    ASSERT_EQ(m.cursor, IOM_ITEM_RESUME);
    iom_action_t a = iom_key(&m, IOM_KEY_ENTER);
    ASSERT_EQ(a.type, IOM_ACT_RESUME);
    a = iom_key(&m, IOM_KEY_ESC);
    ASSERT_EQ(a.type, IOM_ACT_RESUME);
}

TEST(test_up_down_wrap) {
    iom_init(&m); iom_open(&m);
    iom_key(&m, IOM_KEY_DOWN);
    ASSERT_EQ(m.cursor, IOM_ITEM_DRIVE0);           /* after Resume: back to the top */
    iom_key(&m, IOM_KEY_UP);
    ASSERT_EQ(m.cursor, IOM_ITEM_RESUME);
    iom_key(&m, IOM_KEY_HOME);
    ASSERT_EQ(m.cursor, 0);
    iom_key(&m, IOM_KEY_END);
    ASSERT_EQ(m.cursor, IOM_ITEMS - 1);
}

TEST(test_left_right_columns) {
    iom_init(&m); iom_open(&m);
    m.cursor = IOM_ITEM_JOYSTICK;
    iom_key(&m, IOM_KEY_RIGHT);
    ASSERT_EQ(m.cursor, IOM_ITEM_KEYBOARD);
    iom_key(&m, IOM_KEY_LEFT);
    ASSERT_EQ(m.cursor, IOM_ITEM_JOYSTICK);
    m.cursor = IOM_ITEM_PRINTER;
    iom_key(&m, IOM_KEY_RIGHT);
    ASSERT_EQ(m.cursor, IOM_ITEM_PRINTER);          /* full-width line: no column */
    m.cursor = IOM_ITEM_RESET;
    iom_key(&m, IOM_KEY_RIGHT);
    ASSERT_EQ(m.cursor, IOM_ITEM_SAVE);
    iom_key(&m, IOM_KEY_RIGHT);
    ASSERT_EQ(m.cursor, IOM_ITEM_RESUME);
    iom_key(&m, IOM_KEY_RIGHT);
    ASSERT_EQ(m.cursor, IOM_ITEM_RESUME);          /* edge: does not wrap */
}

TEST(test_toggles_return_actions) {
    iom_init(&m); iom_open(&m);
    static const struct { int item; iom_act_type_t act; } t[] = {
        { IOM_ITEM_PRINTER, IOM_ACT_PRINTER_CYCLE }, { IOM_ITEM_JOYSTICK, IOM_ACT_JOYSTICK_CYCLE },
        { IOM_ITEM_KEYBOARD, IOM_ACT_KEYBOARD_TOGGLE }, { IOM_ITEM_TAPE_FAST, IOM_ACT_TAPE_FAST_TOGGLE },
        { IOM_ITEM_RESET, IOM_ACT_RESET }, { IOM_ITEM_SAVE, IOM_ACT_SAVE_CONFIG },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        m.cursor = t[i].item;
        ASSERT_EQ(iom_key(&m, IOM_KEY_ENTER).type, t[i].act);
    }
}

TEST(test_drive_columns_protect_and_eject) {
    iom_init(&m); fill_state(&m); iom_open(&m);
    m.cursor = IOM_ITEM_DRIVE0 + 2;
    iom_key(&m, IOM_KEY_RIGHT);                    /* protection column */
    ASSERT_EQ(m.sub, 1);
    iom_action_t a = iom_key(&m, IOM_KEY_ENTER);
    ASSERT_EQ(a.type, IOM_ACT_DISK_PROTECT);
    ASSERT_EQ(a.target, 2);
    a = iom_key(&m, IOM_KEY_DEL);
    ASSERT_EQ(a.type, IOM_ACT_DISK_EJECT);
    ASSERT_EQ(a.target, 2);
    iom_key(&m, IOM_KEY_DOWN);                     /* drive D: column kept */
    ASSERT_EQ(m.sub, 1);
    m.cursor = IOM_ITEM_TAPE;
    ASSERT_EQ(iom_key(&m, IOM_KEY_ENTER).type, IOM_ACT_TAPE_REWIND);
    iom_key(&m, IOM_KEY_DOWN);                     /* outside media: column reset to 0 */
    ASSERT_EQ(m.sub, 0);
}

TEST(test_drive_without_interface_refused) {
    iom_init(&m); iom_open(&m);                    /* drives = 0 */
    m.cursor = IOM_ITEM_DRIVE0;
    iom_action_t a = iom_key(&m, IOM_KEY_ENTER);
    ASSERT_EQ(a.type, IOM_ACT_NONE);
    ASSERT_TRUE(!m.browsing);
    ASSERT_TRUE(m.message_error);
}

/* Temporary media directory for the file picker. */
static char tmpdir[64];
static const char* dirs[2];
static void make_media(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/iomenu_test_%d", (int)getpid());
    mkdir(tmpdir, 0700);
    static const char* const names[] = { "Citadelle.dsk", "zork.dsk", "AIGLE.TAP", "hello.tap", "etat0001.ost", "x.txt" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char p[128];
        snprintf(p, sizeof(p), "%s/%s", tmpdir, names[i]);
        FILE* f = fopen(p, "wb");
        if (f) { fwrite("0123456789", 1, 10, f); fclose(f); }
    }
    dirs[0] = tmpdir; dirs[1] = NULL;
}
static void rm_media(void) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd) != 0) { /* best-effort cleanup */ }
}

TEST(test_browser_disk_insert) {
    iom_init(&m); fill_state(&m); iom_set_dirs(&m, dirs); iom_open(&m);
    m.cursor = IOM_ITEM_DRIVE0 + 2;                /* drive C, empty */
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_TRUE(m.browsing);
    ASSERT_EQ(m.nfiles, 2);                        /* only the .dsk files */
    ASSERT_EQ(m.browse_cursor, 0);                 /* « Éjecter » (Eject) */
    iom_key(&m, 'z');                              /* jump to initial */
    ASSERT_TRUE(strcmp(m.files[m.browse_cursor - 1].name, "zork.dsk") == 0);
    iom_action_t a = iom_key(&m, IOM_KEY_ENTER);
    ASSERT_EQ(a.type, IOM_ACT_DISK_INSERT);
    ASSERT_EQ(a.target, 2);
    ASSERT_TRUE(strstr(a.path, "zork.dsk") != NULL);
    ASSERT_TRUE(!m.browsing);
}

TEST(test_browser_refuses_image_in_other_drive) {
    iom_init(&m); fill_state(&m); iom_set_dirs(&m, dirs); iom_open(&m);
    m.cursor = IOM_ITEM_DRIVE0 + 3;
    iom_key(&m, IOM_KEY_ENTER);
    iom_key(&m, 'c');                              /* Citadelle.dsk, already in A */
    iom_action_t a = iom_key(&m, IOM_KEY_ENTER);
    ASSERT_EQ(a.type, IOM_ACT_NONE);
    ASSERT_TRUE(m.message_error);
    ASSERT_TRUE(m.browsing);                       /* stays in the file picker */
}

TEST(test_browser_tape_and_snapshot) {
    iom_init(&m); fill_state(&m); iom_set_dirs(&m, dirs); iom_open(&m);
    m.cursor = IOM_ITEM_TAPE;
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_EQ(m.nfiles, 2);                        /* .tap, case ignored */
    ASSERT_TRUE(m.browse_cursor > 0);              /* cursor on the inserted cassette */
    ASSERT_TRUE(strcmp(m.files[m.browse_cursor - 1].name, "AIGLE.TAP") == 0);
    iom_key(&m, IOM_KEY_HOME);
    ASSERT_EQ(iom_key(&m, IOM_KEY_ENTER).type, IOM_ACT_TAPE_EJECT);
    m.cursor = IOM_ITEM_SNAPSHOT;
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_EQ(m.nfiles, 1);
    ASSERT_EQ(iom_key(&m, IOM_KEY_ENTER).type, IOM_ACT_SNAPSHOT_SAVE);
    iom_key(&m, IOM_KEY_ENTER);
    iom_key(&m, IOM_KEY_DOWN);
    iom_action_t a = iom_key(&m, IOM_KEY_ENTER);
    ASSERT_EQ(a.type, IOM_ACT_SNAPSHOT_LOAD);
    ASSERT_TRUE(strstr(a.path, "etat0001.ost") != NULL);
    m.cursor = IOM_ITEM_TAPE;
    iom_key(&m, IOM_KEY_ENTER);
    iom_key(&m, IOM_KEY_ESC);                      /* back without action */
    ASSERT_TRUE(!m.browsing);
    ASSERT_TRUE(m.open);
}

TEST(test_draw_main_page) {
    iom_init(&m); fill_state(&m); iom_open(&m);
    iom_message(&m, false, "Disque A: Citadelle.dsk");
    iom_draw(&m, &surf);
    ASSERT_TRUE(surf_has(&surf, "Oric Atmos"));
    ASSERT_TRUE(surf_has(&surf, "Citadelle.dsk"));
    ASSERT_TRUE(surf_has(&surf, "AIGLE.TAP"));
    ASSERT_TRUE(surf_has(&surf, "Microdisc"));
    ASSERT_TRUE(surf_has(&surf, "impression.txt"));
    ASSERT_TRUE(surf_has(&surf, "QWERTY"));
    ASSERT_TRUE(surf_has(&surf, "Reprendre"));
    ASSERT_TRUE(surf_has(&surf, "Disque A: Citadelle.dsk"));
    /* Large header letters: « P » over two cells. */
    ASSERT_EQ(surf.ch[1][3], 'P');
    ASSERT_EQ(surf.big[1][3], 1);
    ASSERT_EQ(surf.big[1][4], 2);
    /* Accents converted to Latin-1 (é = 0xE9). */
    ASSERT_TRUE(surf_has(&surf, "P\xE9riph\xE9riques"));
}

TEST(test_utf8_special_glyphs) {
    memset(&surf, 0, sizeof(surf));
    int n = iom_puts(&surf, 0, 0, "— … → é", 0x07, -1);
    ASSERT_EQ(n, 7);
    ASSERT_EQ(surf.ch[0][0], IOM_EMDASH);
    ASSERT_EQ(surf.ch[0][2], IOM_ELLIPSIS);
    ASSERT_EQ(surf.ch[0][4], IOM_ARROW_R);
    ASSERT_EQ(surf.ch[0][6], 0xE9);
}

TEST(test_rasterize_colors) {
    static uint8_t rgb[IOM_WIDTH * IOM_HEIGHT * 3];
    iom_init(&m); fill_state(&m); iom_open(&m);
    iom_draw(&m, &surf);
    iom_rasterize(&surf, rgb);
    /* Header: solid blue paper (pixel (0,0)). */
    ASSERT_EQ(rgb[0], 0); ASSERT_EQ(rgb[1], 0); ASSERT_EQ(rgb[2], 255);
    /* Doubled lines: line 1 = line 0. */
    ASSERT_TRUE(memcmp(rgb, rgb + IOM_WIDTH * 3, IOM_WIDTH * 3) == 0);
    /* Dithered panel: two neighbouring pixels of an empty cell alternate blue / black. */
    const int y = 6 * 16, x = 60 * 8;              /* row 6, inside the floppy panel */
    const uint8_t* p0 = rgb + ((size_t)y * IOM_WIDTH + x) * 3;
    const uint8_t* p1 = p0 + 3;
    ASSERT_TRUE(p0[2] != p1[2]);
    if (getenv("IOM_PPM")) {
        FILE* f = fopen(getenv("IOM_PPM"), "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", IOM_WIDTH, IOM_HEIGHT);
            fwrite(rgb, 1, sizeof(rgb), f);
            fclose(f);
        }
    }
}

TEST(test_closed_menu_ignores_keys) {
    iom_init(&m);
    ASSERT_EQ(iom_key(&m, IOM_KEY_ENTER).type, IOM_ACT_NONE);
}

/* Expansion cards: list page (dynamic, from the registry), card page,
 * exclusive toggle, checked input, file browser, Apply. */
TEST(test_cards_pages) {
    iom_init(&m);
    fill_state(&m);
    cards_state_defaults(&m.cards);
    const int md = cards_find("microdisc"), lo = cards_find("loci"), ac = cards_find("acia");
    cards_set_on(&m.cards, md, true);
    m.cards_orig = m.cards;
    iom_open(&m);
    m.cursor = IOM_ITEM_CARDS;
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_EQ(m.page, IOM_PAGE_CARDS);
    iom_draw(&m, &surf);
    ASSERT_TRUE(surf_has(&surf, "Microdisc") && surf_has(&surf, "MEA8000"));
    ASSERT_TRUE(surf_has(&surf, "Appliquer et red"));
    /* Nothing changed: Apply does not restart. */
    m.card_cursor = cards_count();
    ASSERT_EQ(iom_key(&m, IOM_KEY_ENTER).type, IOM_ACT_NONE);
    /* LOCI: card page, presence → the Microdisc goes away (same group). */
    m.card_cursor = lo;
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_EQ(m.page, IOM_PAGE_CARD);
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_TRUE(m.cards.card[lo].on && !m.cards.card[md].on);
    iom_draw(&m, &surf);
    ASSERT_TRUE(surf_has(&surf, "LOCI") && surf_has(&surf, "menu LOCI"));
    /* File parameter: browser; « Aucun fichier » clears the value. */
    m.param_cursor = 2;                                   /* SD image */
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_TRUE(m.browsing && m.browse_target == IOM_BROWSE_CARD);
    m.browse_cursor = 0;
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_TRUE(!m.browsing && m.cards.card[lo].value[1][0] == '\0');
    iom_key(&m, IOM_KEY_ESC);
    ASSERT_EQ(m.page, IOM_PAGE_CARDS);
    /* ACIA: address input, checked (0300-03FF). */
    m.card_cursor = ac;
    iom_key(&m, IOM_KEY_ENTER);
    iom_key(&m, IOM_KEY_ENTER);                           /* present */
    m.param_cursor = 2;                                   /* address */
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_TRUE(m.editing);
    for (int i = 0; i < 4; i++) iom_key(&m, IOM_KEY_DEL);
    iom_text(&m, "1234");
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_TRUE(m.editing && m.message_error);            /* rejected */
    for (int i = 0; i < 4; i++) iom_key(&m, IOM_KEY_DEL);
    iom_text(&m, "0380");
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_TRUE(!m.editing);
    ASSERT_TRUE(strcmp(m.cards.card[ac].value[1], "0380") == 0);
    iom_key(&m, IOM_KEY_DEL);                             /* Del: default value */
    ASSERT_TRUE(strcmp(m.cards.card[ac].value[1], "031C") == 0);
    iom_key(&m, IOM_KEY_ESC);
    /* Apply: restart action; refused when read-only (web). */
    m.card_cursor = cards_count();
    m.cards_readonly = true;
    ASSERT_EQ(iom_key(&m, IOM_KEY_ENTER).type, IOM_ACT_NONE);
    m.cards_readonly = false;
    ASSERT_EQ(iom_key(&m, IOM_KEY_ENTER).type, IOM_ACT_CARDS_APPLY);
    /* Cancel: back to the running machine. */
    m.card_cursor = cards_count() + 1;
    iom_key(&m, IOM_KEY_ENTER);
    ASSERT_TRUE(m.cards.card[md].on && !m.cards.card[lo].on);
}

int main(void) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Menu des périphériques E/S (F1)\n");
    printf("═══════════════════════════════════════════════════════\n");
    make_media();
    RUN(test_open_starts_on_resume);
    RUN(test_up_down_wrap);
    RUN(test_left_right_columns);
    RUN(test_toggles_return_actions);
    RUN(test_drive_columns_protect_and_eject);
    RUN(test_drive_without_interface_refused);
    RUN(test_browser_disk_insert);
    RUN(test_browser_refuses_image_in_other_drive);
    RUN(test_browser_tape_and_snapshot);
    RUN(test_draw_main_page);
    RUN(test_utf8_special_glyphs);
    RUN(test_rasterize_colors);
    RUN(test_closed_menu_ignores_keys);
    RUN(test_cards_pages);
    rm_media();
    printf("\n  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
