/**
 * @file test_loci_acia_miss.c
 * @brief Reproduction fidèle de la course PHI2 du LOCI sur l'ACIA $0380 (picowifi).
 * @author bmarty <bmarty@mailo.com>
 *
 * Vérifie le modèle « course de serve perdue » de io_bus.c (acia_dev_read/write/peek) :
 *
 *  - VIA inhibé symétriquement → un raté renvoie l'OPEN-BUS (dernier octet du data
 *    bus), pas le VIA ni 0xFF (cf. extensions/analyse/read-serve-et-inhibition-via.md).
 *  - Lecture DATA ratée = DESTRUCTIVE côté LOCI : l'octet RX est consommé « en aveugle »
 *    et perdu → « modem injoignable » (le pilier du bug).
 *  - Lecture STAT/CMD/CTRL ratée = IDEMPOTENTE : open-bus ce tour-ci, mais registre
 *    relisible au suivant (raté pardonné, comme le polling disque/MIA).
 *  - ÉCRITURE toujours fiable : atteint l'ACIA même course perdue.
 *  - peek() (observateur) : open-bus SANS consommer.
 *
 * Le tout est DÉTERMINISTE : la fenêtre `tior` fiable pilote le raté (pas d'aléa).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emulator.h"
#include "io/io_bus.h"
#include "io/acia6551.h"
#include "io/serial_backend.h"
#include "io/loci.h"
#include "io/bus_timing.h"
#include "memory/memory.h"

/* ── Micro-framework (identique aux autres suites) ─────────────────────────── */
static int tests_passed = 0;
static int tests_failed = 0;
#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-45s", #name); name(); \
    printf("\n"); } while (0)
#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { printf("[FAIL] %s:%d %s", __FILE__, __LINE__, #cond); \
        tests_failed++; return; } } while (0)
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) do { \
    long _va = (long)(a), _vb = (long)(b); \
    if (_va != _vb) { printf("[FAIL] %s:%d %s==%s (%ld != %ld)", \
        __FILE__, __LINE__, #a, #b, _va, _vb); tests_failed++; return; } } while (0)
#define PASS() do { tests_passed++; printf("[OK]"); } while (0)

/* ── Harnais : émulateur minimal LOCI + ACIA à $0380 ───────────────────────── */
static emulator_t*      g_emu = NULL;
static serial_backend_t* g_loop = NULL;

static void setup(void) {
    g_emu = (emulator_t*)calloc(1, sizeof(emulator_t));
    memory_init(&g_emu->memory);
    acia_init(&g_emu->acia);
    g_loop = serial_backend_loopback_create();
    g_loop->open(g_loop);
    acia_set_backend(&g_emu->acia, g_loop);
    /* 19200 8-N-1, DTR on : autorise la réception loopback. */
    acia_write(&g_emu->acia, ACIA_REG_CONTROL, 0x1F);
    acia_write(&g_emu->acia, ACIA_REG_COMMAND, 0x01);

    g_emu->has_serial = true;
    g_emu->acia_base_addr = 0x0380;
    g_emu->has_loci = true;
    /* Fenêtre fiable [5,10] : tior=0 (défaut) → HORS fenêtre → course perdue. */
    loci_set_mia_window(&g_emu->loci, 5, 10);
    g_emu->loci.mia_tior = 0;
}

static void teardown(void) {
    if (g_loop) { serial_backend_destroy(g_loop); g_loop = NULL; }
    if (g_emu) { memory_cleanup(&g_emu->memory); free(g_emu); g_emu = NULL; }
}

static void set_reliable(bool reliable) {
    g_emu->loci.mia_tior = reliable ? 7 : 0;   /* 7 ∈ [5,10], 0 ∉ */
}

/* Injecte un octet dans le flux RX (via le backend loopback) et le fait remonter
 * dans l'ACIA (RDRF posé). */
static void inject_rx(uint8_t byte) {
    g_loop->send(g_loop, byte);
    for (int i = 0; i < 800; i++) acia_tick(&g_emu->acia, 4);
}

/* Lecture/écriture via le bus I/O réel (dispatch io_bus), comme le 6502. */
static uint8_t bus_read(uint16_t addr) {
    const io_device_t* d = io_bus_find(g_emu, addr);
    return (d && d->read) ? d->read(g_emu, addr) : 0;
}
static uint8_t bus_peek(uint16_t addr) {
    const io_device_t* d = io_bus_find(g_emu, addr);
    return (d && d->peek) ? d->peek(g_emu, addr) : (d ? d->read(g_emu, addr) : 0);
}
static bool bus_write(uint16_t addr, uint8_t v) {
    const io_device_t* d = io_bus_find_write(g_emu, addr);
    return d && d->write ? d->write(g_emu, addr, v) : false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

/* $0380 est bien revendiqué par l'ACIA (pas le LOCI : hors fenêtre MIA/TAP/DSK). */
TEST(test_acia_claims_0380) {
    setup();
    const io_device_t* d = io_bus_find(g_emu, 0x0380);
    ASSERT_TRUE(d != NULL);
    ASSERT_TRUE(strcmp(d->name, "acia") == 0);
    teardown();
    PASS();
}

/* Course perdue : lecture DATA renvoie l'open-bus (dernier octet du bus), PAS le
 * VIA ni 0xFF, et l'octet RX est CONSOMMÉ (perdu) → RDRF retombe. */
TEST(test_data_miss_is_open_bus_and_destructive) {
    setup();
    set_reliable(false);
    inject_rx(0xAA);
    ASSERT_TRUE(acia_peek(&g_emu->acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);

    g_emu->memory.last_bus_value = 0x3C;         /* résidu de bus (open-bus attendu) */
    uint8_t got = bus_read(0x0380);
    ASSERT_EQ(got, 0x3C);                          /* open-bus, ni 0xAA ni 0xFF */

    /* L'octet a été consommé en aveugle → perdu. En redevenant fiable, plus de RDRF. */
    set_reliable(true);
    ASSERT_FALSE(acia_read(&g_emu->acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);
    teardown();
    PASS();
}

/* Course perdue : lecture STATUS renvoie l'open-bus mais NE consomme PAS (idempotent) →
 * l'octet RX survit et reste lisible une fois la fiabilité rétablie. */
TEST(test_status_miss_is_idempotent) {
    setup();
    set_reliable(false);
    inject_rx(0xBB);

    g_emu->memory.last_bus_value = 0x11;
    uint8_t st = bus_read(0x0381);                 /* STATUS raté */
    ASSERT_EQ(st, 0x11);                            /* open-bus */

    /* RX pas volé : redevenu fiable, RDRF encore là, DATA relit 0xBB. */
    set_reliable(true);
    ASSERT_TRUE(acia_read(&g_emu->acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);
    ASSERT_EQ(acia_read(&g_emu->acia, ACIA_REG_DATA), 0xBB);
    teardown();
    PASS();
}

/* L'écriture est toujours fiable : elle atteint l'ACIA même course perdue. */
TEST(test_write_always_reaches_acia) {
    setup();
    set_reliable(false);
    ASSERT_TRUE(bus_write(0x0382, 0x0B));          /* COMMAND = DTR|TIC (course perdue) */

    set_reliable(true);
    ASSERT_EQ(acia_read(&g_emu->acia, ACIA_REG_COMMAND), 0x0B);
    teardown();
    PASS();
}

/* peek() (observateur : débogueur/moniteur) renvoie l'open-bus SANS consommer le RX. */
TEST(test_peek_miss_open_bus_non_destructive) {
    setup();
    set_reliable(false);
    inject_rx(0xCC);

    g_emu->memory.last_bus_value = 0x22;
    ASSERT_EQ(bus_peek(0x0380), 0x22);             /* open-bus, non destructif */

    set_reliable(true);
    ASSERT_TRUE(acia_read(&g_emu->acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);
    ASSERT_EQ(acia_read(&g_emu->acia, ACIA_REG_DATA), 0xCC);  /* octet préservé */
    teardown();
    PASS();
}

/* Fenêtre fiable : comportement 6551 strictement inchangé (aucune régression). */
TEST(test_reliable_read_is_pristine) {
    setup();
    set_reliable(true);
    inject_rx(0xDD);
    g_emu->memory.last_bus_value = 0x99;           /* ne doit PAS fuiter en fiable */
    ASSERT_TRUE(bus_read(0x0381) & ACIA_STATUS_RDRF);
    ASSERT_EQ(bus_read(0x0380), 0xDD);             /* vraie donnée, pas l'open-bus */
    teardown();
    PASS();
}

/* Sans LOCI (ACIA autonome), aucune course : lecture toujours propre même tior=0. */
TEST(test_no_loci_no_race) {
    setup();
    g_emu->has_loci = false;                        /* pas de MIA → pas de serve fragile */
    g_emu->loci.mia_tior = 0;
    inject_rx(0xEE);
    g_emu->memory.last_bus_value = 0x55;
    ASSERT_EQ(bus_read(0x0380), 0xEE);             /* donnée réelle, open-bus ignoré */
    teardown();
    PASS();
}

/* ── Épic B / Phase 1 : modèle de course PHI2 sous-cycle (physiquement fondé) ── */

/* Le serve arrive au subtick (tior + serve_subticks) ; propre ssi ≤ latch. */
TEST(test_phase_model_serve_race) {
    setup();
    loci_set_serve_timing(&g_emu->loci, 20, 27);   /* serve=20, latch=27 subticks */

    g_emu->loci.mia_tior = 0;                        /* valid=20 ≤ 27 → propre */
    ASSERT_TRUE(loci_mia_io_reliable(&g_emu->loci));
    inject_rx(0x7E);
    ASSERT_EQ(bus_read(0x0380), 0x7E);              /* vraie donnée */

    g_emu->loci.mia_tior = 8;                        /* valid=28 > 27 → course perdue */
    ASSERT_FALSE(loci_mia_io_reliable(&g_emu->loci));
    inject_rx(0x99);
    g_emu->memory.last_bus_value = 0x44;
    ASSERT_EQ(bus_read(0x0380), 0x44);              /* open-bus, octet perdu */
    teardown();
    PASS();
}

/* Reproduit le rapport de bug : même carte, build `-Os` (serve court) marche,
 * build `-O2` (serve long) rate — indépendamment de tout réglage tior. */
TEST(test_phase_reproduces_build_os_vs_o2) {
    setup();
    g_emu->loci.mia_tior = 0;

    loci_set_serve_timing(&g_emu->loci, 26, 27);   /* -Os : serve 26 cyc ≤ latch → OK */
    ASSERT_TRUE(loci_mia_io_reliable(&g_emu->loci));

    loci_set_serve_timing(&g_emu->loci, 36, 27);   /* -O2 : serve 36 cyc > latch → KO */
    ASSERT_FALSE(loci_mia_io_reliable(&g_emu->loci));
    teardown();
    PASS();
}

/* Le prédicat de course brut (bus_timing.h) : valid ≤ latch gagne. */
TEST(test_bus_serve_wins_race_predicate) {
    ASSERT_TRUE(bus_serve_wins_race(0, 27));         /* on-board : toujours */
    ASSERT_TRUE(bus_serve_wins_race(27, 27));        /* pile au latch */
    ASSERT_FALSE(bus_serve_wins_race(28, 27));       /* rate d'un subtick */
    PASS();
}

int main(void) {
    printf("\n=== LOCI ACIA $0380 — course PHI2 (picowifi) ===\n");
    RUN(test_acia_claims_0380);
    RUN(test_data_miss_is_open_bus_and_destructive);
    RUN(test_status_miss_is_idempotent);
    RUN(test_write_always_reaches_acia);
    RUN(test_peek_miss_open_bus_non_destructive);
    RUN(test_reliable_read_is_pristine);
    RUN(test_no_loci_no_race);
    RUN(test_phase_model_serve_race);
    RUN(test_phase_reproduces_build_os_vs_o2);
    RUN(test_bus_serve_wins_race_predicate);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
