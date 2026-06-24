/**
 * @file test_osd.c
 * @brief Tests de l'OSD (overlay changement de média) — logique + rendu.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "video/osd.h"
#include "video/video.h"

static int tests_passed = 0, tests_failed = 0;
#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-46s", #name); name(); } while (0)
#define ASSERT_TRUE(x) do { if (!(x)) { printf("FAIL (%s:%d: %s)\n", __FILE__, __LINE__, #x); tests_failed++; return; } } while (0)
#define ASSERT_EQ(a,b) do { if ((long)(a)!=(long)(b)) { printf("FAIL (%s:%d: %s != %s -> %ld != %ld)\n", __FILE__, __LINE__, #a,#b,(long)(a),(long)(b)); tests_failed++; return; } } while (0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while (0)

/* Crée un dossier temporaire avec quelques médias. */
static void make_media_dir(const char* dir) {
    mkdir(dir, 0755);
    const char* files[] = { "zeta.tap", "alpha.tap", "game.dsk", "readme.txt", NULL };
    for (int i = 0; files[i]; i++) {
        char p[512]; snprintf(p, sizeof(p), "%s/%s", dir, files[i]);
        FILE* f = fopen(p, "wb"); if (f) { fputc('X', f); fclose(f); }
    }
}

TEST(test_osd_scan_filters_and_sorts) {
    const char* dir = "/tmp/phosphoric_osd_test";
    make_media_dir(dir);
    osd_t osd; osd_init(&osd);
    const char* dirs[] = { dir, NULL };
    osd_scan(&osd, dirs);
    /* 3 médias (2 .tap + 1 .dsk), le .txt ignoré */
    ASSERT_EQ(osd.count, 3);
    /* cassettes d'abord, triées alpha : alpha.tap, zeta.tap, puis game.dsk */
    ASSERT_TRUE(!osd.entries[0].is_disk);
    ASSERT_TRUE(strcmp(osd.entries[0].name, "alpha.tap") == 0);
    ASSERT_TRUE(strcmp(osd.entries[1].name, "zeta.tap") == 0);
    ASSERT_TRUE(osd.entries[2].is_disk);
    ASSERT_TRUE(strcmp(osd.entries[2].name, "game.dsk") == 0);
    PASS();
}

TEST(test_osd_navigation_clamps) {
    osd_t osd; osd_init(&osd);
    const char* dir = "/tmp/phosphoric_osd_test";
    const char* dirs[] = { dir, NULL };
    osd_scan(&osd, dirs);
    osd.open = true;
    ASSERT_EQ(osd.selected, 0);
    osd_key(&osd, OSD_KEY_UP);              /* déjà en haut : reste 0 */
    ASSERT_EQ(osd.selected, 0);
    osd_key(&osd, OSD_KEY_DOWN);
    ASSERT_EQ(osd.selected, 1);
    osd_key(&osd, OSD_KEY_DOWN);
    osd_key(&osd, OSD_KEY_DOWN);            /* clamp au dernier (index 2) */
    ASSERT_EQ(osd.selected, 2);
    PASS();
}

TEST(test_osd_enter_returns_activate) {
    osd_t osd; osd_init(&osd);
    const char* dir = "/tmp/phosphoric_osd_test";
    const char* dirs[] = { dir, NULL };
    osd_scan(&osd, dirs);
    osd.open = true;
    ASSERT_EQ(osd_key(&osd, OSD_KEY_ENTER), OSD_ACTIVATE);
    /* Échap ferme et signale la fermeture */
    ASSERT_EQ(osd_key(&osd, OSD_KEY_ESC), OSD_CLOSED);
    ASSERT_TRUE(!osd.open);
    PASS();
}

TEST(test_osd_key_ignored_when_closed) {
    osd_t osd; osd_init(&osd);
    ASSERT_EQ(osd_key(&osd, OSD_KEY_ENTER), OSD_NONE);
    PASS();
}

TEST(test_osd_render_draws_text) {
    osd_t osd; osd_init(&osd);
    const char* dir = "/tmp/phosphoric_osd_test";
    const char* dirs[] = { dir, NULL };
    osd_scan(&osd, dirs);
    osd.open = true;
    /* police synthétique : chaque glyphe = toutes lignes pleines (0x3F) */
    for (int i = 0; i < 128 * 8; i++) osd.font[i] = 0x3F;
    osd.font_ready = true;

    video_t* vid = calloc(1, sizeof(video_t));
    vid->native_w = 240; vid->native_h = 224;
    /* fond blanc pour vérifier l'assombrissement + le texte */
    memset(vid->framebuffer, 255, (size_t)vid->native_w * vid->native_h * 3);

    osd_render(&osd, vid);

    /* Le panneau doit avoir assombri une grande partie de l'écran. */
    long dark = 0, lit = 0;
    for (int i = 0; i < vid->native_w * vid->native_h * 3; i += 3) {
        if (vid->framebuffer[i] < 80) dark++;
        else lit++;
    }
    ASSERT_TRUE(dark > 1000);   /* le panneau translucide a assombri */
    ASSERT_TRUE(lit > 100);     /* du texte (pixels clairs) a été dessiné */
    free(vid);
    PASS();
}

int main(void) {
    printf("Running OSD tests...\n");
    printf("===========================================================\n");
    RUN(test_osd_scan_filters_and_sorts);
    RUN(test_osd_navigation_clamps);
    RUN(test_osd_enter_returns_activate);
    RUN(test_osd_key_ignored_when_closed);
    RUN(test_osd_render_draws_text);
    printf("===========================================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
