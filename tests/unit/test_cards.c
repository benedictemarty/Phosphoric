/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_cards.c
 * @brief Registre des cartes d'extension (menu F1) : état lu depuis la ligne
 *        de commande, groupes exclusifs, conflits d'E/S, options de relance,
 *        configuration
 * @author bmarty <bmarty@mailo.com>
 */
#include "cards.h"
#include "emulator.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { int b = tests_failed; printf("  %-52s", #name); name(); \
    if (tests_failed == b) { tests_passed++; printf("PASS\n"); } } while (0)
#define ASSERT_TRUE(x) do { if (!(x)) { \
    printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #x); tests_failed++; return; } } while (0)
#define ASSERT_STR(a, b) do { if (strcmp((a), (b)) != 0) { \
    printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
    tests_failed++; return; } } while (0)

/* Options reconstruites, jointes par des espaces (comparaison simple). */
static void joined(char** av, char* out, size_t sz) {
    out[0] = '\0';
    for (char** p = av; *p; p++) {
        if (p != av) strncat(out, " ", sz - strlen(out) - 1);
        strncat(out, *p, sz - strlen(out) - 1);
    }
}

TEST(test_registry_is_described) {
    ASSERT_TRUE(cards_count() >= 10);
    for (int i = 0; i < cards_count(); i++) {
        const card_desc_t* d = cards_get(i);
        ASSERT_TRUE(d->id && d->name && d->role && d->role[0]);
        for (int p = 0; p < d->nparams; p++)
            ASSERT_TRUE(d->param[p].label && d->param[p].help && d->param[p].help[0]);
        ASSERT_TRUE(cards_find(d->id) == i);
    }
    ASSERT_TRUE(cards_find("microdisc") >= 0 && cards_find("loci") >= 0);
    ASSERT_TRUE(cards_find("inexistante") < 0);
}

TEST(test_state_from_cli) {
    char* argv[] = { "oric1-emu", "-r", "roms/basic11b.rom", "--disk-rom", "roms/md.rom",
                     "--serial", "tcp:bbs:23", "--acia-addr=0380", "--mea8000", NULL };
    cards_state_t st;
    cards_state_from(&st, NULL, 9, argv);
    int md = cards_find("microdisc"), ac = cards_find("acia"), me = cards_find("mea8000");
    ASSERT_TRUE(st.card[md].on);
    ASSERT_STR(st.card[md].value[0], "roms/md.rom");
    ASSERT_TRUE(st.card[ac].on);
    ASSERT_STR(st.card[ac].value[0], "tcp:bbs:23");
    ASSERT_STR(st.card[ac].value[1], "0380");
    ASSERT_TRUE(st.card[me].on);
    ASSERT_TRUE(!st.card[cards_find("jasmin")].on);
    ASSERT_TRUE(st.card[cards_find("ula_ng")].on);          /* toujours présente */
}

TEST(test_disk_group_is_exclusive) {
    cards_state_t st;
    cards_state_defaults(&st);
    int md = cards_find("microdisc"), ja = cards_find("jasmin"), lo = cards_find("loci");
    cards_set_on(&st, md, true);
    cards_set_on(&st, lo, true);
    ASSERT_TRUE(st.card[lo].on && !st.card[md].on);
    cards_set_on(&st, ja, true);
    ASSERT_TRUE(st.card[ja].on && !st.card[lo].on);
    cards_set_on(&st, cards_find("ula_ng"), false);         /* fixe : inchangée */
    ASSERT_TRUE(st.card[cards_find("ula_ng")].on);
}

TEST(test_io_conflict_detected) {
    cards_state_t st;
    char msg[96];
    cards_state_defaults(&st);
    cards_set_on(&st, cards_find("mageco"), true);
    cards_set_on(&st, cards_find("mea8000"), true);          /* toutes deux en $03FE */
    ASSERT_TRUE(cards_conflict(&st, msg, sizeof msg));
    ASSERT_TRUE(strstr(msg, "Mageco MIDI") && strstr(msg, "MEA8000") && strstr(msg, "$03FE"));
    snprintf(st.card[cards_find("mea8000")].value[0], CARD_VALUE_MAX, "03F0");
    ASSERT_TRUE(!cards_conflict(&st, msg, sizeof msg));
}

TEST(test_build_argv_replaces_cards) {
    char* argv[] = { "oric1-emu", "-r", "roms/basic11b.rom", "--disk-rom", "roms/microdis.rom",
                     "-d", "jeu.dsk", "--loci-usb", "cle", "--fast", NULL };
    cards_state_t st;
    cards_state_from(&st, NULL, 10, argv);
    /* LOCI à la place du Microdisc, menu LOCI au démarrage. */
    cards_set_on(&st, cards_find("loci"), true);
    int argc2;
    char** av = cards_build_argv(&st, 10, argv, &argc2);
    char buf[512];
    joined(av, buf, sizeof buf);
    ASSERT_STR(buf, "oric1-emu -d jeu.dsk --loci-usb cle --fast -r roms/loci/locirom --loci "
                    "--no-config-cards");
    cards_argv_free(av);
    /* LOCI éteinte : sa ROM de menu redevient le BASIC, --loci-usb disparaît. */
    char* argv2[] = { "oric1-emu", "-r", "roms/loci/locirom", "--loci", "--loci-usb", "cle", NULL };
    cards_state_from(&st, NULL, 6, argv2);
    cards_set_on(&st, cards_find("loci"), false);
    av = cards_build_argv(&st, 6, argv2, &argc2);
    joined(av, buf, sizeof buf);
    ASSERT_STR(buf, "oric1-emu -r roms/basic11b.rom --no-config-cards");
    ASSERT_TRUE(argc2 == 4);
    cards_argv_free(av);
}

TEST(test_build_argv_params) {
    char* argv[] = { "oric1-emu", NULL };
    cards_state_t st;
    cards_state_defaults(&st);
    int ac = cards_find("acia");
    cards_set_on(&st, ac, true);
    snprintf(st.card[ac].value[0], CARD_VALUE_MAX, "picowifi");
    snprintf(st.card[ac].value[1], CARD_VALUE_MAX, "0380");
    snprintf(st.card[ac].value[4], CARD_VALUE_MAX, "oui");      /* V23 */
    char** av = cards_build_argv(&st, 1, argv, NULL);
    char buf[256];
    joined(av, buf, sizeof buf);
    ASSERT_STR(buf, "oric1-emu --serial picowifi --acia-addr 0380 --serial-v23 --no-config-cards");
    cards_argv_free(av);
}

TEST(test_cfg_roundtrip) {
    cards_state_t st, back;
    cards_state_defaults(&st);
    int sp = cards_find("sp0256");
    cards_set_on(&st, sp, true);
    snprintf(st.card[sp].value[1], CARD_VALUE_MAX, "03F2");
    FILE* f = tmpfile();
    ASSERT_TRUE(f != NULL);
    cards_cfg_write(&st, f);
    rewind(f);
    cards_state_defaults(&back);
    char line[256];
    bool seen = false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        char* eq = strchr(line, '=');
        ASSERT_TRUE(eq != NULL);
        *eq = '\0';
        ASSERT_TRUE(cards_cfg_line(&back, line, eq + 1, &seen));
    }
    fclose(f);
    ASSERT_TRUE(seen && back.card[sp].on);
    ASSERT_STR(back.card[sp].value[1], "03F2");
    ASSERT_TRUE(!cards_cfg_line(&back, "clavier", "azerty", NULL));   /* pas une carte */
}

int main(void) {
    printf("=== Registre des cartes d'extension ===\n");
    RUN(test_registry_is_described);
    RUN(test_state_from_cli);
    RUN(test_disk_group_is_exclusive);
    RUN(test_io_conflict_detected);
    RUN(test_build_argv_replaces_cards);
    RUN(test_build_argv_params);
    RUN(test_cfg_roundtrip);
    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
