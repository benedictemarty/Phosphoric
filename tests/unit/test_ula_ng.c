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

int main(void) {
    printf("\n=== ULA-NG unit tests (step 1: unlock/lock) ===\n\n");
    RUN(test_reset_is_locked);
    RUN(test_addr_window);
    RUN(test_locked_writes_passthrough);
    RUN(test_unlock_sequence);
    RUN(test_unlocked_writes_consumed);
    RUN(test_wrong_second_byte);
    RUN(test_sequence_broken_by_window_write);
    RUN(test_reset_relocks);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
