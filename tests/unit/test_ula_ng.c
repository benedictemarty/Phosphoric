/**
 * @file test_ula_ng.c
 * @brief ULA-NG étape 1 : déverrouillage NG_LOCK/NG_ID + garde verrou.
 * @author bmarty <bmarty@mailo.com>
 *
 * Vérifie : état verrouillé au reset (indiscernable, passthrough VIA), séquence
 * de déverrouillage 'N','G', handshake NG_ID/NG_IDCHK, robustesse (séquences
 * cassées), re-verrouillage au reset. Cf docs/ula-ng/AUDIT.md, ULA-NG-SPEC.md §3.
 */

#include <stdio.h>
#include <string.h>
#include "io/ula_ng.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: expected 0x%X, got 0x%X\n", __FILE__, __LINE__, (unsigned)(b), (unsigned)(a)); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); tests_failed++; return; } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) { printf("FAIL\n    %s:%d: expected false\n", __FILE__, __LINE__); tests_failed++; return; } \
} while(0)

/* Déverrouille proprement un module fraîchement reset. */
static void unlock(ula_ng_t* u) {
    ula_ng_write(u, ULA_NG_REG_LOCK, ULA_NG_UNLOCK_N);
    ula_ng_write(u, ULA_NG_REG_LOCK, ULA_NG_UNLOCK_G);
}

TEST(test_reset_is_locked) {
    ula_ng_t u; ula_ng_init(&u);
    ASSERT_FALSE(ula_ng_active(&u));   /* verrouillé → n'intercepte pas */
}

TEST(test_addr_window) {
    ASSERT_FALSE(ula_ng_addr_in_window(0x033F));
    ASSERT_TRUE(ula_ng_addr_in_window(0x0340));
    ASSERT_TRUE(ula_ng_addr_in_window(0x034F));
    ASSERT_TRUE(ula_ng_addr_in_window(0x035F));
    ASSERT_FALSE(ula_ng_addr_in_window(0x0360));
    ASSERT_FALSE(ula_ng_addr_in_window(0x0300));   /* VIA, pas ULA-NG */
}

TEST(test_locked_writes_passthrough) {
    /* Verrouillé : toute écriture fenêtre renvoie 0 (passthrough VIA). */
    ula_ng_t u; ula_ng_init(&u);
    ASSERT_EQ(ula_ng_write(&u, ULA_NG_REG_LOCK, ULA_NG_UNLOCK_N), 0);
    ASSERT_EQ(ula_ng_write(&u, 0x0341, 0xAB), 0);
    ASSERT_EQ(ula_ng_write(&u, 0x035F, 0xCD), 0);
}

TEST(test_unlock_sequence) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ASSERT_TRUE(ula_ng_active(&u));
    ASSERT_EQ(ula_ng_read(&u, ULA_NG_REG_LOCK),  ULA_NG_VERSION);         /* NG_ID = 0x1E */
    ASSERT_EQ(ula_ng_read(&u, ULA_NG_REG_IDCHK), (uint8_t)~ULA_NG_VERSION); /* NG_IDCHK = 0xE1 */
    /* handshake : NG_ID XOR NG_IDCHK == 0xFF */
    ASSERT_EQ(ula_ng_read(&u, ULA_NG_REG_LOCK) ^ ula_ng_read(&u, ULA_NG_REG_IDCHK), 0xFF);
}

TEST(test_unlocked_writes_consumed) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    /* déverrouillé : écriture d'un registre → consommée (renvoie 1) + relue */
    ASSERT_EQ(ula_ng_write(&u, 0x0341, 0x5A), 1);
    ASSERT_EQ(ula_ng_read(&u, 0x0341), 0x5A);
    /* NG_ID/NG_IDCHK restent en lecture seule malgré une écriture */
    ASSERT_EQ(ula_ng_write(&u, ULA_NG_REG_LOCK, 0x99), 1);
    ASSERT_EQ(ula_ng_read(&u, ULA_NG_REG_LOCK), ULA_NG_VERSION);
}

TEST(test_wrong_second_byte) {
    /* 'N' puis autre chose que 'G' → reste verrouillé */
    ula_ng_t u; ula_ng_init(&u);
    ula_ng_write(&u, ULA_NG_REG_LOCK, ULA_NG_UNLOCK_N);
    ula_ng_write(&u, ULA_NG_REG_LOCK, 0x00);
    ASSERT_FALSE(ula_ng_active(&u));
    ula_ng_write(&u, ULA_NG_REG_LOCK, ULA_NG_UNLOCK_G);   /* 'G' seul ne suffit pas */
    ASSERT_FALSE(ula_ng_active(&u));
}

TEST(test_sequence_broken_by_window_write) {
    /* 'N', puis une écriture ailleurs dans la fenêtre casse la séquence (SPEC §3) */
    ula_ng_t u; ula_ng_init(&u);
    ula_ng_write(&u, ULA_NG_REG_LOCK, ULA_NG_UNLOCK_N);
    ula_ng_write(&u, 0x0345, 0x12);                      /* écriture intercalée */
    ula_ng_write(&u, ULA_NG_REG_LOCK, ULA_NG_UNLOCK_G);
    ASSERT_FALSE(ula_ng_active(&u));
}

TEST(test_reset_relocks) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ASSERT_TRUE(ula_ng_active(&u));
    ula_ng_write(&u, 0x0341, 0x77);
    ula_ng_reset(&u);                                    /* reset matériel */
    ASSERT_FALSE(ula_ng_active(&u));
    ASSERT_EQ(u.regs[0x0341 - ULA_NG_WINDOW_LO], 0);     /* registres remis à 0 */
}

/* ── Étape 2 : palette-indirection (§5.1) ─────────────────────────────── */

TEST(test_palette_default_identity) {
    /* Au reset : LUT 0-7 = couleurs Oric (identité). */
    ula_ng_t u; ula_ng_init(&u);
    ASSERT_EQ(u.pal[0][0], 0x00); ASSERT_EQ(u.pal[0][1], 0x00); ASSERT_EQ(u.pal[0][2], 0x00);  /* noir */
    ASSERT_EQ(u.pal[1][0], 0xFF); ASSERT_EQ(u.pal[1][1], 0x00); ASSERT_EQ(u.pal[1][2], 0x00);  /* rouge */
    ASSERT_EQ(u.pal[7][0], 0xFF); ASSERT_EQ(u.pal[7][1], 0xFF); ASSERT_EQ(u.pal[7][2], 0xFF);  /* blanc */
}

TEST(test_palette_gating) {
    ula_ng_t u; ula_ng_init(&u);
    ASSERT_FALSE(u.active);                 /* verrouillé */
    unlock(&u);
    ASSERT_FALSE(u.active);                 /* déverrouillé mais NG_MODE.b0=0 */
    ula_ng_write(&u, ULA_NG_REG_MODE, 0x01);    /* NG_MODE.b0 = 1 */
    ASSERT_TRUE(u.active);
    ula_ng_write(&u, ULA_NG_REG_MODE, 0x00);    /* désactive */
    ASSERT_FALSE(u.active);
    ula_ng_write(&u, ULA_NG_REG_MODE, 0x01);
    ula_ng_reset(&u);                           /* reset re-verrouille */
    ASSERT_FALSE(u.active);
}

TEST(test_palette_program) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    /* programmer l'entrée 2 = ($F,$0,$0) -> RGB888 (FF,00,00) */
    ula_ng_write(&u, ULA_NG_REG_PAL_IDX, 2);
    ula_ng_write(&u, ULA_NG_REG_PAL_LO, 0x0F);      /* 0000RRRR : R=15 */
    ula_ng_write(&u, ULA_NG_REG_PAL_HI, 0x00);      /* GGGGBBBB : G=0,B=0 -> commit */
    ASSERT_EQ(u.pal[2][0], 0xFF); ASSERT_EQ(u.pal[2][1], 0x00); ASSERT_EQ(u.pal[2][2], 0x00);
    /* auto-incrément : l'index passe à 3 */
    ASSERT_EQ(u.pal_idx, 3);
}

TEST(test_palette_expand_nibble) {
    /* RGB444 -> RGB888 par réplication : $8 -> $88, $A -> $AA, $F -> $FF */
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_PAL_IDX, 5);
    ula_ng_write(&u, ULA_NG_REG_PAL_LO, 0x08);      /* R=8 */
    ula_ng_write(&u, ULA_NG_REG_PAL_HI, 0xAF);      /* G=A, B=F */
    ASSERT_EQ(u.pal[5][0], 0x88);
    ASSERT_EQ(u.pal[5][1], 0xAA);
    ASSERT_EQ(u.pal[5][2], 0xFF);
}

TEST(test_palette_locked_no_effect) {
    /* Verrouillé : écrire les registres palette ne change rien (passthrough). */
    ula_ng_t u; ula_ng_init(&u);
    ula_ng_write(&u, ULA_NG_REG_PAL_IDX, 3);
    ula_ng_write(&u, ULA_NG_REG_PAL_LO, 0x0F);
    ula_ng_write(&u, ULA_NG_REG_PAL_HI, 0xFF);
    ASSERT_EQ(u.pal[3][0], 0xFF); ASSERT_EQ(u.pal[3][1], 0xFF); ASSERT_EQ(u.pal[3][2], 0x00); /* Oric jaune inchangé */
    ASSERT_EQ(u.pal_idx, 0);
}

/* ── Étape 3 : IRQ raster (§5.2) ──────────────────────────────────────── */

/* Déverrouille + arme l'IRQ raster à la ligne `line`. */
static void arm_raster(ula_ng_t* u, int line) {
    unlock(u);
    ula_ng_write(u, ULA_NG_REG_MODE, ULA_NG_MODE_ENABLE);   /* NG_MODE.b0 */
    ula_ng_write(u, ULA_NG_REG_RASTER, (uint8_t)line);      /* NG_RASTERLINE */
    ula_ng_write(u, ULA_NG_REG_STATUS, ULA_NG_STATUS_EN);   /* enable IRQ */
}

TEST(test_raster_fires_at_line) {
    ula_ng_t u; ula_ng_init(&u);
    arm_raster(&u, 100);
    ula_ng_scanline(&u, 99);
    ASSERT_FALSE(ula_ng_irq(&u));               /* pas encore */
    ula_ng_scanline(&u, 100);
    ASSERT_TRUE(ula_ng_irq(&u));                /* IRQ levée */
    ASSERT_EQ(ula_ng_read(&u, ULA_NG_REG_STATUS), ULA_NG_STATUS_IRQ);  /* b7 */
}

TEST(test_raster_level_until_ack) {
    ula_ng_t u; ula_ng_init(&u);
    arm_raster(&u, 50);
    ula_ng_scanline(&u, 50);
    ASSERT_TRUE(ula_ng_irq(&u));
    ula_ng_scanline(&u, 60);                    /* d'autres lignes : reste levée */
    ASSERT_TRUE(ula_ng_irq(&u));
    ula_ng_write(&u, ULA_NG_REG_STATUS, ULA_NG_STATUS_EN);  /* acquit (reste armée) */
    ASSERT_FALSE(ula_ng_irq(&u));               /* acquittée */
    ASSERT_EQ(ula_ng_read(&u, ULA_NG_REG_STATUS), 0x00);
    ula_ng_scanline(&u, 50);                    /* re-déclenche à la ligne */
    ASSERT_TRUE(ula_ng_irq(&u));
}

TEST(test_raster_wrong_line) {
    ula_ng_t u; ula_ng_init(&u);
    arm_raster(&u, 100);
    for (int y = 0; y < 312; y++) if (y != 100) ula_ng_scanline(&u, y);
    ASSERT_FALSE(ula_ng_irq(&u));
}

TEST(test_raster_gating) {
    ula_ng_t u; ula_ng_init(&u);
    /* verrouillé : aucune IRQ */
    ula_ng_scanline(&u, 0);
    ASSERT_FALSE(ula_ng_irq(&u));
    /* déverrouillé + NG_MODE.b0 mais enable=0 : aucune IRQ */
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_MODE, ULA_NG_MODE_ENABLE);
    ula_ng_write(&u, ULA_NG_REG_RASTER, 10);
    ula_ng_scanline(&u, 10);
    ASSERT_FALSE(ula_ng_irq(&u));
    /* enable mais NG_MODE.b0=0 : aucune IRQ */
    ula_ng_write(&u, ULA_NG_REG_STATUS, ULA_NG_STATUS_EN);
    ula_ng_write(&u, ULA_NG_REG_MODE, 0x00);
    ula_ng_scanline(&u, 10);
    ASSERT_FALSE(ula_ng_irq(&u));
}

TEST(test_raster_reset_clears) {
    ula_ng_t u; ula_ng_init(&u);
    arm_raster(&u, 20);
    ula_ng_scanline(&u, 20);
    ASSERT_TRUE(ula_ng_irq(&u));
    ula_ng_reset(&u);
    ASSERT_FALSE(ula_ng_irq(&u));
    ASSERT_FALSE(u.raster_enable);
}

/* ── Étape 4 : start-address (§5.3) ───────────────────────────────────── */

TEST(test_scrstart_default_zero) {
    ula_ng_t u; ula_ng_init(&u);
    ASSERT_EQ(u.scrstart, 0);                   /* 0 = base par défaut */
}

TEST(test_scrstart_program) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_SCR_LO, 0x00);  /* LSB */
    ula_ng_write(&u, ULA_NG_REG_SCR_HI, 0x90);  /* MSB -> $9000 */
    ASSERT_EQ(u.scrstart, 0x9000);
    /* modifier seulement le LSB */
    ula_ng_write(&u, ULA_NG_REG_SCR_LO, 0x28);
    ASSERT_EQ(u.scrstart, 0x9028);
}

TEST(test_scrstart_locked_no_effect) {
    ula_ng_t u; ula_ng_init(&u);
    ula_ng_write(&u, ULA_NG_REG_SCR_LO, 0xA8);  /* verrouillé : passthrough */
    ula_ng_write(&u, ULA_NG_REG_SCR_HI, 0xBB);
    ASSERT_EQ(u.scrstart, 0);
}

TEST(test_scrstart_reset_clears) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_SCR_LO, 0x00);
    ula_ng_write(&u, ULA_NG_REG_SCR_HI, 0xA0);
    ASSERT_EQ(u.scrstart, 0xA000);
    ula_ng_reset(&u);
    ASSERT_EQ(u.scrstart, 0);
}

/* ── Étape 5 : palette par scanline / copper (§5.4) ───────────────────── */

/* Programme une entrée copper (ligne, index, R4,G4,B4). */
static void cop_entry(ula_ng_t* u, int line, int idx, int r, int g, int b) {
    ula_ng_write(u, ULA_NG_REG_COP_DATA, (uint8_t)line);
    ula_ng_write(u, ULA_NG_REG_COP_DATA, (uint8_t)((idx << 4) | (r & 0x0F)));
    ula_ng_write(u, ULA_NG_REG_COP_DATA, (uint8_t)(((g & 0x0F) << 4) | (b & 0x0F)));
}

TEST(test_copper_program) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_MODE, ULA_NG_MODE_ENABLE);
    ula_ng_write(&u, ULA_NG_REG_COP_CTRL, 0);       /* reset liste */
    cop_entry(&u, 10, 3, 0xF, 0, 0);                /* ligne 10, index 3, rouge */
    ASSERT_EQ(u.copper_count, 1);
    ASSERT_EQ(u.copper[0].line, 10);
    ASSERT_EQ(u.copper[0].index, 3);
    ASSERT_EQ(u.copper[0].r, 0xFF); ASSERT_EQ(u.copper[0].g, 0x00); ASSERT_EQ(u.copper[0].b, 0x00);
}

TEST(test_copper_apply_at_line) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_MODE, ULA_NG_MODE_ENABLE);
    ula_ng_write(&u, ULA_NG_REG_COP_CTRL, 0);
    cop_entry(&u, 50, 7, 0, 0xF, 0);                /* ligne 50 : couleur 7 -> vert */
    ula_ng_scanline(&u, 49);
    ASSERT_EQ(u.pal[7][1], 0xFF);                   /* pal[7] = blanc (identité) */
    ASSERT_EQ(u.pal[7][0], 0xFF);
    ula_ng_scanline(&u, 50);                        /* applique l'entrée */
    ASSERT_EQ(u.pal[7][0], 0x00); ASSERT_EQ(u.pal[7][1], 0xFF); ASSERT_EQ(u.pal[7][2], 0x00);
}

TEST(test_copper_gradient) {
    /* Même index, couleurs différentes selon la ligne. */
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_MODE, ULA_NG_MODE_ENABLE);
    ula_ng_write(&u, ULA_NG_REG_COP_CTRL, 0);
    cop_entry(&u, 0,   1, 0xF, 0, 0);               /* ligne 0 : couleur 1 rouge */
    cop_entry(&u, 100, 1, 0, 0, 0xF);               /* ligne 100 : couleur 1 bleu */
    ula_ng_scanline(&u, 0);
    ASSERT_EQ(u.pal[1][0], 0xFF); ASSERT_EQ(u.pal[1][2], 0x00);   /* rouge */
    ula_ng_scanline(&u, 100);
    ASSERT_EQ(u.pal[1][0], 0x00); ASSERT_EQ(u.pal[1][2], 0xFF);   /* bleu */
}

TEST(test_copper_gating) {
    ula_ng_t u; ula_ng_init(&u);
    /* verrouillé : le flux copper est passthrough, rien programmé */
    ula_ng_write(&u, ULA_NG_REG_COP_CTRL, 0);
    cop_entry(&u, 10, 0, 0xF, 0xF, 0xF);
    ASSERT_EQ(u.copper_count, 0);
    /* déverrouillé + copper programmé mais NG_MODE.b0=0 : scanline n'applique pas */
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_COP_CTRL, 0);
    cop_entry(&u, 5, 2, 0xF, 0, 0);
    ula_ng_scanline(&u, 5);
    ASSERT_EQ(u.pal[2][0], 0x00);                   /* couleur 2 (vert Oric) inchangée */
}

TEST(test_copper_reset) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_MODE, ULA_NG_MODE_ENABLE);
    ula_ng_write(&u, ULA_NG_REG_COP_CTRL, 0);
    cop_entry(&u, 1, 0, 0xF, 0, 0);
    ASSERT_EQ(u.copper_count, 1);
    ula_ng_write(&u, ULA_NG_REG_COP_CTRL, 0);       /* reset liste */
    ASSERT_EQ(u.copper_count, 0);
    cop_entry(&u, 2, 0, 0, 0xF, 0);
    ula_ng_reset(&u);                               /* reset matériel */
    ASSERT_EQ(u.copper_count, 0);
}

/* ── Étape 6 : scroll fin X/Y (§5.5) ──────────────────────────────────── */

TEST(test_scroll_default_zero) {
    ula_ng_t u; ula_ng_init(&u);
    ASSERT_EQ(u.scrollx, 0); ASSERT_EQ(u.scrolly, 0);
}

TEST(test_scroll_program) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_SCROLLX, 3);
    ula_ng_write(&u, ULA_NG_REG_SCROLLY, 5);
    ASSERT_EQ(u.scrollx, 3); ASSERT_EQ(u.scrolly, 5);
}

TEST(test_scroll_clamp) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_SCROLLX, 9);   /* X clampé à 5 (cellule 6 px) */
    ula_ng_write(&u, ULA_NG_REG_SCROLLY, 9);   /* Y masqué 0-7 : 9&7 = 1 */
    ASSERT_EQ(u.scrollx, 5); ASSERT_EQ(u.scrolly, 1);
}

TEST(test_scroll_locked_no_effect) {
    ula_ng_t u; ula_ng_init(&u);
    ula_ng_write(&u, ULA_NG_REG_SCROLLX, 4);
    ula_ng_write(&u, ULA_NG_REG_SCROLLY, 6);
    ASSERT_EQ(u.scrollx, 0); ASSERT_EQ(u.scrolly, 0);
}

/* ── Étape 7 : attributs parallèles (§5.6) ────────────────────────────── */

TEST(test_attr_default) {
    ula_ng_t u; ula_ng_init(&u);
    ASSERT_FALSE(u.attr_active);
    ASSERT_EQ(u.attr[0], 0); ASSERT_EQ(u.attr[8191], 0);
}

TEST(test_attr_gating) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ASSERT_FALSE(u.attr_active);                     /* NG_MODE.b1=0 */
    ula_ng_write(&u, ULA_NG_REG_MODE, ULA_NG_MODE_ATTR);  /* b1 */
    ASSERT_TRUE(u.attr_active);
    ula_ng_write(&u, ULA_NG_REG_MODE, 0);
    ASSERT_FALSE(u.attr_active);
}

TEST(test_attr_fill) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_ATTR_FILL, 0x1A);    /* paper=3, ink=2 */
    ASSERT_EQ(u.attr[0], 0x1A); ASSERT_EQ(u.attr[100], 0x1A); ASSERT_EQ(u.attr[8191], 0x1A);
    ASSERT_EQ(u.attr_wp, 0);
}

TEST(test_attr_stream) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_ATTR_FILL, 0x00);    /* reset ptr + plan à 0 */
    ula_ng_write(&u, ULA_NG_REG_ATTR_DATA, 0x11);
    ula_ng_write(&u, ULA_NG_REG_ATTR_DATA, 0x22);
    ula_ng_write(&u, ULA_NG_REG_ATTR_DATA, 0x33);
    ASSERT_EQ(u.attr[0], 0x11); ASSERT_EQ(u.attr[1], 0x22); ASSERT_EQ(u.attr[2], 0x33);
    ASSERT_EQ(u.attr_wp, 3);
}

TEST(test_attr_locked_no_effect) {
    ula_ng_t u; ula_ng_init(&u);
    ula_ng_write(&u, ULA_NG_REG_ATTR_FILL, 0xFF);    /* verrouillé : passthrough */
    ASSERT_EQ(u.attr[0], 0);
    ASSERT_FALSE(u.attr_active);
}

/* ── Étape 8 : sprites matériels (§5.7) ─────────────────────────────────── */

static uint8_t g_fb[32 * 32 * 3];

TEST(test_spr_default) {
    ula_ng_t u; ula_ng_init(&u);
    ASSERT_FALSE(u.spr_active);
    ASSERT_FALSE(u.spr_collision);
    ASSERT_FALSE(u.sprites[0].enable);
}

TEST(test_spr_gating) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ASSERT_FALSE(u.spr_active);
    ula_ng_write(&u, ULA_NG_REG_SPR_CTRL, 0x01);   /* enable global */
    ASSERT_TRUE(u.spr_active);
    ula_ng_write(&u, ULA_NG_REG_SPR_CTRL, 0x00);
    ASSERT_FALSE(u.spr_active);
}

TEST(test_spr_program) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_SPR_SEL, 3);
    ula_ng_write(&u, ULA_NG_REG_SPR_X, 40);
    ula_ng_write(&u, ULA_NG_REG_SPR_Y, 50);
    ula_ng_write(&u, ULA_NG_REG_SPR_ATTR, 0x01);
    ula_ng_write(&u, ULA_NG_REG_SPR_DATA, 5);
    ula_ng_write(&u, ULA_NG_REG_SPR_DATA, 2);
    ASSERT_EQ(u.sprites[3].x, 40);
    ASSERT_EQ(u.sprites[3].y, 50);
    ASSERT_TRUE(u.sprites[3].enable);
    ASSERT_EQ(u.sprites[3].pattern[0], 5);
    ASSERT_EQ(u.sprites[3].pattern[1], 2);
    ASSERT_EQ(u.spr_wp, 2);
    ula_ng_write(&u, ULA_NG_REG_SPR_SEL, 3);   /* SEL remet le pointeur motif à 0 */
    ASSERT_EQ(u.spr_wp, 0);
}

TEST(test_spr_composite_and_transparency) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_SPR_CTRL, 0x01);
    ula_ng_write(&u, ULA_NG_REG_SPR_SEL, 0);
    ula_ng_write(&u, ULA_NG_REG_SPR_X, 4);
    ula_ng_write(&u, ULA_NG_REG_SPR_Y, 4);
    ula_ng_write(&u, ULA_NG_REG_SPR_ATTR, 0x01);
    ula_ng_write(&u, ULA_NG_REG_SPR_DATA, 0);   /* px (row0,col0) transparent */
    ula_ng_write(&u, ULA_NG_REG_SPR_DATA, 1);   /* px (row0,col1) index 1 = rouge */
    memset(g_fb, 0, sizeof(g_fb));
    ula_ng_composite_scanline(&u, g_fb, 32, 32, 4);   /* y=4 = row0 du sprite */
    int o0 = (4 * 32 + 4) * 3, o1 = (4 * 32 + 5) * 3;
    ASSERT_EQ(g_fb[o0], 0); ASSERT_EQ(g_fb[o0 + 1], 0); ASSERT_EQ(g_fb[o0 + 2], 0);
    ASSERT_EQ(g_fb[o1], u.pal[1][0]);
    ASSERT_EQ(g_fb[o1 + 1], u.pal[1][1]);
    ASSERT_EQ(g_fb[o1 + 2], u.pal[1][2]);
}

TEST(test_spr_collision) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_SPR_CTRL, 0x01);
    for (int s = 0; s < 2; s++) {               /* 2 sprites opaques superposés */
        ula_ng_write(&u, ULA_NG_REG_SPR_SEL, (uint8_t)s);
        ula_ng_write(&u, ULA_NG_REG_SPR_X, 8);
        ula_ng_write(&u, ULA_NG_REG_SPR_Y, 8);
        ula_ng_write(&u, ULA_NG_REG_SPR_ATTR, 0x01);
        ula_ng_write(&u, ULA_NG_REG_SPR_DATA, 1);
    }
    ASSERT_FALSE(u.spr_collision);
    memset(g_fb, 0, sizeof(g_fb));
    ula_ng_composite_scanline(&u, g_fb, 32, 32, 8);
    ASSERT_TRUE(u.spr_collision);
    ASSERT_EQ(ula_ng_read(&u, ULA_NG_REG_SPR_STATUS), ULA_NG_SPR_STATUS_COL);
    ASSERT_EQ(ula_ng_read(&u, ULA_NG_REG_SPR_STATUS), 0x00);   /* clear on read */
    ASSERT_FALSE(u.spr_collision);
}

TEST(test_spr_locked_no_composite) {
    ula_ng_t u; ula_ng_init(&u);
    ula_ng_write(&u, ULA_NG_REG_SPR_CTRL, 0x01);   /* verrouillé : passthrough */
    ASSERT_FALSE(u.spr_active);
    memset(g_fb, 0xAB, sizeof(g_fb));
    ula_ng_composite_scanline(&u, g_fb, 32, 32, 0);   /* no-op */
    ASSERT_EQ(g_fb[0], 0xAB);
}

/* ── Étape 9 : modes vidéo étendus §5.8 (chunky 4bpp / texte 80col) ──────── */

TEST(test_mode_default) {
    ula_ng_t u; ula_ng_init(&u);
    ASSERT_FALSE(u.chunky_active);
    ASSERT_FALSE(u.text80_active);
}

TEST(test_mode_chunky) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_MODE, 0x05);   /* b0 (enable) + b2-3=01 (chunky) */
    ASSERT_TRUE(u.chunky_active);
    ASSERT_FALSE(u.text80_active);
}

TEST(test_mode_text80) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_MODE, 0x09);   /* b0 (enable) + b2-3=10 (80col) */
    ASSERT_TRUE(u.text80_active);
    ASSERT_FALSE(u.chunky_active);
}

TEST(test_mode_needs_enable) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    ula_ng_write(&u, ULA_NG_REG_MODE, 0x04);   /* b2 sans b0 : extensions inactives */
    ASSERT_FALSE(u.chunky_active);
    ASSERT_FALSE(u.text80_active);
}

TEST(test_mode_locked_no_effect) {
    ula_ng_t u; ula_ng_init(&u);
    ula_ng_write(&u, ULA_NG_REG_MODE, 0x05);   /* verrouillé : passthrough */
    ASSERT_FALSE(u.chunky_active);
    ASSERT_FALSE(u.text80_active);
}

/* ── VDU intégré (docs/ula-ng/VDU.md) ───────────────────────────────────── */

static void vdu(ula_ng_t* u, uint8_t b) { ula_ng_write(u, ULA_NG_REG_VDU, b); }

TEST(test_vdu_mode) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    vdu(&u, 22); vdu(&u, 1);          /* MODE 1 = chunky */
    ASSERT_TRUE(u.chunky_active);
    vdu(&u, 22); vdu(&u, 2);          /* MODE 2 = 80col */
    ASSERT_TRUE(u.text80_active);
    ASSERT_FALSE(u.chunky_active);
    vdu(&u, 22); vdu(&u, 0);          /* MODE 0 = std */
    ASSERT_FALSE(u.chunky_active);
    ASSERT_FALSE(u.text80_active);
    ASSERT_TRUE(u.active);            /* b0 actif */
}

TEST(test_vdu_palette) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    vdu(&u, 19); vdu(&u, 7); vdu(&u, 15); vdu(&u, 0); vdu(&u, 0);  /* LUT[7] = rouge */
    ASSERT_EQ(u.pal[7][0], 0xFF); ASSERT_EQ(u.pal[7][1], 0x00); ASSERT_EQ(u.pal[7][2], 0x00);
}

TEST(test_vdu_fill) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    vdu(&u, 18); vdu(&u, 0x21);       /* fond couleur par cellule = $21 */
    ASSERT_TRUE(u.attr_active);
    ASSERT_EQ(u.attr[0], 0x21); ASSERT_EQ(u.attr[100], 0x21);
}

TEST(test_vdu_cell) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    vdu(&u, 31); vdu(&u, 5); vdu(&u, 2); vdu(&u, 0x1A);  /* col5, row2 -> scanlines 16-23 */
    ASSERT_TRUE(u.attr_active);
    ASSERT_EQ(u.attr[16 * 40 + 5], 0x1A);
    ASSERT_EQ(u.attr[23 * 40 + 5], 0x1A);
    ASSERT_EQ(u.attr[15 * 40 + 5], 0x00);   /* hors cellule : inchangé */
}

TEST(test_vdu_reset) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    vdu(&u, 22); vdu(&u, 1);          /* chunky */
    ASSERT_TRUE(u.chunky_active);
    vdu(&u, 20);                      /* reset -> normal */
    ASSERT_FALSE(u.chunky_active);
    ASSERT_FALSE(u.active);
}

TEST(test_vdu_unknown_ignored) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    vdu(&u, 99);                      /* commande inconnue : ignorée, pas de crash */
    vdu(&u, 22); vdu(&u, 1);          /* le flux reste sain ensuite */
    ASSERT_TRUE(u.chunky_active);
}

TEST(test_vdu_locked_no_effect) {
    ula_ng_t u; ula_ng_init(&u);
    vdu(&u, 22); vdu(&u, 1);          /* verrouillé : passthrough, aucun effet */
    ASSERT_FALSE(u.chunky_active);
}

/* VDU v0.2 : graphiques dans la VRAM portée par l'ULA-NG */

TEST(test_vdu_gfx_clg) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    u.vram[0] = 0xAB;                 /* sale */
    vdu(&u, 16);                      /* CLG */
    ASSERT_TRUE(u.vram_active);
    ASSERT_TRUE(u.chunky_active);
    ASSERT_EQ(u.vram[0], 0x00);
}

TEST(test_vdu_gfx_plot) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    vdu(&u, 16);
    vdu(&u, 17); vdu(&u, 5);          /* couleur 5 */
    vdu(&u, 25); vdu(&u, 10); vdu(&u, 20);  /* PLOT (10,20) : x pair -> quartet haut */
    ASSERT_EQ(u.vram[20 * 80 + 5], 0x50);
    vdu(&u, 17); vdu(&u, 7);
    vdu(&u, 25); vdu(&u, 11); vdu(&u, 20);  /* PLOT (11,20) : x impair -> quartet bas */
    ASSERT_EQ(u.vram[20 * 80 + 5], 0x57);
}

TEST(test_vdu_gfx_line) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    vdu(&u, 16);
    vdu(&u, 17); vdu(&u, 3);
    vdu(&u, 26); vdu(&u, 0); vdu(&u, 0); vdu(&u, 4); vdu(&u, 0);  /* DRAW (0,0)-(4,0) */
    ASSERT_EQ(u.vram[0], 0x33);       /* px (0,0)+(1,0) */
    ASSERT_EQ(u.vram[1], 0x33);       /* px (2,0)+(3,0) */
    ASSERT_EQ((u.vram[2] >> 4) & 0x0F, 3);  /* px (4,0) */
}

TEST(test_vdu_gfx_reset) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    vdu(&u, 16);
    ASSERT_TRUE(u.vram_active);
    vdu(&u, 20);                      /* reset -> VRAM désactivée */
    ASSERT_FALSE(u.vram_active);
}

/* VDU v0.3 : protocole d'upload (sprite via flux) */

TEST(test_vdu_sprite_upload) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    vdu(&u, 23); vdu(&u, 0);          /* begin upload motif sprite 0 */
    for (int i = 0; i < 256; i++) vdu(&u, 3);  /* motif = index 3 */
    ASSERT_EQ(u.vdu_upload, 0);       /* upload terminé */
    ASSERT_EQ(u.sprites[0].pattern[0], 3);
    ASSERT_EQ(u.sprites[0].pattern[255], 3);
    vdu(&u, 24); vdu(&u, 0); vdu(&u, 40); vdu(&u, 50); vdu(&u, 1);  /* pos + enable */
    ASSERT_EQ(u.sprites[0].x, 40);
    ASSERT_EQ(u.sprites[0].y, 50);
    ASSERT_TRUE(u.sprites[0].enable);
    ASSERT_TRUE(u.spr_active);        /* enable global posé par VDU 24 */
}

TEST(test_vdu_upload_then_command) {
    ula_ng_t u; ula_ng_init(&u);
    unlock(&u);
    vdu(&u, 23); vdu(&u, 1);
    for (int i = 0; i < 256; i++) vdu(&u, 5);
    vdu(&u, 22); vdu(&u, 1);          /* stream fini -> commande normale reconnue */
    ASSERT_TRUE(u.chunky_active);
    ASSERT_EQ(u.sprites[1].pattern[10], 5);
}

int main(void) {
    printf("\n=== ULA-NG unit tests (steps 1-9 + VDU) ===\n\n");
    RUN(test_reset_is_locked);
    RUN(test_addr_window);
    RUN(test_locked_writes_passthrough);
    RUN(test_unlock_sequence);
    RUN(test_unlocked_writes_consumed);
    RUN(test_wrong_second_byte);
    RUN(test_sequence_broken_by_window_write);
    RUN(test_reset_relocks);
    RUN(test_palette_default_identity);
    RUN(test_palette_gating);
    RUN(test_palette_program);
    RUN(test_palette_expand_nibble);
    RUN(test_palette_locked_no_effect);
    RUN(test_raster_fires_at_line);
    RUN(test_raster_level_until_ack);
    RUN(test_raster_wrong_line);
    RUN(test_raster_gating);
    RUN(test_raster_reset_clears);
    RUN(test_scrstart_default_zero);
    RUN(test_scrstart_program);
    RUN(test_scrstart_locked_no_effect);
    RUN(test_scrstart_reset_clears);
    RUN(test_copper_program);
    RUN(test_copper_apply_at_line);
    RUN(test_copper_gradient);
    RUN(test_copper_gating);
    RUN(test_copper_reset);
    RUN(test_scroll_default_zero);
    RUN(test_scroll_program);
    RUN(test_scroll_clamp);
    RUN(test_scroll_locked_no_effect);
    RUN(test_attr_default);
    RUN(test_attr_gating);
    RUN(test_attr_fill);
    RUN(test_attr_stream);
    RUN(test_attr_locked_no_effect);
    RUN(test_spr_default);
    RUN(test_spr_gating);
    RUN(test_spr_program);
    RUN(test_spr_composite_and_transparency);
    RUN(test_spr_collision);
    RUN(test_spr_locked_no_composite);
    RUN(test_mode_default);
    RUN(test_mode_chunky);
    RUN(test_mode_text80);
    RUN(test_mode_needs_enable);
    RUN(test_mode_locked_no_effect);
    RUN(test_vdu_mode);
    RUN(test_vdu_palette);
    RUN(test_vdu_fill);
    RUN(test_vdu_cell);
    RUN(test_vdu_reset);
    RUN(test_vdu_unknown_ignored);
    RUN(test_vdu_locked_no_effect);
    RUN(test_vdu_gfx_clg);
    RUN(test_vdu_gfx_plot);
    RUN(test_vdu_gfx_line);
    RUN(test_vdu_gfx_reset);
    RUN(test_vdu_sprite_upload);
    RUN(test_vdu_upload_then_command);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
