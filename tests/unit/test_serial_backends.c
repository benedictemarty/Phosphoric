/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_serial_backends.c
 * @brief Transports de la liaison série : modem AT, tcp, pty, port COM, smf,
 *        analyse des spécifications
 * @author bmarty <bmarty@mailo.com>
 *
 * Hermétique : tout passe par 127.0.0.1 et des pseudo-terminaux (openpty), sans
 * matériel ni réseau extérieur. Les transports sont pilotés comme le fait
 * l'ACIA : send() octet par octet, recv()/poll() à chaque sondage.
 */
#define _DEFAULT_SOURCE   /* openpty, usleep */
#include "io/serial_backend.h"
#include "io/acia6551.h"
#include "utils/logging.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { int b = tests_failed; printf("  %-52s", #name); fflush(stdout); name(); \
    if (tests_failed == b) { tests_passed++; printf("PASS\n"); } } while (0)
#define ASSERT_TRUE(x) do { if (!(x)) { \
    printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #x); tests_failed++; return; } } while (0)
#define ASSERT_HAS(hay, needle) do { if (!strstr((hay), (needle))) { \
    printf("FAIL\n    %s:%d: « %s » absent de « %s »\n", __FILE__, __LINE__, (needle), (hay)); \
    tests_failed++; return; } } while (0)

/* ── Outils ──────────────────────────────────────────────────────────── */

/* Serveur TCP local sur un port libre ; renvoie le descripteur d'écoute. */
static int listen_local(uint16_t* port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (fd < 0 || bind(fd, (struct sockaddr*)&a, sizeof a) != 0 || listen(fd, 2) != 0 ||
        getsockname(fd, (struct sockaddr*)&a, &len) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    return fd;
}

/* Port libre (lié puis relâché) pour un transport qui écoute lui-même. */
static uint16_t free_port(void) {
    uint16_t p = 0;
    int fd = listen_local(&p);
    if (fd >= 0) close(fd);
    return p;
}

static int connect_local(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (fd < 0 || connect(fd, (struct sockaddr*)&a, sizeof a) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

static void send_str(serial_backend_t* b, const char* s) {
    while (*s) b->send(b, (uint8_t)*s++);
}

/* Lit ce que le transport rend pendant au plus @p ms (s'arrête dès que
 * @p until apparaît, si non NULL). */
static void drain(serial_backend_t* b, char* out, size_t sz, int ms, const char* until) {
    size_t n = 0;
    out[0] = '\0';
    for (int t = 0; t < ms; t++) {
        uint8_t c;
        while (b->recv(b, &c) && n + 1 < sz) { out[n++] = (char)c; out[n] = '\0'; }
        if (until && strstr(out, until)) return;
        usleep(1000);
    }
}

/* Lit un descripteur (non bloquant) pendant au plus @p ms. */
static size_t read_fd(int fd, char* out, size_t sz, int ms, const char* until) {
    size_t n = 0;
    out[0] = '\0';
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    for (int t = 0; t < ms; t++) {
        ssize_t r = read(fd, out + n, sz - 1 - n);
        if (r > 0) { n += (size_t)r; out[n] = '\0'; }
        if (until && strstr(out, until)) break;
        usleep(1000);
    }
    fcntl(fd, F_SETFL, fl);
    return n;
}

static serial_backend_t* modem_open(uint16_t port, bool listen) {
    serial_backend_t* m = serial_backend_modem_create("", port, listen);
    if (m && !m->open(m)) { serial_backend_destroy(m); return NULL; }
    return m;
}

/* ── Modem AT ────────────────────────────────────────────────────────── */

TEST(test_modem_at_commands) {
    serial_backend_t* m = modem_open(0, false);
    char out[512];
    ASSERT_TRUE(m != NULL);
    ASSERT_TRUE(!m->connected(m));
    send_str(m, "AT\r");
    drain(m, out, sizeof out, 200, "OK\r\n");
    ASSERT_HAS(out, "AT\r");                 /* écho actif par défaut */
    ASSERT_HAS(out, "OK\r\n");
    send_str(m, "ATE0\r");
    drain(m, out, sizeof out, 200, "OK\r\n");
    ASSERT_HAS(out, "OK");
    send_str(m, "ATS0=2\r");
    drain(m, out, sizeof out, 200, "OK\r\n");
    ASSERT_TRUE(strncmp(out, "OK\r\n", 4) == 0);   /* plus d'écho */
    ASSERT_TRUE(m->state.modem.s0_rings == 2);
    send_str(m, "ATX\bZ\r");                 /* retour arrière : « ATZ » */
    drain(m, out, sizeof out, 200, "OK\r\n");
    ASSERT_HAS(out, "OK");
    ASSERT_TRUE(m->state.modem.echo && m->state.modem.s0_rings == 0);
    send_str(m, "ATQ\r");                     /* inconnue */
    drain(m, out, sizeof out, 200, "ERROR");
    ASSERT_HAS(out, "ERROR\r\n");
    send_str(m, "ATA\r");                     /* pas en écoute */
    drain(m, out, sizeof out, 200, "ERROR");
    ASSERT_HAS(out, "ERROR\r\n");
    serial_backend_destroy(m);
}

TEST(test_modem_dial_data_and_peer_hangup) {
    uint16_t port = 0;
    int srv = listen_local(&port);
    ASSERT_TRUE(srv >= 0);
    serial_backend_t* m = modem_open(0, false);
    ASSERT_TRUE(m != NULL);
    char cmd[64], out[512];
    snprintf(cmd, sizeof cmd, "ATDT127.0.0.1:%u\r", port);
    send_str(m, cmd);
    drain(m, out, sizeof out, 500, "CONNECT");
    ASSERT_HAS(out, "CONNECT\r\n");
    ASSERT_TRUE(m->connected(m));
    int peer = accept(srv, NULL, NULL);
    ASSERT_TRUE(peer >= 0);
    send_str(m, "HELLO");                     /* Oric → serveur */
    read_fd(peer, out, sizeof out, 500, "HELLO");
    ASSERT_HAS(out, "HELLO");
    ASSERT_TRUE(write(peer, "BBS>", 4) == 4); /* serveur → Oric */
    drain(m, out, sizeof out, 500, "BBS>");
    ASSERT_HAS(out, "BBS>");
    close(peer);                              /* le serveur raccroche */
    drain(m, out, sizeof out, 500, "NO CARRIER");
    ASSERT_HAS(out, "NO CARRIER");
    ASSERT_TRUE(!m->connected(m));
    close(srv);
    serial_backend_destroy(m);
}

TEST(test_modem_dial_refused) {
    serial_backend_t* m = modem_open(0, false);
    char cmd[64], out[256];
    ASSERT_TRUE(m != NULL);
    snprintf(cmd, sizeof cmd, "ATD127.0.0.1:%u\r", free_port());   /* personne n'écoute */
    send_str(m, cmd);
    drain(m, out, sizeof out, 500, "NO CARRIER");
    ASSERT_HAS(out, "NO CARRIER");
    ASSERT_TRUE(!m->connected(m));
    serial_backend_destroy(m);
}

/* Hayes : silence, « +++ » tapé d'affilée → mode commande (OK) ; ATH raccroche. */
TEST(test_modem_escape_and_hangup) {
    uint16_t port = 0;
    int srv = listen_local(&port);
    serial_backend_t* m = modem_open(0, false);
    char cmd[64], out[256];
    ASSERT_TRUE(srv >= 0 && m != NULL);
    snprintf(cmd, sizeof cmd, "ATD127.0.0.1:%u\r", port);
    send_str(m, cmd);
    drain(m, out, sizeof out, 500, "CONNECT");
    int peer = accept(srv, NULL, NULL);
    ASSERT_TRUE(peer >= 0 && m->connected(m));
    /* Sans silence avant, « x+++ » est une donnée : envoyée telle quelle. */
    send_str(m, "x+++");
    read_fd(peer, out, sizeof out, 300, "x+++");
    ASSERT_HAS(out, "x+++");
    ASSERT_TRUE(m->connected(m));
    for (int i = 0; i < 100; i++) { uint8_t c; m->recv(m, &c); }   /* silence (garde) */
    send_str(m, "+++");
    drain(m, out, sizeof out, 300, "OK");
    ASSERT_HAS(out, "OK");
    send_str(m, "ATH\r");
    drain(m, out, sizeof out, 300, "OK");
    ASSERT_HAS(out, "OK");
    ASSERT_TRUE(!m->connected(m));
    /* Après la donnée, le serveur n'a reçu aucun « + » de l'échappement. */
    read_fd(peer, out, sizeof out, 100, NULL);
    ASSERT_TRUE(strchr(out, '+') == NULL);
    close(peer);
    close(srv);
    serial_backend_destroy(m);
}

TEST(test_modem_listen_and_answer) {
    const uint16_t port = free_port();
    serial_backend_t* m = modem_open(port, true);
    char out[256];
    ASSERT_TRUE(m != NULL && m->state.modem.listen_fd >= 0);
    int caller = connect_local(port);         /* appel entrant */
    ASSERT_TRUE(caller >= 0);
    usleep(20000);
    send_str(m, "ATA\r");
    drain(m, out, sizeof out, 500, "CONNECT");
    ASSERT_HAS(out, "CONNECT");
    ASSERT_TRUE(m->connected(m));
    ASSERT_TRUE(write(caller, "RING?", 5) == 5);
    drain(m, out, sizeof out, 500, "RING?");
    ASSERT_HAS(out, "RING?");
    close(caller);
    serial_backend_destroy(m);
}

/* ── tcp ─────────────────────────────────────────────────────────────── */

TEST(test_tcp_both_ways) {
    uint16_t port = 0;
    int srv = listen_local(&port);
    serial_backend_t* t = serial_backend_tcp_create("127.0.0.1", port);
    char out[128];
    ASSERT_TRUE(srv >= 0 && t != NULL);
    ASSERT_TRUE(t->open(t));
    ASSERT_TRUE(t->connected(t));
    int peer = accept(srv, NULL, NULL);
    ASSERT_TRUE(peer >= 0);
    send_str(t, "ORIC");
    read_fd(peer, out, sizeof out, 500, "ORIC");
    ASSERT_HAS(out, "ORIC");
    ASSERT_TRUE(write(peer, "ATMOS", 5) == 5);
    for (int i = 0; i < 500 && !t->poll(t); i++) usleep(1000);
    ASSERT_TRUE(t->poll(t));
    drain(t, out, sizeof out, 200, "ATMOS");
    ASSERT_HAS(out, "ATMOS");
    close(peer);
    close(srv);
    serial_backend_destroy(t);
}

TEST(test_tcp_refused) {
    serial_backend_t* t = serial_backend_tcp_create("127.0.0.1", free_port());
    ASSERT_TRUE(t != NULL);
    ASSERT_TRUE(!t->open(t) || !t->connected(t));
    serial_backend_destroy(t);
}

/* ── pty et port COM ─────────────────────────────────────────────────── */

TEST(test_pty_both_ways) {
    serial_backend_t* p = serial_backend_pty_create();
    char out[128];
    ASSERT_TRUE(p != NULL && p->open(p));
    ASSERT_TRUE(p->state.pty.slave_name[0] == '/');
    int slave = open(p->state.pty.slave_name, O_RDWR | O_NOCTTY);
    ASSERT_TRUE(slave >= 0);
    struct termios tio;
    tcgetattr(slave, &tio);
    cfmakeraw(&tio);
    tcsetattr(slave, TCSANOW, &tio);
    ASSERT_TRUE(write(slave, "PTY", 3) == 3);
    drain(p, out, sizeof out, 500, "PTY");
    ASSERT_HAS(out, "PTY");
    send_str(p, "OK!");
    read_fd(slave, out, sizeof out, 500, "OK!");
    ASSERT_HAS(out, "OK!");
    close(slave);
    serial_backend_destroy(p);
}

TEST(test_com_on_pseudo_terminal) {
    int master = -1, slave = -1;
    char name[256], cfg[300], out[128];
    ASSERT_TRUE(openpty(&master, &slave, name, NULL, NULL) == 0);
    struct termios tio;
    tcgetattr(master, &tio);
    cfmakeraw(&tio);
    tcsetattr(master, TCSANOW, &tio);
    snprintf(cfg, sizeof cfg, "9600,8,N,1,%s", name);
    serial_backend_t* c = serial_backend_com_create(cfg);
    ASSERT_TRUE(c != NULL);
    ASSERT_TRUE(c->state.com.baud == 9600 && c->state.com.parity == 'N');
    ASSERT_TRUE(strcmp(c->state.com.device, name) == 0);
    ASSERT_TRUE(c->open(c));
    ASSERT_TRUE(c->connected(c));
    send_str(c, "COM");
    read_fd(master, out, sizeof out, 500, "COM");
    ASSERT_HAS(out, "COM");
    ASSERT_TRUE(write(master, "1200", 4) == 4);
    drain(c, out, sizeof out, 500, "1200");
    ASSERT_HAS(out, "1200");
    serial_backend_destroy(c);
    close(slave);
    close(master);
}

/* ── smf : fichier MIDI rejoué en MIDI IN cadencé ────────────────────── */

TEST(test_smf_replays_in_order) {
    static const uint8_t mid[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96,          /* format 0, 96 PPQN */
        'M','T','r','k', 0,0,0,12,
        0x00, 0x90, 0x3C, 0x7F,                            /* Note On  C4 */
        0x10, 0x80, 0x3C, 0x00,                            /* Note Off (16 tics) */
        0x00, 0xFF, 0x2F, 0x00                             /* fin de piste */
    };
    char path[] = "/tmp/phosphoric_test_smf_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0 && write(fd, mid, sizeof mid) == (ssize_t)sizeof mid);
    close(fd);
    serial_backend_t* s = serial_backend_smf_create(path, false);
    ASSERT_TRUE(s != NULL && s->open(s));
    uint8_t got[16];
    int n = 0;
    for (int t = 0; t < 2000 && n < 6; t++) {
        uint8_t c;
        while (n < 16 && s->recv(s, &c)) got[n++] = c;
        usleep(1000);
    }
    ASSERT_TRUE(n == 6);
    ASSERT_TRUE(got[0] == 0x90 && got[1] == 0x3C && got[2] == 0x7F);
    ASSERT_TRUE(got[3] == 0x80 && got[4] == 0x3C && got[5] == 0x00);
    ASSERT_TRUE(s->send(s, 0xF8));            /* TX de l'Oric : ignoré, sans erreur */
    serial_backend_destroy(s);
    unlink(path);
}

/* ── Digitelec DTL 2000 : DTR monte → appel, DTR tombe → raccroché ───── */

TEST(test_digitelec_dtr_dials_and_hangs_up) {
    static acia6551_t acia;
    uint16_t port = 0;
    int srv = listen_local(&port);
    char out[128];
    ASSERT_TRUE(srv >= 0);
    acia_init(&acia);
    serial_backend_t* d = serial_backend_digitelec_create("127.0.0.1", port, &acia);
    ASSERT_TRUE(d != NULL && d->open(d));
    ASSERT_TRUE(!d->connected(d));
    ASSERT_TRUE(!d->send(d, 'X'));            /* DTR bas : pas de porteuse */
    acia.command |= ACIA_CMD_DTR;             /* l'Oric décroche */
    d->poll(d);
    ASSERT_TRUE(d->connected(d));
    ASSERT_TRUE((acia.status & ACIA_STATUS_DCD) == 0);   /* porteuse (active basse) */
    int peer = accept(srv, NULL, NULL);
    ASSERT_TRUE(peer >= 0);
    send_str(d, "V23");
    read_fd(peer, out, sizeof out, 500, "V23");
    ASSERT_HAS(out, "V23");
    ASSERT_TRUE(write(peer, "3615", 4) == 4);
    drain(d, out, sizeof out, 500, "3615");
    ASSERT_HAS(out, "3615");
    acia.command &= (uint8_t)~ACIA_CMD_DTR;   /* l'Oric raccroche */
    d->poll(d);
    ASSERT_TRUE(!d->connected(d));
    ASSERT_TRUE((acia.status & ACIA_STATUS_DCD) != 0);
    close(peer);
    close(srv);
    serial_backend_destroy(d);
}

/* ── Analyse des spécifications (--serial, --dtl2000, --mageco) ──────── */

TEST(test_transport_specs) {
    static const struct { const char* spec; serial_backend_type_t type; } ok[] = {
        { "loopback", SERIAL_BACKEND_LOOPBACK },
        { "tcp:127.0.0.1:2323", SERIAL_BACKEND_TCP },
        { "pty", SERIAL_BACKEND_PTY },
        { "com:9600,8,N,1,/dev/null", SERIAL_BACKEND_COM },
        { "file:/dev/null", SERIAL_BACKEND_FILE },
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        serial_backend_t* b = serial_transport_create(ok[i].spec);
        ASSERT_TRUE(b != NULL);
        ASSERT_TRUE(b->type == ok[i].type);
        serial_backend_destroy(b);
    }
    serial_backend_t* t = serial_transport_create("tcp:bbs.example:2323");
    ASSERT_TRUE(t && strcmp(t->state.tcp.host, "bbs.example") == 0 && t->state.tcp.port == 2323);
    serial_backend_destroy(t);
    ASSERT_TRUE(serial_transport_create("inconnu") == NULL);
    ASSERT_TRUE(serial_transport_create("") == NULL);
}

int main(void) {
    printf("=== Transports de la liaison série ===\n");
    log_init(LOG_LEVEL_ERROR);
    RUN(test_modem_at_commands);
    RUN(test_modem_dial_data_and_peer_hangup);
    RUN(test_modem_dial_refused);
    RUN(test_modem_escape_and_hangup);
    RUN(test_modem_listen_and_answer);
    RUN(test_tcp_both_ways);
    RUN(test_tcp_refused);
    RUN(test_pty_both_ways);
    RUN(test_com_on_pseudo_terminal);
    RUN(test_smf_replays_in_order);
    RUN(test_digitelec_dtr_dials_and_hangs_up);
    RUN(test_transport_specs);
    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
