/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_cards.c
 * @brief Registre des cartes d'extension (menu F1) : état lu depuis la ligne
 *        de commande, groupes exclusifs, conflits d'E/S, options de relance,
 *        configuration
 * @author bmarty <bmarty@mailo.com>
 */
#define _DEFAULT_SOURCE              /* symlink, setenv, mkstemp */
#include "cards.h"
#include "emulator.h"
#include "io/picowifi_detect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* /sys factice : ttyACM0 = autre montage TinyUSB, ttyACM1 = picowifi. */
static char g_sys[256];

static void fake_tty(const char* tty, const char* product) {
    char p[512], dev[512];
    snprintf(dev, sizeof dev, "%s/devices/usb1/%s", g_sys, tty);
    snprintf(p, sizeof p, "%s/1-1:1.0", dev);
    mkdir(dev, 0755); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/product", dev);
    FILE* f = fopen(p, "w");
    if (f) { fprintf(f, "%s\n", product); fclose(f); }
    snprintf(p, sizeof p, "%s/class/tty/%s", g_sys, tty);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/class/tty/%s/device", g_sys, tty);
    snprintf(dev, sizeof dev, "../../../devices/usb1/%s/1-1:1.0", tty);
    if (symlink(dev, p) != 0) perror("symlink");
}

static void fake_sysfs(bool with_picowifi) {
    char cmd[300];
    snprintf(g_sys, sizeof g_sys, "/tmp/phos_sysfs_%d", (int)getpid());
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/class/tty %s/devices/usb1",
             g_sys, g_sys, g_sys);
    if (system(cmd) != 0) return;
    fake_tty("ttyACM0", "TinyUSB Device");
    if (with_picowifi) fake_tty("ttyACM1", PICOWIFI_USB_PRODUCT);
    setenv("PHOSPHORIC_SYSFS_ROOT", g_sys, 1);
}

static void fake_sysfs_drop(void) {
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_sys);
    if (system(cmd) != 0) { /* rien */ }
    unsetenv("PHOSPHORIC_SYSFS_ROOT");
}

TEST(test_picowifi_detect_by_usb_product) {
    char dev[64];
    fake_sysfs(true);
    ASSERT_TRUE(picowifi_detect(NULL, dev, sizeof dev));
    ASSERT_STR(dev, "/dev/ttyACM1");                 /* pas ttyACM0 (autre produit) */
    fake_sysfs(false);
    ASSERT_TRUE(!picowifi_detect(NULL, dev, sizeof dev));
    ASSERT_TRUE(!picowifi_detect("/inexistant", dev, sizeof dev));
    picowifi_serial_spec("/dev/ttyACM1", dev, sizeof dev);
    ASSERT_STR(dev, "com:115200,8,N,1,/dev/ttyACM1");
    fake_sysfs_drop();
}

TEST(test_choice_param_cycles) {
    const card_desc_t* d = cards_get(cards_find("loci"));
    const card_param_t* p = NULL;
    for (int i = 0; i < d->nparams; i++) if (strcmp(d->param[i].key, "modem") == 0) p = &d->param[i];
    ASSERT_TRUE(p && p->kind == CARD_P_CHOICE && p->cli == NULL);
    char v[CARD_VALUE_MAX];
    cards_param_default(p, v, sizeof v);
    ASSERT_STR(v, LOCI_MODEM_NONE);
    cards_choice_next(p, v, sizeof v); ASSERT_STR(v, LOCI_MODEM_SIM);
    cards_choice_next(p, v, sizeof v); ASSERT_STR(v, LOCI_MODEM_REAL);
    cards_choice_next(p, v, sizeof v); ASSERT_STR(v, LOCI_MODEM_NONE);
    snprintf(v, sizeof v, "bizarre");
    cards_choice_next(p, v, sizeof v); ASSERT_STR(v, LOCI_MODEM_NONE);
}

TEST(test_loci_modem_build_argv) {
    char* argv[] = { "oric1-emu", "--serial-trace", "t.log", NULL };
    cards_state_t st;
    int lo = cards_find("loci"), ac = cards_find("acia");
    char buf[512];
    fake_sysfs(true);
    cards_state_defaults(&st);
    cards_set_on(&st, lo, true);
    snprintf(st.card[lo].value[0], CARD_VALUE_MAX, "non");
    snprintf(st.card[lo].value[3], CARD_VALUE_MAX, LOCI_MODEM_SIM);
    char** av = cards_build_argv(&st, 3, argv, NULL);
    joined(av, buf, sizeof buf);       /* --serial-trace gardée : le modem est une ACIA */
    ASSERT_STR(buf, "oric1-emu --serial-trace t.log --loci --serial picowifi --no-config-cards");
    cards_argv_free(av);
    snprintf(st.card[lo].value[3], CARD_VALUE_MAX, LOCI_MODEM_REAL);   /* port détecté */
    av = cards_build_argv(&st, 1, argv, NULL);
    joined(av, buf, sizeof buf);
    ASSERT_STR(buf, "oric1-emu --loci --serial com:115200,8,N,1,/dev/ttyACM1 --no-config-cards");
    cards_argv_free(av);
    snprintf(st.card[lo].value[4], CARD_VALUE_MAX, "/dev/ttyUSB3");     /* port imposé */
    av = cards_build_argv(&st, 1, argv, NULL);
    joined(av, buf, sizeof buf);
    ASSERT_STR(buf, "oric1-emu --loci --serial com:115200,8,N,1,/dev/ttyUSB3 --no-config-cards");
    cards_argv_free(av);
    /* Carte ACIA active : elle garde la ligne série, et le menu le signale. */
    char msg[96];
    ASSERT_TRUE(!cards_conflict(&st, msg, sizeof msg));
    cards_set_on(&st, ac, true);
    ASSERT_TRUE(cards_conflict(&st, msg, sizeof msg) && strstr(msg, "une seule ligne"));
    fake_sysfs_drop();
}

TEST(test_loci_modem_real_missing_is_reported) {
    cards_state_t st;
    int lo = cards_find("loci");
    char msg[96];
    fake_sysfs(false);
    cards_state_defaults(&st);
    cards_set_on(&st, lo, true);
    snprintf(st.card[lo].value[3], CARD_VALUE_MAX, LOCI_MODEM_REAL);
    ASSERT_TRUE(cards_conflict(&st, msg, sizeof msg) && strstr(msg, "aucun picowifi"));
    char* argv[] = { "oric1-emu", NULL };
    char** av = cards_build_argv(&st, 1, argv, NULL);
    char buf[256];
    joined(av, buf, sizeof buf);
    ASSERT_STR(buf, "oric1-emu -r roms/loci/locirom --loci --no-config-cards");
    cards_argv_free(av);
    snprintf(st.card[lo].value[4], CARD_VALUE_MAX, "/dev/ttyACM7");
    ASSERT_TRUE(!cards_conflict(&st, msg, sizeof msg));
    fake_sysfs_drop();
}

TEST(test_loci_modem_state_from_cli) {
    cards_state_t st;
    int lo = cards_find("loci"), ac = cards_find("acia");
    fake_sysfs(true);
    char* a1[] = { "oric1-emu", "--loci", "--serial", "picowifi", NULL };
    cards_state_from(&st, NULL, 4, a1);
    ASSERT_TRUE(st.card[lo].on && !st.card[ac].on);
    ASSERT_STR(st.card[lo].value[3], LOCI_MODEM_SIM);
    char* a2[] = { "oric1-emu", "--loci", "--serial", "com:115200,8,N,1,/dev/ttyACM1", NULL };
    cards_state_from(&st, NULL, 4, a2);                 /* port détecté : laissé vide */
    ASSERT_STR(st.card[lo].value[3], LOCI_MODEM_REAL);
    ASSERT_STR(st.card[lo].value[4], "");
    char* a3[] = { "oric1-emu", "--loci", "--serial", "com:115200,8,N,1,/dev/ttyS0", NULL };
    cards_state_from(&st, NULL, 4, a3);
    ASSERT_STR(st.card[lo].value[4], "/dev/ttyS0");
    /* ACIA réglée autrement (V23, identifiants, autre adresse) : reste une carte. */
    char* a4[] = { "oric1-emu", "--loci", "--serial", "picowifi", "--serial-v23", NULL };
    cards_state_from(&st, NULL, 5, a4);
    ASSERT_TRUE(st.card[ac].on);
    ASSERT_STR(st.card[lo].value[3], LOCI_MODEM_NONE);
    char* a5[] = { "oric1-emu", "--loci", "--serial", "picowifi:box:secret", NULL };
    cards_state_from(&st, NULL, 4, a5);
    ASSERT_TRUE(st.card[ac].on);
    char* a6[] = { "oric1-emu", "--serial", "picowifi", NULL };   /* sans LOCI */
    cards_state_from(&st, NULL, 3, a6);
    ASSERT_TRUE(st.card[ac].on && !st.card[lo].on);
    fake_sysfs_drop();
}

TEST(test_loci_modem_cfg) {
    cards_state_t st, back;
    bool seen[16];
    int lo = cards_find("loci");
    cards_state_defaults(&st);
    cards_set_on(&st, lo, true);
    snprintf(st.card[lo].value[3], CARD_VALUE_MAX, LOCI_MODEM_SIM);
    char path[] = "/tmp/phos_cards_cfg_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    FILE* f = fdopen(fd, "w");
    cards_cfg_write(&st, f);
    fclose(f);
    ASSERT_TRUE(cards_cfg_read(path, &back, seen));
    ASSERT_TRUE(back.card[lo].on);
    ASSERT_STR(back.card[lo].value[3], LOCI_MODEM_SIM);
    /* Configuration : LOCI + modem simulé ajoutés au lancement... */
    char* a1[] = { "oric1-emu", NULL };
    int n = 0;
    char** av = cards_config_argv(path, 1, a1, &n);
    char buf[256];
    ASSERT_TRUE(av != NULL);
    joined(av, buf, sizeof buf);
    ASSERT_TRUE(strstr(buf, "--loci") && strstr(buf, "--serial picowifi"));
    cards_argv_free(av);
    /* ...sauf si la ligne de commande choisit déjà la ligne série. */
    char* a2[] = { "oric1-emu", "--serial", "tcp:bbs:23", NULL };
    av = cards_config_argv(path, 3, a2, &n);
    ASSERT_TRUE(av != NULL);
    joined(av, buf, sizeof buf);
    ASSERT_TRUE(strstr(buf, "--loci") && !strstr(buf, "picowifi"));
    cards_argv_free(av);
    unlink(path);
}

/* Pont LOCI-USB de la Feather (--loci-hw) : ttyACM2, à côté du picowifi. */
#define LOCI_USB_FW_PRODUCT "LOCI-USB (bus 6502 pour Phosphoric)"

TEST(test_loci_usb_detect_by_usb_product) {
    char dev[64];
    fake_sysfs(true);
    ASSERT_TRUE(!loci_usb_detect(NULL, dev, sizeof dev));     /* picowifi seul */
    fake_tty("ttyACM2", LOCI_USB_FW_PRODUCT);
    ASSERT_TRUE(loci_usb_detect(NULL, dev, sizeof dev));
    ASSERT_STR(dev, "/dev/ttyACM2");
    ASSERT_TRUE(picowifi_detect(NULL, dev, sizeof dev));      /* chacun le sien */
    ASSERT_STR(dev, "/dev/ttyACM1");
    fake_sysfs(false);
    fake_tty("ttyACM3", "LOCI-USB loci-fw (bus 6502)");        /* firmware loci-fw-usb */
    ASSERT_TRUE(loci_usb_detect(NULL, dev, sizeof dev));
    ASSERT_STR(dev, "/dev/ttyACM3");
    fake_sysfs_drop();
}

TEST(test_loci_hw_card) {
    cards_state_t st;
    int hw = cards_find("loci_hw"), lo = cards_find("loci");
    char buf[256], msg[96];
    ASSERT_TRUE(hw >= 0 && lo >= 0);                 /* backend « hw » (main) */
    ASSERT_STR(cards_get(hw)->group, "disque");
    fake_sysfs(true);
    fake_tty("ttyACM2", LOCI_USB_FW_PRODUCT);
    /* Ligne de commande : port détecté laissé vide, autre port gardé. */
    char* a1[] = { "oric1-emu", "--loci-hw", "/dev/ttyACM2", NULL };
    cards_state_from(&st, NULL, 3, a1);
    ASSERT_TRUE(st.card[hw].on && !st.card[lo].on);
    ASSERT_STR(st.card[hw].value[0], "");
    char* a2[] = { "oric1-emu", "--loci-hw=/dev/ttyUSB9", NULL };
    cards_state_from(&st, NULL, 2, a2);
    ASSERT_STR(st.card[hw].value[0], "/dev/ttyUSB9");
    /* Groupe « disque » : exclusive avec le LOCI simulé. */
    cards_set_on(&st, lo, true);
    ASSERT_TRUE(!st.card[hw].on);
    cards_set_on(&st, hw, true);
    ASSERT_TRUE(!st.card[lo].on);
    /* Relance : port détecté, puis port imposé. */
    char* a0[] = { "oric1-emu", "--loci", "--loci-flash", "f", NULL };
    st.card[hw].value[0][0] = '\0';
    ASSERT_TRUE(!cards_conflict(&st, msg, sizeof msg));
    char** av = cards_build_argv(&st, 4, a0, NULL);
    joined(av, buf, sizeof buf);
    ASSERT_STR(buf, "oric1-emu --loci-hw /dev/ttyACM2 --no-config-cards");
    cards_argv_free(av);
    snprintf(st.card[hw].value[0], CARD_VALUE_MAX, "/dev/ttyUSB9");
    av = cards_build_argv(&st, 1, a0, NULL);
    joined(av, buf, sizeof buf);
    ASSERT_STR(buf, "oric1-emu --loci-hw /dev/ttyUSB9 --no-config-cards");
    cards_argv_free(av);
    /* Aucun pont branché, port vide : signalé, et la carte n'est pas relancée. */
    fake_sysfs(false);
    st.card[hw].value[0][0] = '\0';
    ASSERT_TRUE(cards_conflict(&st, msg, sizeof msg) && strstr(msg, "LOCI-USB"));
    av = cards_build_argv(&st, 1, a0, NULL);
    joined(av, buf, sizeof buf);
    ASSERT_STR(buf, "oric1-emu --no-config-cards");
    cards_argv_free(av);
    fake_sysfs_drop();
}

TEST(test_loci_hw_cfg) {
    cards_state_t st, back;
    bool seen[16];
    int hw = cards_find("loci_hw");
    cards_state_defaults(&st);
    cards_set_on(&st, hw, true);
    snprintf(st.card[hw].value[0], CARD_VALUE_MAX, "/dev/ttyACM5");
    char path[] = "/tmp/phos_cards_hw_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    FILE* f = fdopen(fd, "w");
    cards_cfg_write(&st, f);
    fclose(f);
    ASSERT_TRUE(cards_cfg_read(path, &back, seen));
    unlink(path);
    ASSERT_TRUE(back.card[hw].on && seen[hw]);
    ASSERT_STR(back.card[hw].value[0], "/dev/ttyACM5");
}

int main(void) {
    /* test-cards est lié au backend LOCI « stub » : la fiche « LOCI réelle »
     * n'apparaît qu'avec le backend « hw » (make LOCI_HW=1). */
    setenv("PHOSPHORIC_TEST_LOCI_BACKEND", "hw", 1);
    printf("=== Registre des cartes d'extension ===\n");
    RUN(test_registry_is_described);
    RUN(test_state_from_cli);
    RUN(test_disk_group_is_exclusive);
    RUN(test_io_conflict_detected);
    RUN(test_build_argv_replaces_cards);
    RUN(test_build_argv_params);
    RUN(test_cfg_roundtrip);
    RUN(test_picowifi_detect_by_usb_product);
    RUN(test_choice_param_cycles);
    RUN(test_loci_modem_build_argv);
    RUN(test_loci_modem_real_missing_is_reported);
    RUN(test_loci_modem_state_from_cli);
    RUN(test_loci_modem_cfg);
    RUN(test_loci_usb_detect_by_usb_product);
    RUN(test_loci_hw_card);
    RUN(test_loci_hw_cfg);
    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
