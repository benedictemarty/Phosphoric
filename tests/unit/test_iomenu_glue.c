/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_iomenu_glue.c
 * @brief Tests de la liaison menu F1 ↔ émulateur : état affiché, actions
 *        (médias, protection, clavier, joystick, imprimante) et phosphoric.cfg.
 * @author bmarty <bmarty@mailo.com>
 *
 * Machine minimale (Microdisc seul, pas de ROM) : les fonctions testées ne
 * touchent qu'aux champs qu'elles gèrent. Le test tourne dans un dossier
 * temporaire (fichiers d'impression, .dsk, .cfg).
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE   /* mkdtemp sous macOS malgré _POSIX_C_SOURCE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "iomenu_glue.h"
#include "storage/sedoric.h"
#include "io/loci_internal.h"

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
#define ASSERT_STR(a, b) do { if (strcmp((a), (b)) != 0) { \
    printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
    tests_failed++; return; } } while (0)

static emulator_t* emu;

/* Libère la machine de test et tout ce que le menu lui a alloué : disquettes,
 * chemins dupliqués (ici toujours par media_*), cassette. */
static void machine_free(void) {
    if (!emu) return;
    for (int i = 0; i < MICRODISC_MAX_DRIVES; i++) {
        if (emu->disks[i]) sedoric_destroy(emu->disks[i]);
        emu_set_disk_path(emu, i, NULL);
    }
    free(emu->tapebuf);
    emu_set_tape_path(emu, NULL);
    free(emu);
    emu = NULL;
}

static void machine_new(void) {
    machine_free();
    emu = calloc(1, sizeof(*emu));
    microdisc_init(&emu->microdisc);
    emu->has_microdisc = true;
    emu->model = ORIC_MODEL_ATMOS;
    oric_printer_init(&emu->printer);
    oric_joystick_init(&emu->joystick);
    iom_init(&emu->iomenu);
}

/* Deux images .dsk vierges dans le dossier de test. */
static void make_disks(void) {
    sedoric_disk_t* d = sedoric_create_blank(40, 1);
    if (d) { sedoric_save(d, "un.dsk"); sedoric_save(d, "deux.dsk"); sedoric_destroy(d); }
    FILE* f = fopen("hello.tap", "wb");
    if (f) { static const uint8_t tap[16] = { 0x16, 0x16, 0x16, 0x24 }; fwrite(tap, 1, sizeof(tap), f); fclose(f); }
}

TEST(test_refresh_reflects_machine) {
    machine_new();
    ASSERT_EQ(media_disk_insert(emu, 0, "un.dsk"), MEDIA_OK);
    ASSERT_EQ(media_disk_insert(emu, 2, "deux.dsk"), MEDIA_OK);
    ASSERT_EQ(media_tape_insert(emu, "hello.tap"), MEDIA_OK);
    emu->tapeoffs = 4;                                   /* 4 / 16 octets lus */
    iomenu_refresh(emu);
    const iom_state_t* st = &emu->iomenu.st;
    ASSERT_STR(st->machine, "Oric Atmos");
    ASSERT_STR(st->disk_iface, "Microdisc");
    ASSERT_EQ(st->drives, 4);
    ASSERT_STR(st->drive[0], "un.dsk");
    ASSERT_STR(st->drive[1], "");
    ASSERT_STR(st->drive[2], "deux.dsk");
    ASSERT_STR(st->tape, "hello.tap");
    ASSERT_EQ(st->tape_percent, 25);
    ASSERT_TRUE(st->card[0].present);                   /* Microdisc */
    ASSERT_TRUE(!st->card[1].present);                  /* Jasmin */
}

TEST(test_no_disk_interface) {
    machine_new();
    emu->has_microdisc = false;
    ASSERT_EQ(media_disk_insert(emu, 0, "un.dsk"), MEDIA_NO_IFACE);
    iomenu_refresh(emu);
    ASSERT_EQ(emu->iomenu.st.drives, 0);
    ASSERT_TRUE(emu->iomenu.st.disk_iface == NULL);
}

TEST(test_eject_and_errors) {
    machine_new();
    ASSERT_EQ(media_disk_eject(emu, 1), MEDIA_EMPTY);
    ASSERT_EQ(media_disk_insert(emu, 1, "absent.dsk"), MEDIA_LOAD_FAILED);
    ASSERT_EQ(media_disk_insert(emu, 1, "un.dsk"), MEDIA_OK);
    ASSERT_TRUE(emu->disks[1] != NULL);
    ASSERT_EQ(media_disk_eject(emu, 1), MEDIA_OK);
    ASSERT_TRUE(emu->disks[1] == NULL);
    ASSERT_EQ(media_tape_eject(emu), MEDIA_EMPTY);
}

/* Chemins copiés à l'insertion : possédés, libérés au remplacement et à
 * l'éjection (fuite avant 2.9.1, vue par LeakSanitizer) ; un chemin venu de la
 * ligne de commande n'est jamais libéré. */
TEST(test_disk_path_ownership) {
    machine_new();
    emu->disk_paths[1] = "argv.dsk";                 /* comme au lancement */
    ASSERT_EQ(media_disk_insert(emu, 1, "un.dsk"), MEDIA_OK);
    ASSERT_TRUE(emu->disk_path_owned[1]);
    ASSERT_STR(emu->disk_paths[1], "un.dsk");
    ASSERT_EQ(media_disk_insert(emu, 1, "deux.dsk"), MEDIA_OK);
    ASSERT_STR(emu->disk_paths[1], "deux.dsk");
    ASSERT_EQ(media_disk_eject(emu, 1), MEDIA_OK);
    ASSERT_TRUE(emu->disk_paths[1] == NULL);
    ASSERT_TRUE(!emu->disk_path_owned[1]);
    ASSERT_EQ(media_tape_insert(emu, "hello.tap"), MEDIA_OK);
    ASSERT_TRUE(emu->tape_path_owned);
    ASSERT_EQ(media_tape_eject(emu), MEDIA_OK);
    ASSERT_TRUE(emu->tape_path == NULL && !emu->tape_path_owned);
}

/* Une disquette va à la carte présente : avec LOCI (modèle interne) et sans
 * Microdisc/Jasmin, le menu F1 la monte dans le lecteur LOCI. Avec un LOCI
 * co-simulé ou réel, c'est son firmware qui monte ses images : refus. */
TEST(test_disk_routed_to_loci) {
    machine_new();
    emu->has_microdisc = false;
    ASSERT_TRUE(loci_init(&emu->loci));
    emu->has_loci = true;
    iom_action_t a = { IOM_ACT_DISK_INSERT, 1, "un.dsk" };
    iomenu_apply(emu, &a);
    ASSERT_STR(emu->loci.dsk_host_path[1], "un.dsk");
    ASSERT_TRUE(emu->loci.dsk_image[1] != NULL);
    ASSERT_TRUE(emu->disks[1] == NULL);                 /* rien côté Microdisc */
    iomenu_refresh(emu);
    ASSERT_STR(emu->iomenu.st.disk_iface, "LOCI");
    ASSERT_EQ(emu->iomenu.st.drives, 4);
    ASSERT_STR(emu->iomenu.st.drive[1], "un.dsk");
    ASSERT_TRUE(emu->iomenu.st.no_drive_protect);         /* pas de colonne « écriture » */
    ASSERT_EQ(media_disk_eject(emu, 1), MEDIA_OK);
    ASSERT_TRUE(emu->loci.dsk_host_path[1][0] == '\0');
    ASSERT_EQ(media_disk_eject(emu, 1), MEDIA_EMPTY);
    emu->loci_external = true;                          /* --loci-emu / --loci-hw */
    ASSERT_EQ(media_disk_insert(emu, 0, "un.dsk"), MEDIA_NO_IFACE);
    iomenu_refresh(emu);
    ASSERT_EQ(emu->iomenu.st.drives, 0);
    loci_cleanup(&emu->loci);
}

TEST(test_apply_protect_follows_selected_drive) {
    machine_new();
    media_disk_insert(emu, 0, "un.dsk");
    iom_action_t a = { IOM_ACT_DISK_PROTECT, 0, "" };
    iomenu_apply(emu, &a);
    ASSERT_TRUE(emu->microdisc.write_protect[0]);
    ASSERT_TRUE(emu->microdisc.fdc.write_protected);    /* lecteur A sélectionné */
    a.target = 1;
    iomenu_apply(emu, &a);                               /* B protégé, A toujours sous la tête */
    ASSERT_TRUE(emu->microdisc.write_protect[1]);
    iomenu_refresh(emu);
    ASSERT_TRUE(emu->iomenu.st.drive_ro[0]);
    a.target = 0;
    iomenu_apply(emu, &a);                               /* A libéré */
    ASSERT_TRUE(!emu->microdisc.fdc.write_protected);
    ASSERT_TRUE(!emu->iomenu.message_error);
}

TEST(test_apply_toggles) {
    machine_new();
    iom_action_t a = { IOM_ACT_KEYBOARD_TOGGLE, 0, "" };
    iomenu_apply(emu, &a);
    ASSERT_EQ(emu->keyboard.layout, ORIC_KB_AZERTY);
    iomenu_apply(emu, &a);
    ASSERT_EQ(emu->keyboard.layout, ORIC_KB_QWERTY);
    a.type = IOM_ACT_JOYSTICK_CYCLE;
    iomenu_apply(emu, &a);
    ASSERT_EQ(emu->joystick.mode, ORIC_JOY_KEYBOARD);
    iomenu_apply(emu, &a);
    ASSERT_EQ(emu->joystick.mode, ORIC_JOY_SDL_GAMEPAD);
    iomenu_apply(emu, &a);
    ASSERT_EQ(emu->joystick.mode, ORIC_JOY_DISABLED);
    a.type = IOM_ACT_TAPE_FAST_TOGGLE;
    iomenu_apply(emu, &a);
    ASSERT_TRUE(emu->fast_load);
    a.type = IOM_ACT_RESUME;
    ASSERT_TRUE(iomenu_apply(emu, &a));                  /* ferme le menu */
}

TEST(test_apply_printer_cycle) {
    machine_new();
    iom_action_t a = { IOM_ACT_PRINTER_CYCLE, 0, "" };
    iomenu_apply(emu, &a);
    ASSERT_TRUE(oric_printer_is_active(&emu->printer));
    ASSERT_EQ(emu->printer.type, PRINTER_TEXT);
    ASSERT_TRUE(access("impression.txt", F_OK) == 0);
    iomenu_apply(emu, &a);
    ASSERT_EQ(emu->printer.type, PRINTER_MCP40);
    iomenu_apply(emu, &a);
    ASSERT_TRUE(!oric_printer_is_active(&emu->printer));
    iomenu_refresh(emu);
    ASSERT_EQ(emu->iomenu.st.printer, IOM_PRINTER_OFF);
}

TEST(test_config_roundtrip_and_precedence) {
    machine_new();
    emu->diskrom_path = "roms/microdis.rom";
    media_disk_insert(emu, 0, "un.dsk");
    media_disk_insert(emu, 3, "deux.dsk");
    emu_disk_set_protected(emu, 3, true);
    media_tape_insert(emu, "hello.tap");
    oric_keyboard_set_layout(&emu->keyboard, ORIC_KB_AZERTY);
    oric_joystick_set_mode(&emu->joystick, ORIC_JOY_KEYBOARD);
    /* Fichier existant : commentaire et clé inconnue à conserver, clé gérée à remplacer. */
    FILE* f = fopen("t.cfg", "w");
    ASSERT_TRUE(f != NULL);
    fputs("# mes réglages\nmodem=oui\nclavier=qwerty\n", f);
    fclose(f);
    ASSERT_TRUE(iomenu_config_save(emu, "t.cfg"));

    char buf[2048] = "";
    f = fopen("t.cfg", "r");
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "# mes réglages\n") != NULL);
    ASSERT_TRUE(strstr(buf, "modem=oui\n") != NULL);
    ASSERT_TRUE(strstr(buf, "clavier=qwerty") == NULL);   /* remplacée */
    ASSERT_TRUE(strstr(buf, "clavier=azerty\n") != NULL);
    ASSERT_TRUE(strstr(buf, "a=un.dsk\n") != NULL);
    ASSERT_TRUE(strstr(buf, "d=deux.dsk\n") != NULL);
    ASSERT_TRUE(strstr(buf, "protection_d=oui\n") != NULL);
    /* Cartes : clés carte.* (le Microdisc présent, sa ROM par défaut). */
    ASSERT_TRUE(strstr(buf, "carte.microdisc=oui\n") != NULL);
    ASSERT_TRUE(strstr(buf, "carte.jasmin=non\n") != NULL);
    ASSERT_TRUE(strstr(buf, "rom_disque=") == NULL);
    {   /* Relues au lancement en options de carte (ligne de commande vide). */
        char* argv0[] = { "oric1-emu", NULL };
        int xc = 0;
        char** xa = cards_config_argv("t.cfg", 1, argv0, &xc);
        ASSERT_TRUE(xa != NULL && xc == 3);
        ASSERT_STR(xa[1], "--disk-rom");
        ASSERT_STR(xa[2], "roms/microdis.rom");
        cards_argv_free(xa);
    }

    /* Relecture : complète une ligne de commande vide… */
    cli_opts_t cfg;
    cli_opts_init(&cfg);
    int applied = iomenu_config_load("t.cfg", &cfg);
    ASSERT_TRUE(applied >= 6);
    ASSERT_STR(cfg.disk_files[0], "un.dsk");
    ASSERT_STR(cfg.disk_files[3], "deux.dsk");
    ASSERT_TRUE(cfg.disk_protect[3]);
    ASSERT_TRUE(cfg.disk_rom_file == NULL);              /* les cartes passent par cards.c */
    ASSERT_STR(cfg.tape_file, "hello.tap");
    ASSERT_STR(cfg.keyboard_layout, "azerty");
    ASSERT_STR(cfg.joystick_mode, "keys");
    /* Chaînes dupliquées par iomenu_config_load (durée de vie du programme en
     * usage réel) : libérées ici pour LeakSanitizer. */
    free((void*)cfg.disk_files[0]);
    free((void*)cfg.disk_files[3]);
    free((void*)cfg.disk_rom_file);
    free((void*)cfg.tape_file);
    /* …mais la ligne de commande est prioritaire. */
    cli_opts_init(&cfg);
    cfg.disk_files[0] = "cli.dsk";
    cfg.keyboard_layout = "qwerty";
    cfg.jasmin_rom_file = "roms/jasmin.rom";
    iomenu_config_load("t.cfg", &cfg);
    ASSERT_STR(cfg.disk_files[0], "cli.dsk");
    ASSERT_STR(cfg.keyboard_layout, "qwerty");
    ASSERT_TRUE(cfg.disk_rom_file == NULL);             /* interface déjà choisie */
    free((void*)cfg.disk_files[3]);                     /* [0] vient de la « CLI » */
    free((void*)cfg.tape_file);
    /* Enregistrer deux fois ne duplique pas les lignes gérées. */
    ASSERT_TRUE(iomenu_config_save(emu, "t.cfg"));
    f = fopen("t.cfg", "r");
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(strstr(buf, "a=un.dsk") + 1, "\na=un.dsk") == NULL);
    ASSERT_EQ(iomenu_config_load("absent.cfg", &cfg), -1);
}

TEST(test_screenshot_writes_ppm) {
    machine_new();
    ASSERT_TRUE(iomenu_screenshot(emu, "menu.ppm"));
    FILE* f = fopen("menu.ppm", "rb");
    ASSERT_TRUE(f != NULL);
    char hdr[32] = "";
    ASSERT_TRUE(fgets(hdr, sizeof(hdr), f) != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    ASSERT_STR(hdr, "P6\n");
    ASSERT_EQ(size, (long)(IOM_WIDTH * IOM_HEIGHT * 3 + strlen("P6\n640 640\n255\n")));
    ASSERT_TRUE(!emu->iomenu.open);                      /* le menu n'est pas resté ouvert */
}

int main(void) {
    char dir[] = "/tmp/iomenu_glue_XXXXXX";
    if (!mkdtemp(dir) || chdir(dir) != 0) { perror("mkdtemp"); return 1; }
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Menu F1 : liaison avec l'émulateur, phosphoric.cfg\n");
    printf("═══════════════════════════════════════════════════════\n");
    make_disks();
    RUN(test_refresh_reflects_machine);
    RUN(test_no_disk_interface);
    RUN(test_eject_and_errors);
    RUN(test_disk_path_ownership);
    RUN(test_disk_routed_to_loci);
    RUN(test_apply_protect_follows_selected_drive);
    RUN(test_apply_toggles);
    RUN(test_apply_printer_cycle);
    RUN(test_config_roundtrip_and_precedence);
    RUN(test_screenshot_writes_ppm);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (chdir("/") != 0 || system(cmd) != 0) { /* nettoyage au mieux */ }
    printf("\n  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    machine_free();
    return tests_failed ? 1 : 0;
}
