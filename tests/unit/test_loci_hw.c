/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * test_loci_hw.c — backend --loci-hw (src/io/loci_hw.c) face à une cartouche
 * MAQUETTE sur un pseudo-terminal : course Φ2 mesurée (caps TIMING, commandes
 * TIMING/RDT/WRT du protocole loci-usb) et boîte aux lettres BAL de loci-fw
 * (écritures en page $FF captées, attente de fin de commande, cache ROM).
 *
 * Les durées renvoyées par la maquette sont celles mesurées sur la Feather 5723
 * (sys 120 MHz, Φ2cfg 4000 kHz → tick PIO 8,33 ns ; serve 23 cycles, act 69-118 en
 * lecture, 429 pour une écriture RAMX) ; les cas en retard sont fabriqués pour
 * franchir l'échéance (1000 - 100 = 900 ns). Chronologie (loci_hw.c) :
 *   prêt   = (22 + tior) × 8,33 + 16,7 + serve × 8,33
 *   donnée = max(prêt, 666,7 + 16,7) + 3 × 8,33
 */
#define _DEFAULT_SOURCE
#include "io/loci_emu.h"
#include "io/loci_hw_probe.h"
#include "loci_usb_proto.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* Compteurs internes du backend (src/io/loci_hw.c), hors interface loci_emu.h. */
void loci_hw_timing_stats(unsigned long *timed, unsigned long *late, unsigned long *stale);

static int g_fails, g_checks;
#define CHECK(c, msg) do { g_checks++; if (c) printf("  [OK]   %s\n", msg); \
                           else { g_fails++; printf("  [FAIL] %s\n", msg); } } while (0)

/* ── maquette ── */
static int      m_fd;
static uint8_t  m_tior;
static uint16_t m_serve[65536], m_act[65536];
/* Image ROM servie et BAL : $FF00-$FFCF captée ; une commande non nulle en $FF00 se
 * termine après 3 lectures de $FF00 ($FF00 ← 0, $FF02 ← résultat, gen8 + 1). */
static uint8_t  m_rom[16384], m_gen;
static int      m_bal_pending, m_rdn_count;
static uint8_t  m_id = 'L';   /* $0319 lu par RD (reconnaissance, loci_hw_probe) */

static void mwrite(const uint8_t *p, unsigned n)
{
    while (n) { ssize_t w = write(m_fd, p, n); if (w <= 0) return; p += w; n -= (unsigned)w; }
}
static int mread(void) { uint8_t b; return read(m_fd, &b, 1) == 1 ? b : -1; }

static void *mock_thread(void *arg)
{
    (void)arg;
    int c;
    while ((c = mread()) >= 0) {
        if (c != LUP_REQ_MAGIC) continue;
        int cmd = mread();
        if (cmd < 0) break;
        uint8_t hdr[3] = { LUP_RSP_MAGIC, (uint8_t)cmd, LUP_ST_OK }, a[3];
        switch (cmd) {
        case LUP_CMD_PING: {
            uint8_t p[3] = { LUP_VERSION, 1, LUP_CAP_FIRMWARE | LUP_CAP_TIMING | LUP_CAP_VIRTUAL };
            mwrite(hdr, 3); mwrite(p, 3); break; }
        case LUP_CMD_LINES: { uint8_t p[4] = { LUP_L_NROMDIS, 0, 0, m_gen }; mwrite(hdr, 3); mwrite(p, 4); break; }
        case LUP_CMD_RD: {
            a[0] = (uint8_t)mread(); a[1] = (uint8_t)mread();
            uint16_t ad = (uint16_t)(a[0] | a[1] << 8);
            if (ad == 0xFF00 && m_bal_pending && --m_bal_pending == 0) {
                m_rom[0x3F00] = 0; m_rom[0x3F02] = 0x07; m_gen++;
            }
            uint8_t p[3] = { ad >= 0xC000 ? m_rom[ad - 0xC000] : ad == 0x0319 ? m_id : 0xFF,
                             LUP_F_NROMDIS | LUP_F_SERVED, m_gen };
            mwrite(hdr, 3); mwrite(p, 3); break; }
        case LUP_CMD_RDN: {
            uint8_t b[4]; for (int i = 0; i < 4; i++) b[i] = (uint8_t)mread();
            uint16_t ad = (uint16_t)(b[0] | b[1] << 8); unsigned n = (unsigned)(b[2] | b[3] << 8);
            m_rdn_count++;
            mwrite(hdr, 3);
            for (unsigned i = 0; i < n; i++) {
                uint8_t p[2] = { m_rom[(uint16_t)(ad + i) - 0xC000], LUP_F_NROMDIS | LUP_F_SERVED };
                mwrite(p, 2);
            }
            mwrite(&m_gen, 1); break; }
        case LUP_CMD_WR: {
            a[0] = (uint8_t)mread(); a[1] = (uint8_t)mread(); a[2] = (uint8_t)mread();
            uint16_t ad = (uint16_t)(a[0] | a[1] << 8);
            uint8_t fl = LUP_F_NROMDIS;
            if (ad >= 0xFF00 && ad <= 0xFFCF) {
                m_rom[ad - 0xC000] = a[2]; fl |= LUP_F_SERVED;
                if (ad == 0xFF00 && a[2]) m_bal_pending = 3;
            }
            uint8_t p[2] = { fl, m_gen }; mwrite(hdr, 3); mwrite(p, 2); break; }
        case LUP_CMD_TIMING: {   /* sys 120000 kHz, Φ2cfg 4000 kHz (défaut du firmware), tior */
            uint8_t p[12] = { 0xC0, 0xD4, 0x01, 0, 0xA0, 0x0F, 0, 0, m_tior, 0, 0, 0 };
            mwrite(hdr, 3); mwrite(p, 12); break; }
        case LUP_CMD_RDT: {
            a[0] = (uint8_t)mread(); a[1] = (uint8_t)mread();
            uint16_t ad = (uint16_t)(a[0] | a[1] << 8);
            uint8_t p[7] = { (uint8_t)(ad & 0xFF), LUP_F_SERVED, 0,
                             (uint8_t)m_serve[ad], (uint8_t)(m_serve[ad] >> 8),
                             (uint8_t)m_act[ad], (uint8_t)(m_act[ad] >> 8) };
            mwrite(hdr, 3); mwrite(p, 7); break; }
        case LUP_CMD_WRT: {
            a[0] = (uint8_t)mread(); a[1] = (uint8_t)mread(); a[2] = (uint8_t)mread();
            uint16_t ad = (uint16_t)(a[0] | a[1] << 8);
            uint8_t p[4] = { 0, 0, (uint8_t)m_act[ad], (uint8_t)(m_act[ad] >> 8) };
            mwrite(hdr, 3); mwrite(p, 4); break; }
        default: hdr[2] = LUP_ST_BADCMD; mwrite(hdr, 3);
        }
    }
    return NULL;
}

static void start(const char *faithful)
{
    int master, slave;
    char name[128];
    if (openpty(&master, &slave, name, NULL, NULL) != 0) { perror("openpty"); exit(1); }
    struct termios t; tcgetattr(master, &t); cfmakeraw(&t); tcsetattr(master, TCSANOW, &t);
    m_fd = master;
    pthread_t th; pthread_create(&th, NULL, mock_thread, NULL); pthread_detach(th);
    if (faithful) setenv("LOCI_HW_FAITHFUL", "1", 1); else unsetenv("LOCI_HW_FAITHFUL");
    setenv("LOCI_HW_IDLE_POLL", "0", 1);
    if (loci_emu_start(name) != 0) { printf("  [FAIL] loci_emu_start\n"); exit(1); }
    close(slave);
}

int main(void)
{
    printf("test_loci_hw_timing — course Φ2 mesurée (--loci-hw, caps TIMING)\n");
    unsigned long timed, late, stale;

    /* Durées réelles de la Feather : serve 23 → prêt 392 ns, donnée 708 ns (après Φ2 haut). */
    m_serve[0x0381] = 23;  m_act[0x0381] = 118;
    m_serve[0x0319] = 23;  m_act[0x0319] = 73;
    /* 80 cycles : donnée 892 ns, à temps avec tior 0 ; 917 ns avec tior 3, en retard. */
    m_serve[0x0382] = 80;  m_act[0x0382] = 110;
    /* 120 cycles : donnée 1225 ns, toujours en retard. */
    m_serve[0x0380] = 120; m_act[0x0380] = 130;
    /* Écriture RAMX : act 429 cycles = 4 cycles 6502. */
    m_act[0x03E0] = 429;

    start(NULL);
    loci_emu_api_read(0x0381); loci_emu_idle_poll(10);
    loci_emu_api_read(0x0319); loci_emu_idle_poll(10);
    loci_emu_api_read(0x0382); loci_emu_idle_poll(10);
    loci_hw_timing_stats(&timed, &late, &stale);
    CHECK(timed == 3 && late == 0, "serve 23 et 80 cycles (donnée à 708 et 892 ns, tior 0) : à temps");
    CHECK(!loci_emu_read_lost(), "détection seule : aucune lecture rendue perdue");

    uint8_t v = loci_emu_api_read(0x0380); loci_emu_idle_poll(10);
    loci_hw_timing_stats(&timed, &late, &stale);
    CHECK(late == 1, "serve 120 cycles (donnée à 1225 ns > 900) : lecture en retard comptée");
    CHECK(v == 0x80 && !loci_emu_read_lost(), "sans LOCI_HW_FAITHFUL : la donnée est rendue telle quelle");

    /* Iopage périmé : écriture RAMX (act = 4 cycles 6502) puis accès 2 cycles plus tard. */
    loci_emu_api_write(0x03E0, 0); loci_emu_idle_poll(2);
    loci_emu_api_read(0x0319);
    loci_hw_timing_stats(&timed, &late, &stale);
    CHECK(stale == 1, "accès 2 cycles après une action de 4 cycles : iopage périmé détecté");
    loci_emu_idle_poll(10);
    loci_emu_api_write(0x03E0, 0); loci_emu_idle_poll(4);
    loci_emu_api_read(0x0319);
    loci_hw_timing_stats(&timed, &late, &stale);
    CHECK(stale == 1, "accès 4 cycles après une action de 4 cycles : à temps");
    loci_emu_stop();

    /* Mode fidèle, tior 3 : serve 80 → donnée à 917 ns > 900 → open-bus. */
    m_tior = 3;
    start("1");
    loci_emu_api_read(0x0382);
    CHECK(loci_emu_read_lost(), "fidèle, tior 3 + serve 80 cycles : lecture perdue (open-bus)");
    CHECK(!loci_emu_read_lost(), "le drapeau de lecture perdue est remis à zéro une fois lu");
    loci_emu_api_read(0x0381);
    CHECK(!loci_emu_read_lost(), "fidèle, serve 23 cycles : lecture propre");
    loci_emu_stop();

    /* ── BAL de loci-fw ── */
    m_tior = 0;
    start(NULL);
    uint8_t r = 0xFF;
    CHECK(loci_emu_rom_read(0xFF00, &r) && r == 0 && m_rdn_count == 1, "ROM servie lue par RDN (cache rempli)");
    CHECK(loci_emu_rom_write(0xFF01, 0x42), "écriture $FF01 captée par la BAL (true : la ROM masque la RAM)");
    CHECK(loci_emu_rom_read(0xFF01, &r) && r == 0x42 && m_rdn_count == 1,
          "cache mis à jour sur place, sans RDN (gen8 inchangé)");
    CHECK(loci_emu_rom_write(0xFF00, 0x05), "écriture du groupe en $FF00 : commande lancée");
    CHECK(m_bal_pending == 0, "attente de fin de commande ($FF00 relu jusqu'à 0)");
    CHECK(loci_emu_rom_read(0xFF00, &r) && r == 0 && loci_emu_rom_read(0xFF02, &r) && r == 0x07
          && m_rdn_count == 2, "gen8 changé par le dispatcher : cache rechargé, résultats visibles");
    CHECK(!loci_emu_rom_write(0xFFE0, 0x11), "hors BAL ($FFE0) : non captée → false");
    CHECK(loci_emu_rom_write(0xFF00, 0x02) && m_bal_pending == 3,
          "groupe 2 (Console, exécuté par le 6502) : captée, pas d'attente");
    loci_emu_stop();

    /* ── Reconnaissance d'une LOCI-USB (loci_hw_probe) ── */
    {
        int master, slave;
        char name[128];
        if (openpty(&master, &slave, name, NULL, NULL) != 0) { perror("openpty"); return 1; }
        struct termios t; tcgetattr(slave, &t); cfmakeraw(&t); tcsetattr(slave, TCSANOW, &t);
        m_fd = master;
        pthread_t th; pthread_create(&th, NULL, mock_thread, NULL); pthread_detach(th);
        loci_probe_info_t info;
        CHECK(loci_hw_probe_fd(slave, &info) == LOCI_PROBE_OK && info.id == 'L' &&
              info.proto == LUP_VERSION && info.fw == 1, "PING + 'L' en $0319 : LOCI reconnue");
        m_id = 0xFF;
        CHECK(loci_hw_probe_fd(slave, &info) == LOCI_PROBE_NOT_LOCI && info.id == 0xFF,
              "pont qui répond sans 'L' : pas une LOCI");
        CHECK(loci_hw_port_user(name) == 0, "port tenu par ce seul processus : libre");
        pid_t child = fork();
        if (child == 0) { int fd = open(name, O_RDWR | O_NOCTTY); (void)fd; pause(); _exit(0); }
        int user = 0;
        for (int i = 0; i < 100 && !user; i++) { usleep(10000); user = loci_hw_port_user(name); }
        CHECK(user == (int)child, "port ouvert par un autre processus : son PID est rendu");
        CHECK(loci_hw_probe(name, &info) == LOCI_PROBE_BUSY && info.busy_pid == (int)child,
              "port occupé : pas de PING, LOCI_PROBE_BUSY");
        kill(child, SIGKILL); waitpid(child, NULL, 0);
        close(slave);
    }

    printf("%d/%d vérifications OK\n", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;
}
