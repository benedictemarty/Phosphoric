/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * test_loci_hw.c — --loci-hw backend (src/io/loci_hw.c) against a MOCK
 * cartridge on a pseudo-terminal: measured Φ2 race (TIMING caps, TIMING/RDT/WRT
 * commands of the loci-usb protocol) and loci-fw BAL mailbox
 * (captured page $FF writes, wait for command completion, ROM cache).
 *
 * The durations returned by the mock are those measured on Feather 5723
 * (sys 120 MHz, Φ2cfg 4000 kHz → PIO tick 8.33 ns; serve 23 cycles, act 69-118 for
 * a read, 429 for a RAMX write); the late cases are crafted to
 * cross the deadline (1000 - 100 = 900 ns). Timeline (loci_hw.c):
 *   ready = (22 + tior) × 8.33 + 16.7 + serve × 8.33
 *   data  = max(ready, 666.7 + 16.7) + 3 × 8.33
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
#include <time.h>
#include <unistd.h>

/* Internal backend counters (src/io/loci_hw.c), outside the loci_emu.h interface. */
void loci_hw_timing_stats(unsigned long *timed, unsigned long *late, unsigned long *stale);

static int g_fails, g_checks;
#define CHECK(c, msg) do { g_checks++; if (c) printf("  [OK]   %s\n", msg); \
                           else { g_fails++; printf("  [FAIL] %s\n", msg); } } while (0)

/* ── mock ── */
static int      m_fd;
static uint8_t  m_tior;
static uint16_t m_serve[65536], m_act[65536];
/* Served ROM image and BAL: $FF00-$FFCF captured; a non-zero command at $FF00
 * completes after 3 reads of $FF00 ($FF00 ← 0, $FF02 ← result, gen8 + 1). */
static uint8_t  m_rom[16384], m_gen;
static int      m_bal_pending, m_rdn_count;
static int      m_btn_warm_irqs;  /* BTN: nIRQ pulses returned by the next LINES (warm press) */
static int      m_lines_irqs;
/* Log of the writes received by the mock on $03A4/$03A8 (WR/WRT one by one, or
 * grouped WRN), in arrival order. */
static uint8_t  m_xw[70000];
static unsigned m_xw_n, m_xw_single, m_xw_wrn, m_xw_wrn_max;
/* XRAM ports RW0/RW1 modelled (m_xs_model) as in sys/mia.c: a read returns
 * xram[ADDR] then ADDR += STEP; a write stores xram[ADDR] then ADDR += STEP;
 * $03A5/$03A6-7 (STEP0/ADDR0), $03A9/$03AA-B (STEP1/ADDR1). XPEEK without effect,
 * XADV = k real reads. m_caps_xs announces LUP_CAP_XSTREAM. */
static int      m_xs_model, m_caps_xs;
static uint8_t  m_xram[65536];
static uint16_t m_xaddr[2];
static int8_t   m_xstep[2];
static unsigned m_port_reads, m_xpeeks, m_xadvs;
static int  port_ch(uint16_t ad) { return ad == 0x03A4 ? 0 : ad == 0x03A8 ? 1 : -1; }
static uint8_t port_read(int ch) { uint8_t v = m_xram[m_xaddr[ch]]; m_xaddr[ch] = (uint16_t)(m_xaddr[ch] + m_xstep[ch]); return v; }
static void port_write(int ch, uint8_t v) { m_xram[m_xaddr[ch]] = v; m_xaddr[ch] = (uint16_t)(m_xaddr[ch] + m_xstep[ch]); }
static void reg_write(uint16_t ad, uint8_t v)
{
    int ch = port_ch(ad);
    if (ch >= 0) { port_write(ch, v); return; }
    if (ad == 0x03A5) m_xstep[0] = (int8_t)v;
    if (ad == 0x03A6) m_xaddr[0] = (uint16_t)((m_xaddr[0] & 0xFF00) | v);
    if (ad == 0x03A7) m_xaddr[0] = (uint16_t)((m_xaddr[0] & 0x00FF) | v << 8);
    if (ad == 0x03A9) m_xstep[1] = (int8_t)v;
    if (ad == 0x03AA) m_xaddr[1] = (uint16_t)((m_xaddr[1] & 0xFF00) | v);
    if (ad == 0x03AB) m_xaddr[1] = (uint16_t)((m_xaddr[1] & 0x00FF) | v << 8);
}
static int      m_boot_swap;  /* WRT $03AF=$A0 replaces the served ROM and moves gen8 */
static uint8_t  m_id = 'L';   /* $0319 read by RD (recognition, loci_hw_probe) */

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
            uint8_t caps = LUP_CAP_FIRMWARE | LUP_CAP_TIMING | LUP_CAP_VIRTUAL;
#ifdef LUP_CAP_XSTREAM
            if (m_caps_xs) caps |= LUP_CAP_XSTREAM;
#endif
            uint8_t p[3] = { LUP_VERSION, 1, caps };
            mwrite(hdr, 3); mwrite(p, 3); break; }
        case LUP_CMD_LINES: { uint8_t p[4] = { LUP_L_NROMDIS, (uint8_t)m_lines_irqs, 0, m_gen };
            m_lines_irqs = 0; mwrite(hdr, 3); mwrite(p, 4); break; }
        case LUP_CMD_BTN: { (void)mread(); m_lines_irqs = m_btn_warm_irqs; mwrite(hdr, 3); break; }
        case LUP_CMD_RD: {
            a[0] = (uint8_t)mread(); a[1] = (uint8_t)mread();
            uint16_t ad = (uint16_t)(a[0] | a[1] << 8);
            if (ad == 0xFF00 && m_bal_pending && --m_bal_pending == 0) {
                m_rom[0x3F00] = 0; m_rom[0x3F02] = 0x07; m_gen++;
            }
            if (m_xs_model && port_ch(ad) >= 0) {
                uint8_t q[3] = { port_read(port_ch(ad)), LUP_F_NROMDIS | LUP_F_SERVED, m_gen };
                m_port_reads++; mwrite(hdr, 3); mwrite(q, 3); break;
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
            if (m_xs_model && ad >= 0x03A4 && ad <= 0x03AB) reg_write(ad, a[2]);
            if (ad >= 0xFF00 && ad <= 0xFFCF) {
                m_rom[ad - 0xC000] = a[2]; fl |= LUP_F_SERVED;
                if (ad == 0xFF00 && a[2]) m_bal_pending = 3;
            }
            uint8_t p[2] = { fl, m_gen }; mwrite(hdr, 3); mwrite(p, 2); break; }
        case LUP_CMD_WRN: {
            uint8_t b[4]; for (int i = 0; i < 4; i++) b[i] = (uint8_t)mread();
            unsigned n = (unsigned)(b[2] | b[3] << 8);
            uint16_t wad = (uint16_t)(b[0] | b[1] << 8);
            for (unsigned i = 0; i < n; i++) {
                int v = mread();
                if (m_xw_n < sizeof(m_xw)) m_xw[m_xw_n++] = (uint8_t)v;
                if (m_xs_model) reg_write(wad, (uint8_t)v);
            }
            m_xw_wrn++; if (n > m_xw_wrn_max) m_xw_wrn_max = n;
            uint8_t p[2] = { LUP_F_NROMDIS, m_gen }; mwrite(hdr, 3); mwrite(p, 2); break; }
        case LUP_CMD_TIMING: {   /* sys 120000 kHz, Φ2cfg 4000 kHz (firmware default), tior */
            uint8_t p[12] = { 0xC0, 0xD4, 0x01, 0, 0xA0, 0x0F, 0, 0, m_tior, 0, 0, 0 };
            mwrite(hdr, 3); mwrite(p, 12); break; }
        case LUP_CMD_RDT: {
            a[0] = (uint8_t)mread(); a[1] = (uint8_t)mread();
            uint16_t ad = (uint16_t)(a[0] | a[1] << 8);
            uint8_t dv = (uint8_t)(ad & 0xFF);
            if (m_xs_model && port_ch(ad) >= 0) { dv = port_read(port_ch(ad)); m_port_reads++; }
            uint8_t p[7] = { dv, LUP_F_NROMDIS | LUP_F_SERVED, m_gen,
                             (uint8_t)m_serve[ad], (uint8_t)(m_serve[ad] >> 8),
                             (uint8_t)m_act[ad], (uint8_t)(m_act[ad] >> 8) };
            mwrite(hdr, 3); mwrite(p, 7); break; }
        case LUP_CMD_WRT: {
            a[0] = (uint8_t)mread(); a[1] = (uint8_t)mread(); a[2] = (uint8_t)mread();
            uint16_t ad = (uint16_t)(a[0] | a[1] << 8);
            if (ad == 0x03AF && a[2] == 0xA0 && m_boot_swap) {   /* mia_api_boot: BASIC into the bank */
                memset(m_rom, 0xBB, sizeof(m_rom)); m_gen++;
            }
            if ((ad == 0x03A4 || ad == 0x03A8) && m_xw_n < sizeof(m_xw)) { m_xw[m_xw_n++] = a[2]; m_xw_single++; }
            if (m_xs_model && ad >= 0x03A4 && ad <= 0x03AB) reg_write(ad, a[2]);
            uint8_t p[4] = { LUP_F_NROMDIS, m_gen, (uint8_t)m_act[ad], (uint8_t)(m_act[ad] >> 8) };
            mwrite(hdr, 3); mwrite(p, 4); break; }
#ifdef LUP_CAP_XSTREAM
        case LUP_CMD_XPEEK: case LUP_CMD_XADV: {
            uint8_t b[3]; for (int i = 0; i < 3; i++) b[i] = (uint8_t)mread();
            int ch = b[0] & 1;
            unsigned n = (unsigned)(b[1] | b[2] << 8);
            if (cmd == LUP_CMD_XPEEK) {
                m_xpeeks++;
                uint8_t q[3] = { (uint8_t)m_xaddr[ch], (uint8_t)(m_xaddr[ch] >> 8), (uint8_t)m_xstep[ch] };
                mwrite(hdr, 3); mwrite(q, 3);
                for (unsigned i = 0; i < n; i++) { uint8_t v = m_xram[(uint16_t)(m_xaddr[ch] + (int)i * m_xstep[ch])]; mwrite(&v, 1); }
                mwrite(&m_gen, 1);
            } else {
                m_xadvs++;
                for (unsigned i = 0; i < n; i++) (void)port_read(ch);
                uint8_t q[2] = { LUP_F_NROMDIS, m_gen }; mwrite(hdr, 3); mwrite(q, 2);
            }
            break; }
#endif
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

    /* Real Feather durations: serve 23 → ready 392 ns, data 708 ns (after Φ2 high). */
    m_serve[0x0381] = 23;  m_act[0x0381] = 118;
    m_serve[0x0319] = 23;  m_act[0x0319] = 73;
    /* 80 cycles: data 892 ns, in time with tior 0; 917 ns with tior 3, late. */
    m_serve[0x0382] = 80;  m_act[0x0382] = 110;
    /* 120 cycles: data 1225 ns, always late. */
    m_serve[0x0380] = 120; m_act[0x0380] = 130;
    /* RAMX write: act 429 cycles = 4 6502 cycles. */
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

    /* Stale iopage: RAMX write (act = 4 6502 cycles) then access 2 cycles later. */
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

    /* Faithful mode, tior 3: serve 80 → data at 917 ns > 900 → open-bus. */
    m_tior = 3;
    start("1");
    loci_emu_api_read(0x0382);
    CHECK(loci_emu_read_lost(), "fidèle, tior 3 + serve 80 cycles : lecture perdue (open-bus)");
    CHECK(!loci_emu_read_lost(), "le drapeau de lecture perdue est remis à zéro une fois lu");
    loci_emu_api_read(0x0381);
    CHECK(!loci_emu_read_lost(), "fidèle, serve 23 cycles : lecture propre");
    loci_emu_stop();

    /* ── loci-fw BAL ── */
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

    /* ── API call that replaces the served ROM (LOCI menu → ESC → mia_api_boot) ── */
    m_tior = 0;
    memset(m_rom, 0x4C, sizeof(m_rom));           /* « menu ROM » */
    m_boot_swap = 1;
    start(NULL);
    r = 0;
    CHECK(loci_emu_rom_read(0xC123, &r) && r == 0x4C, "ROM du menu en cache");
    loci_emu_api_write(0x03AF, 0xA0);             /* STA MIA_OP: the reply carries the new gen8 */
    CHECK(loci_emu_rom_read(0xC124, &r) && r == 0x4C,
          "après STA $03AF : le JSR $03B0 qui suit est lu dans la ROM d'avant (6502 déjà dans le spin)");
    loci_emu_api_read(0x03B0);                    /* MIA_SPIN fetch: first $03xx access */
    CHECK(loci_emu_rom_read(0xC124, &r) && r == 0xBB,
          "premier accès $03xx après l'appel : cache rechargé, nouvelle ROM visible");
    /* RETURN from the menu (call_loci_boot): STA MIA_OP ; LDA MIA_XSTACK $03AC ; PLP ;
     * JMP MIA_SPIN — reading $03AC does not lift the deferral, only reading $03B0. */
    memset(m_rom, 0x4C, sizeof(m_rom)); m_gen++;
    loci_emu_api_read(0x03B0);
    CHECK(loci_emu_rom_read(0xC200, &r) && r == 0x4C, "ROM du menu rechargée");
    loci_emu_api_write(0x03AF, 0xA0);
    loci_emu_api_read(0x03AC);
    CHECK(loci_emu_rom_read(0xC201, &r) && r == 0x4C,
          "lecture de $03AC après STA $03AF : PLP / JMP encore lus dans la ROM d'avant");
    loci_emu_api_read(0x03B0);
    CHECK(loci_emu_rom_read(0xC201, &r) && r == 0xBB, "lecture de MIA_SPIN $03B0 : nouvelle ROM visible");
    loci_emu_api_write(0x03A0, 0x00);             /* another register: immediate invalidation */
    memset(m_rom, 0x11, sizeof(m_rom)); m_gen++;
    loci_emu_api_read(0x03B1);
    CHECK(loci_emu_rom_read(0xC000, &r) && r == 0x11, "hors appel d'API : nouvelle génération prise aussitôt");
    m_boot_swap = 0;
    loci_emu_stop();

    /* ── Warm MENU button: nIRQ (firmware $03BA trap), no nRESET ── */
    start(NULL);
    m_btn_warm_irqs = 1;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    loci_emu_menu_button();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    CHECK(ms < 500, "appui à chaud (nIRQ sans nRESET) : la main revient au 6502 sans attendre nRESET");
    CHECK(loci_emu_irq_take() == 1, "l'impulsion nIRQ du bouton est livrée au 6502 au prochain drain");
    CHECK(loci_emu_irq_take() == 0, "livrée une seule fois");
    CHECK(loci_emu_reset_take() == 0, "pas de reset du 6502");
    m_btn_warm_irqs = 0;
    loci_emu_stop();

    /* ── Posted writes on $03A4 (warm save: ~48 KB byte by byte) ── */
    start(NULL);
    m_xw_n = m_xw_single = m_xw_wrn = m_xw_wrn_max = 0;
    unsigned long req0 = 0;
    for (int i = 0; i < 1000; i++) {
        loci_emu_api_write(0x03A4, (uint8_t)(i * 7));
        loci_emu_irq_take();                      /* like reflect_nirq after each MIA write */
    }
    (void)req0;
    CHECK(m_xw_single == 0 && m_xw_wrn == 3 && m_xw_wrn_max == 256,
          "1000 écritures $03A4 (+ drain nIRQ après chacune) : aucune requête unitaire, 3 WRN pleins");
    loci_emu_api_read(0x03A0);                    /* a read flushes the buffer before it is done */
    int order_ok = m_xw_n == 1000;
    for (int i = 0; order_ok && i < 1000; i++) order_ok = m_xw[i] == (uint8_t)(i * 7);
    CHECK(order_ok && m_xw_wrn == 4, "lecture $03xx : reste vidé avant elle, 1000 octets dans l'ordre");
    loci_emu_api_write(0x03A4, 0x11);
    loci_emu_api_write(0x03A8, 0x22);             /* other port: flushes RW0 first */
    loci_emu_api_write(0x03A5, 0x01);             /* other register: flushes RW1, then a normal write */
    CHECK(m_xw_n == 1002 && m_xw[1000] == 0x11 && m_xw[1001] == 0x22, "changement d'adresse : ordre conservé");
    loci_emu_api_write(0x03A4, 0x33);
    loci_emu_idle_poll(1000);
    CHECK(m_xw_n == 1002, "1000 cycles sans accès : écriture encore en tampon");
    loci_emu_idle_poll(1500);
    CHECK(m_xw_n == 1003 && m_xw[1002] == 0x33, "2500 cycles sans accès : tampon vidé (latence bornée)");
    loci_emu_stop();
    setenv("LOCI_HW_NO_POST", "1", 1);
    start(NULL);
    m_xw_n = m_xw_single = m_xw_wrn = 0;
    for (int i = 0; i < 10; i++) loci_emu_api_write(0x03A4, (uint8_t)i);
    CHECK(m_xw_single == 10 && m_xw_wrn == 0, "LOCI_HW_NO_POST : une requête par écriture");
    loci_emu_stop();
    unsetenv("LOCI_HW_NO_POST");

#ifdef LUP_CAP_XSTREAM
    /* ── Grouped reads on $03A4 (restore: RETURN in the menu) ── */
    m_xs_model = 1; m_caps_xs = 1;
    for (unsigned i = 0; i < 65536; i++) m_xram[i] = (uint8_t)(i * 7 + 3);
    start(NULL);
    loci_emu_api_write(0x03A5, 1);                /* STEP0 = 1, ADDR0 = $1000 */
    loci_emu_api_write(0x03A6, 0x00);
    loci_emu_api_write(0x03A7, 0x10);
    m_port_reads = m_xpeeks = m_xadvs = 0;
    int seq_ok = 1;
    for (unsigned i = 0; i < 600; i++) {
        uint8_t v = loci_emu_api_read(0x03A4);
        loci_emu_irq_take();                      /* like reflect_nirq after each MIA read */
        if (v != (uint8_t)((0x1000 + i) * 7 + 3)) seq_ok = 0;
    }
    CHECK(seq_ok, "600 lectures $03A4 : octets justes, dans l'ordre");
    CHECK(m_port_reads == 2 && m_xpeeks == 3 && m_xadvs == 2,
          "600 lectures : 2 réelles puis 3 XPEEK de 256, réarmement par XADV + XPEEK sans lecture réelle");
    loci_emu_api_read(0x03A0);                    /* another access: XADV settles the served reads */
    CHECK(m_xaddr[0] == 0x1000 + 600, "XADV avant tout autre accès : ADDR0 = état exact de 600 lectures");
    /* Port write during a stream: the settlement comes first. */
    loci_emu_api_read(0x03A4); loci_emu_api_read(0x03A4); loci_emu_api_read(0x03A4);
    loci_emu_api_write(0x03A4, 0xEE);
    loci_emu_api_read(0x03A0);
    CHECK(m_xaddr[0] == 0x1000 + 604 && m_xram[0x1000 + 603] == 0xEE,
          "écriture $03A4 en plein flux : lectures soldées d'abord, l'octet va au bon ADDR");
    /* STEP = 0 (HID keyboard window): never a buffer. */
    loci_emu_api_write(0x03A5, 0);
    m_port_reads = m_xpeeks = 0;
    for (int i = 0; i < 10; i++) loci_emu_api_read(0x03A4);
    CHECK(m_port_reads == 10 && m_xpeeks == 1,
          "STEP0 = 0 : lectures unitaires (pas de tampon périmé), un seul XPEEK tenté");
    loci_emu_api_write(0x03A5, 1);
    /* Bounded latency: after 2000 cycles without access, the buffer is settled. */
    uint16_t before = m_xaddr[0];
    loci_emu_api_read(0x03A4); loci_emu_api_read(0x03A4); loci_emu_api_read(0x03A4);
    CHECK(m_xaddr[0] != (uint16_t)(before + 3), "3 lectures dont des servies : la cartouche n'a pas encore avancé");
    loci_emu_idle_poll(2500);
    CHECK(m_xaddr[0] == (uint16_t)(before + 3), "2500 cycles sans accès : lectures servies soldées (XADV)");
    loci_emu_stop();
    /* Firmware without XSTREAM (caps 1F): one request per read. */
    m_caps_xs = 0;
    start(NULL);
    loci_emu_api_write(0x03A6, 0x00); loci_emu_api_write(0x03A7, 0x20);
    m_port_reads = m_xpeeks = 0;
    for (int i = 0; i < 20; i++) loci_emu_api_read(0x03A4);
    CHECK(m_port_reads == 20 && m_xpeeks == 0, "firmware sans XSTREAM : une requête par lecture");
    loci_emu_stop();
    m_xs_model = 0;
#endif

    /* ── Recognising a LOCI-USB (loci_hw_probe) ── */
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
