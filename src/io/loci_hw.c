/* SPDX-License-Identifier: EUPL-1.2 */
#define _POSIX_C_SOURCE 200809L
/*
 * loci_hw.c — backend LOCI « MATÉRIEL RÉEL » de Phosphoric (--loci-hw DEV).
 *
 * Même interface que loci_emu.h, mais au lieu d'exécuter le firmware dans
 * l'émulateur RP2040 (loci_emu.c) ou de ne rien faire (loci_emu_stub.c), chaque
 * accès du 6502 émulé à la page LOCI ($03xx) ou à la ROM servie ($C000-$FFFF sous
 * nROMDIS) devient un VRAI cycle de bus servi par une LOCI-USB : une LOCI sans
 * interface Oric (Feather RP2040, future carte LOCI-USB) dont le firmware LOCI
 * (variante LOCI_USB) rejoue ces accès, en parlant proto/loci_usb_proto.h sur USB
 * CDC. (La première piste, un Pico pont sur CN1 d'une cartouche inchangée, a été
 * abandonnée le 2026-09-13.)
 *
 * Compilé dans Phosphoric dès que le dépôt loci-usb est là (LOCI_HW, fonctions
 * renommées par loci_be_rename.h) et choisi au lancement par --loci-hw ou le mode
 * « usb » de la carte LOCI (aiguillage loci_backend.c). Source de vérité : cet
 * arbre (instantanés dans ~/loci/loci-usb/phosphoric/).
 *
 * Modèle :
 *  - le firmware réel tourne en continu : « booté » dès que le pont répond (PING) ;
 *    au démarrage, POWERON (caps & LUP_CAP_POWERON) le remet à l'état mise sous tension ;
 *  - $03xx : un aller-retour USB par accès (stop-and-wait, ~0,1-1 ms) ;
 *  - ROM servie : CACHE hôte de 16 Ko rempli par RDN (une banque en une requête),
 *    avec les flags nROMDIS/nMAP par adresse. Invalidé quand la GÉNÉRATION de la vue
 *    ROM (gen8, renvoyée par chaque réponse : base/MAP/trap/chargement changés côté
 *    firmware) bouge, à chaque front nRESET et à chaque changement de nROMDIS.
 *    LOCI_HW_ROM_NOCACHE=1 : pas de cache (un cycle par fetch, exact, lent) ;
 *  - nIRQ / nRESET : fronts comptés par le pont, drainés une fois par frame
 *    (loci_emu_irq_take / loci_emu_reset_take) ; le bouton MENU est PHYSIQUE : l'hôte
 *    voit le nRESET qui en résulte et resette son 6502 ;
 *  - clavier/souris USB : ceux branchés sur la cartouche (pas d'injection possible) ;
 *  - course Φ2 (caps & LUP_CAP_TIMING) : le stop-and-wait fige le 6502 pendant chaque
 *    accès, ce qui masque la contrainte Φ2 d'une vraie LOCI. RDT/WRT renvoient les
 *    cycles d'act_loop mesurés au SysTick du cœur 1 : SERVE (jusqu'au déclenchement de
 *    la DMA de read-serve) et ACT (jusqu'à la fin des effets de bord). Chronologie d'une
 *    lecture $03xx en ns, origine au front descendant de Φ2 qui ouvre le cycle :
 *      - le PIO de la LOCI tourne à Φ2cfg × 30 (Φ2cfg = réglage du firmware, 4000 kHz par
 *        défaut → 1 tick = 8,33 ns ; ce n'est PAS l'horloge de l'Oric) ;
 *      - mia_action pousse le mot dans la FIFO à (22 + tior) ticks + 2 cycles sys de
 *        synchroniseur (comptes lus dans mia.pio, non mesurés) ;
 *      - act_loop le prend (LOCI_HW_POLL_NS, non mesuré, 0) et déclenche la DMA SERVE
 *        cycles sys plus tard ;
 *      - mia_io_read attend Φ2 haut puis pilote le bus (3 + tiod) ticks après :
 *        donnée = max(montée Φ2 + synchro, prêt) + (3 + tiod) ticks.
 *    Φ2 de l'Oric : période 1/LOCI_HW_PHI2_KHZ (1000), haut le dernier tiers du cycle
 *    (LOCI_HW_PHI2_HIGH_NS, période/3 : l'ULA donne 2/3 bas, 1/3 haut). Lecture EN RETARD
 *    si la donnée arrive après la fin du cycle moins le temps d'établissement du 6502
 *    (LOCI_HW_TDSR_NS, 100). Un accès $03xx arrivé moins de ACT cycles 6502 après le
 *    précédent lit un iopage PÉRIMÉ. Détection seule par défaut (compteurs + journal) ;
 *    LOCI_HW_FAITHFUL=1 rend l'open-bus sur une lecture en retard.
 */
#include "io/loci_emu.h"
#include "utils/logging.h"
#include "loci_usb_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "io/bus_timing.h"

static lup_client_t g_c;
static int  g_active;             /* pont ouvert et PING OK */
static int  g_romdis;             /* dernier état connu de nROMDIS (1 = actif) */
static int  g_reset_pending;      /* fronts nRESET vus depuis le dernier loci_emu_reset_take */
static int  g_irq_pending;        /* impulsions nIRQ relevées hors lines_drain (bouton à chaud) */
static int  g_link_err_logged;
static long g_settle_us;          /* LOCI_HW_SETTLE_US : pause après chaque accès $03xx (banc émulé) */
static long g_idle_poll_cycles;   /* LOCI_HW_IDLE_POLL : cycles sans accès LOCI avant un LINES (0 = jamais) */
static long g_idle_cycles;        /* cycles 6502 écoulés depuis le dernier accès LOCI */
static unsigned long g_idle_polls, g_idle_polls_hit;

/* Course Φ2 (caps & LUP_CAP_TIMING) */
static int           g_timing;              /* RDT/WRT utilisés */
static lup_timing_t  g_tm;                  /* horloges et délais courants de la cartouche */
static long          g_phi2_khz = 1000;     /* LOCI_HW_PHI2_KHZ : Φ2 de l'Oric émulé (1 MHz) */
static double        g_high_ns;             /* LOCI_HW_PHI2_HIGH_NS : Φ2 haut (période/3) */
static double        g_tdsr_ns = 100;       /* LOCI_HW_TDSR_NS : établissement des données du 6502 */
static double        g_poll_ns;             /* LOCI_HW_POLL_NS : FIFO → act_loop, non mesuré */
static int           g_faithful;            /* LOCI_HW_FAITHFUL : retard → open-bus */
static int           g_read_lost;           /* dernière lecture perdue (mode fidèle) */
static long          g_since_access = 1L << 30;   /* cycles 6502 depuis le dernier accès $03xx */
static long          g_prev_act_cyc;        /* durée d'act du dernier accès, en cycles 6502 */
static unsigned long g_timed, g_late, g_stale;
static unsigned      g_serve_max, g_act_max;
static double        g_worst_margin = 1e9;  /* échéance - donnée, en ns (négatif = retard) */

/* Cache de la ROM servie ($C000-$FFFF) */
static uint8_t g_rom[16384], g_rom_flags[16384];
static int     g_rom_valid, g_rom_nocache;
static unsigned long g_rom_refills;
static unsigned long g_bal_writes, g_bal_cmds;   /* écritures BAL captées, commandes lancées */
#define BAL_GROUP_CONSOLE 2               /* groupe BAL exécuté côté 6502 */
static long g_bal_timeout_ms = 10000;   /* LOCI_HW_BAL_TIMEOUT_MS : attente maximale d'une commande BAL */

const char *loci_emu_backend_name(void) { return "hw"; }

static void link_error_once(const char *ctx)
{
    if (g_link_err_logged) return;
    g_link_err_logged = 1;
    log_error("LOCI-hw: %s — %s (les accès suivants rendent $FF)", ctx, lup_client_error(&g_c));
}

static void rom_invalidate(const char *why)
{
    if (g_rom_valid) log_debug("LOCI-hw: cache ROM invalidé (%s)", why);
    g_rom_valid = 0;
}

static uint8_t g_gen;          /* génération de la vue ROM du cache */
/* Appel d'API LOCI (écriture de MIA_OP $03AF) : sur un vrai Oric, le 6502 est déjà
 * dans la boucle d'attente de l'iopage ($03B0, `JSR MIA_SPIN` qui suit le STA)
 * quand le firmware agit, même s'il remplace la ROM servie (mia_api_boot charge
 * BASIC dans la banque du menu et fait bouger gen8). Ici le pont rapporte la
 * nouvelle génération dès la réponse à l'écriture : l'invalidation est différée
 * jusqu'à la lecture de MIA_SPIN ($03B0, entrée effective dans la boucle), sinon
 * les octets qui précèdent seraient relus dans la ROM déjà remplacée (menu LOCI →
 * ESC figé sur « Booting », PC=$0244 ; RETURN : call_loci_boot lit encore
 * MIA_XSTACK $03AC puis fait PLP / JMP MIA_SPIN → jam $B5AD). Les
 * invalidations nROMDIS et front nRESET restent immédiates. Sans cache
 * (LOCI_HW_ROM_NOCACHE), la ROM est lue en direct : la course demeure. */
#define LOCI_HW_MIA_OP   0x03AF
#define LOCI_HW_MIA_SPIN 0x03B0
static int g_op_defer;         /* écriture MIA_OP faite, MIA_SPIN pas encore lu */
static int g_gen_pending;      /* changement de génération vu pendant le report */
static void note_gen(void)
{
    if (g_c.gen != g_gen) {
        g_gen = g_c.gen;
        if (g_op_defer) g_gen_pending = 1;
        else rom_invalidate("génération");
    }
}
/* Lecture de MIA_SPIN après l'appel : le 6502 est dans l'iopage, la ROM peut changer. */
static void op_defer_release(void)
{
    if (!g_op_defer) return;
    g_op_defer = 0;
    if (g_gen_pending) { g_gen_pending = 0; rom_invalidate("génération (après appel API)"); }
}
/* Front nRESET : le cache est de toute façon invalidé, le report n'a plus d'objet. */
static void op_defer_cancel(void) { g_op_defer = 0; g_gen_pending = 0; }
static void note_flags(uint8_t flags)
{
    int romdis = (flags & LUP_F_NROMDIS) != 0;
    if (romdis != g_romdis) { g_romdis = romdis; rom_invalidate(romdis ? "nROMDIS actif" : "nROMDIS relâché"); }
    note_gen();
}

/* Trace des accès : LOCI_HW_TRACE=<fichier> (ou "-" = stderr) — une ligne par accès
 * $03xx (R/W, adresse, valeur, flags) ; les lectures répétées identiques sont comptées. */
static FILE *g_trace; static int g_trace_init;
static void trace_access(char dir, uint16_t addr, uint8_t v, uint8_t f)
{
    static char last_dir; static uint16_t last_addr; static uint8_t last_v; static unsigned repeat;
    if (!g_trace_init) { g_trace_init = 1; const char *p = getenv("LOCI_HW_TRACE");
        if (p && *p) { g_trace = (p[0] == '-' && !p[1]) ? stderr : fopen(p, "w"); if (g_trace) setvbuf(g_trace, NULL, _IOLBF, 0); } }
    if (!g_trace) return;
    if (dir == last_dir && addr == last_addr && v == last_v) { repeat++; return; }
    if (repeat) fprintf(g_trace, "   (x%u)\n", repeat + 1);
    repeat = 0; last_dir = dir; last_addr = addr; last_v = v;
    fprintf(g_trace, "%c $%04X %02X f=%02X\n", dir, addr, v, f);
}

/* Banc ÉMULÉ (loci_usb_emul) : le firmware y est bien plus lent que sur silicium
 * alors que le 6502 de Phosphoric court ; une IRQ de fin de secteur peut alors
 * arriver « en retard » par rapport au vrai matériel. LOCI_HW_SETTLE_US=n laisse au
 * firmware n µs de temps réel après chaque accès (0 = rien, défaut ; inutile sur silicium). */
static void settle(void)
{
    if (g_settle_us > 0) { struct timespec ts = { 0, g_settle_us * 1000L }; nanosleep(&ts, NULL); }
}

static void bridge_sync(void);  /* écritures postées et lectures groupées $03A4/$03A8 (plus bas) */
static void xs_settle(void);    /* lectures groupées : rejoue côté firmware les lectures servies */
static void xs_regs_written(uint16_t addr);   /* registres de porte écrits : réarmement permis */

/* ── course Φ2 ── */
static double period_ns(void) { return 1e6 / (double)g_phi2_khz; }
static double tick_ns(void)   { return 1e6 / ((double)g_tm.phi2_khz * 30.0); }   /* PIO = Φ2cfg × 30 */

static void timing_load(void)
{
    if (!g_timing) return;
    bridge_sync();
    if (lup_timing(&g_c, &g_tm) != 0 || !g_tm.sys_khz || !g_tm.phi2_khz) {
        log_warning("LOCI-hw: TIMING a échoué (%s) — course Φ2 non mesurée", lup_client_error(&g_c));
        g_timing = 0;
        return;
    }
    log_info("LOCI-hw: course Φ2 mesurée — sys %lu kHz, tick PIO %.2f ns (Φ2cfg %lu kHz), tior %u, tiod %u ; "
             "Oric : période %.0f ns, Φ2 haut %.0f ns, tDSR %.0f ns ; %s",
             (unsigned long)g_tm.sys_khz, tick_ns(), (unsigned long)g_tm.phi2_khz, g_tm.tior, g_tm.tiod,
             period_ns(), g_high_ns, g_tdsr_ns, g_faithful ? "fidèle (retard → open-bus)" : "détection seule");
}
/* Cycles du cœur 1 → cycles 6502 (arrondi supérieur). */
static long cyc_to_6502(unsigned cyc) { return (long)(((uint64_t)cyc * (uint64_t)g_phi2_khz + g_tm.sys_khz - 1) / g_tm.sys_khz); }

/* Instant (ns après le front descendant de Φ2) où la donnée d'une lecture servie en
 * `serve` cycles sys est sur le bus, et l'échéance du 6502 : chronologie partagée
 * avec le modèle émulé (bus_timing.h), paramétrée par les horloges réelles. */
static bus_loci_timing_t hw_timing(void)
{
    bus_loci_timing_t t = bus_loci_timing_default();
    t.sys_khz   = g_tm.sys_khz;
    t.pio_khz   = g_tm.phi2_khz * 30u;
    t.period_ps = (int64_t)(period_ns() * 1000.0);
    t.high_ps   = (int64_t)(g_high_ns * 1000.0);
    t.tdsr_ps   = (int64_t)(g_tdsr_ns * 1000.0);
    t.poll_ps   = (int64_t)(g_poll_ns * 1000.0);
    return t;
}
static double data_valid_ns(unsigned serve)
{
    bus_loci_timing_t t = hw_timing();
    return bus_loci_read_valid_ps(&t, g_tm.tior, g_tm.tiod, serve) / 1000.0;
}
static double deadline_ns(void) { bus_loci_timing_t t = hw_timing(); return bus_loci_deadline_ps(&t) / 1000.0; }

/* Accès $03xx arrivé avant la fin des effets de bord du précédent : iopage périmé. */
static void check_stale(char dir, uint16_t addr)
{
    if (g_prev_act_cyc > 0 && g_since_access < g_prev_act_cyc) {
        if (++g_stale <= 10)
            log_warning("LOCI-hw: Φ2 %c $%04X %ld cycles après l'accès précédent, dont l'action en dure %ld — iopage périmé",
                        dir, addr, g_since_access, g_prev_act_cyc);
    }
}
static void note_act(unsigned act)
{
    if (act > g_act_max) g_act_max = act;
    g_prev_act_cyc = act ? cyc_to_6502(act) : 0;
    g_since_access = 0;
}

/* ── Écritures postées sur les portes XRAM RW0 ($03A4) / RW1 ($03A8) ──
 * La sauvegarde à chaud (mia_save_state de la ROM du menu) écrit ~48 Ko de RAM Oric
 * dans $03A4, un octet à la fois : un aller-retour USB par octet (~1 min). Ces
 * écritures ne rendent rien au 6502 : elles sont mises en tampon et envoyées par WRN
 * (n écritures sur la même adresse, faites dans l'ordre par act_loop côté firmware).
 * Le tampon est vidé AVANT toute autre requête au pont (lecture, écriture d'une autre
 * adresse, ROM, BAL, LINES, bouton, HID, TIMING, fin de session, reset) : l'ordre vu
 * par le firmware est inchangé ; seuls nIRQ/gen8 de ces écritures arrivent en fin de
 * paquet. Vidé aussi après LOCI_HW_POST_IDLE cycles sans accès (latence bornée).
 * Pas de mesure de la course Φ2 pour elles (WRN ne renvoie pas act16).
 * LOCI_HW_NO_POST=1 : une requête par écriture, comme avant. */
#define LOCI_HW_RW0 0x03A4
#define LOCI_HW_RW1 0x03A8
#define LOCI_HW_POST_MAX  256      /* USBBUS_WRN_MAX du firmware */
#define LOCI_HW_POST_IDLE 2000     /* cycles 6502 sans accès LOCI avant vidage */
static int           g_post_off;
static uint8_t       g_post_buf[LOCI_HW_POST_MAX];
static unsigned      g_post_n;
static uint16_t      g_post_addr;
static unsigned long g_posted, g_post_wrn;
static void post_flush(void)
{
    if (!g_post_n) return;
    uint8_t f = 0;
    unsigned n = g_post_n;
    g_post_n = 0;
    if (lup_wrn(&g_c, g_post_addr, n, g_post_buf, &f) != 0) { link_error_once("écritures groupées (WRN)"); return; }
    g_post_wrn++;
    note_flags(f);
    settle();
}
static int post_write(uint16_t addr, uint8_t v)
{
    if (g_post_off || (addr != LOCI_HW_RW0 && addr != LOCI_HW_RW1)) return 0;
    xs_settle();
    xs_regs_written(addr);
    if (g_post_n && g_post_addr != addr) post_flush();
    g_post_addr = addr;
    g_post_buf[g_post_n++] = v;
    g_posted++;
    trace_access('w', addr, v, 0);
    g_idle_cycles = 0;
    g_since_access = 0;
    if (g_post_n == LOCI_HW_POST_MAX) post_flush();
    return 1;
}

/* ── Lectures groupées sur les portes RW0 / RW1 (caps XSTREAM) ──
 * La restauration (RETURN dans le menu) relit la sauvegarde par $03A4, un octet par
 * lecture. Firmware avec LUP_CAP_XSTREAM : à la 2e lecture consécutive de la même
 * porte, XPEEK rend d'avance les 256 octets que rendraient les lectures suivantes,
 * sans rien modifier ; elles sont servies ici sans requête. Avant toute autre
 * requête au pont (et à l'épuisement, et après LOCI_HW_POST_IDLE cycles sans accès),
 * XADV rejoue côté firmware les k lectures servies : la cartouche est alors dans
 * l'état exact où k lectures du 6502 l'auraient laissée. Pas d'armement si STEP = 0
 * (fenêtre clavier HID réécrite en tâche de fond) ; seul écart possible : la XRAM
 * modifiée côté firmware pendant la vie du tampon (bornée). Pas de mesure Φ2 pour les
 * lectures servies. LOCI_HW_NO_XSTREAM=1 : lectures unitaires. */
#ifdef LUP_CAP_XSTREAM
static int           g_xs_off;
static int           g_xs_ch = -1;          /* porte du tampon (0 = RW0, 1 = RW1), -1 : aucun */
static uint8_t       g_xs_buf[LUP_XPEEK_MAX];
static unsigned      g_xs_n, g_xs_pos;
static uint16_t      g_xs_last;             /* dernière adresse lue (détection de la série) */
static int           g_xs_noarm[2];         /* STEP vu à 0 : pas de réarmement avant une écriture des registres */
static unsigned long g_xs_served, g_xs_peeks;
static int xs_active(void) { return g_xs_ch >= 0; }
static void xs_settle(void)
{
    if (g_xs_ch < 0) return;
    int ch = g_xs_ch;
    unsigned k = g_xs_pos;
    g_xs_ch = -1;
    if (!k) return;
    uint8_t f = 0;
    if (lup_xadv(&g_c, (uint8_t)ch, k, &f) != 0) { link_error_once("lectures groupées (XADV)"); return; }
    note_flags(f);
}
/* Lecture de la porte servie par le tampon : 1 et *d si oui. */
static int xs_peek(uint8_t ch)
{
    int8_t step = 0;
    if (lup_xpeek(&g_c, ch, LUP_XPEEK_MAX, g_xs_buf, NULL, &step) != 0) { link_error_once("lectures groupées (XPEEK)"); return 0; }
    if (!step) { g_xs_noarm[ch] = 1; return 0; }
    g_xs_ch = ch; g_xs_n = LUP_XPEEK_MAX; g_xs_pos = 0; g_xs_peeks++;
    return 1;
}
static int xs_serve(uint16_t addr, uint8_t *d)
{
    if (g_xs_ch < 0) return 0;
    int port = addr == (g_xs_ch ? LOCI_HW_RW1 : LOCI_HW_RW0);
    if (port && g_xs_pos == g_xs_n) {            /* épuisé : XADV puis XPEEK, sans lecture réelle */
        uint8_t ch = (uint8_t)g_xs_ch;
        xs_settle();
        if (!xs_peek(ch)) return 0;
    }
    if (port && g_xs_pos < g_xs_n) {
        *d = g_xs_buf[g_xs_pos++];
        g_xs_served++;
        trace_access('r', addr, *d, 0);
        g_idle_cycles = 0;
        g_since_access = 0;
        return 1;
    }
    xs_settle();
    return 0;
}
/* Après une lecture réelle de la porte : 2e de la série → tampon armé. */
static void xs_arm(uint16_t addr)
{
    int same = addr == g_xs_last;
    g_xs_last = addr;
    if (g_xs_off || !(g_c.caps & LUP_CAP_XSTREAM) || !same) return;
    if (addr != LOCI_HW_RW0 && addr != LOCI_HW_RW1) return;
    uint8_t ch = addr == LOCI_HW_RW1;
    if (!g_xs_noarm[ch]) xs_peek(ch);
}
/* Écriture d'un registre de porte ($03A4-$03AB) : STEP/ADDR ont pu changer. */
static void xs_regs_written(uint16_t addr)
{
    if (addr < LOCI_HW_RW0 || addr > LOCI_HW_RW1 + 3) return;
    g_xs_noarm[0] = g_xs_noarm[1] = 0;
    g_xs_last = 0;
}
#else
static int  xs_active(void) { return 0; }
static void xs_settle(void) { }
static int  xs_serve(uint16_t addr, uint8_t *d) { (void)addr; (void)d; return 0; }
static void xs_arm(uint16_t addr) { (void)addr; }
static void xs_regs_written(uint16_t addr) { (void)addr; }
#endif

/* Avant toute autre requête au pont : lectures servies rejouées, puis écritures
 * postées envoyées (une écriture de la porte solde d'abord le tampon de lecture). */
static void bridge_sync(void)
{
    xs_settle();
    post_flush();
}

/* Cycle de bus générique (page $03xx). */
static uint8_t bus_rd(uint16_t addr)
{
    uint8_t d = 0xFF, f;
    if (!g_active) return 0xFF;
    if (xs_serve(addr, &d)) return d;
    bridge_sync();
    if (addr == LOCI_HW_MIA_SPIN) op_defer_release();   /* entrée dans la boucle d'attente */
    if (g_timing) {
        uint16_t serve = 0, act = 0;
        if (lup_rdt(&g_c, addr, &d, &f, &serve, &act) != 0) { link_error_once("lecture"); return 0xFF; }
        check_stale('R', addr);
        if (serve) {                         /* 0 : accès hors act_loop, rien à juger */
            g_timed++;
            if (serve > g_serve_max) g_serve_max = serve;
            double valid = data_valid_ns(serve), margin = deadline_ns() - valid;
            if (margin < g_worst_margin) g_worst_margin = margin;
            if (margin < 0) {
                if (++g_late <= 10)
                    log_warning("LOCI-hw: Φ2 lecture $%04X EN RETARD — serve %u cycles, donnée à %.0f ns > échéance %.0f ns%s",
                                addr, serve, valid, deadline_ns(), g_faithful ? " → open-bus" : "");
                if (g_faithful) g_read_lost = 1;
            }
        }
        note_act(act);
    } else {
        if (lup_rd(&g_c, addr, &d, &f) != 0) { link_error_once("lecture"); return 0xFF; }
    }
    note_flags(f);
    trace_access('R', addr, d, f);
    settle();
    g_idle_cycles = 0;
    xs_arm(addr);
    return d;
}

static void bus_wr(uint16_t addr, uint8_t v)
{
    uint8_t f;
    if (!g_active) return;
    if (post_write(addr, v)) return;
    bridge_sync();
    xs_regs_written(addr);
    if (addr == LOCI_HW_MIA_OP) g_op_defer = 1;   /* sa réponse peut déjà porter la nouvelle gen8 */
    if (g_timing) {
        uint16_t act = 0;
        if (lup_wrt(&g_c, addr, v, &f, &act) != 0) { link_error_once("écriture"); return; }
        check_stale('W', addr);
        note_act(act);
    } else {
        if (lup_wr(&g_c, addr, v, &f) != 0) { link_error_once("écriture"); return; }
    }
    note_flags(f);
    trace_access('W', addr, v, f);
    settle();
    g_idle_cycles = 0;
}

bool loci_emu_read_lost(void) { int l = g_read_lost; g_read_lost = 0; return l != 0; }

/* ── cycle de vie ── */
int loci_emu_start(const char *dev)
{
    g_rom_nocache = getenv("LOCI_HW_ROM_NOCACHE") != NULL;
    g_settle_us = getenv("LOCI_HW_SETTLE_US") ? atol(getenv("LOCI_HW_SETTLE_US")) : 0;
    g_idle_poll_cycles = getenv("LOCI_HW_IDLE_POLL") ? atol(getenv("LOCI_HW_IDLE_POLL")) : 1000;
    if (lup_open(&g_c, dev) != 0) {
        log_error("LOCI-hw: impossible d'ouvrir le pont « %s » : %s", dev, lup_client_error(&g_c));
        return -1;
    }
    /* La cartouche reste alimentée par l'USB entre deux sessions : sans remise à
     * l'état « mise sous tension », la session suivante hérite de l'état précédent
     * (2 fronts nRESET, gen8 instable, 6502 planté en pile). POWERON = l'équivalent de
     * l'allumage de l'Oric, qui remet aussi la LOCI à zéro. LOCI_HW_NO_POWERON=1 : garder l'état. */
    if ((g_c.caps & LUP_CAP_POWERON) && !getenv("LOCI_HW_NO_POWERON")) {
        if (lup_poweron(&g_c) != 0 || lup_reconnect(&g_c, dev, 10000) != 0) {
            log_error("LOCI-hw: POWERON a échoué : %s", lup_client_error(&g_c));
            lup_close(&g_c);
            return -1;
        }
        log_info("LOCI-hw: cartouche remise à l'état mise sous tension (POWERON)");
    }
    uint8_t lines = 0, irqs, rsts;
    if (lup_lines(&g_c, &lines, &irqs, &rsts) != 0) {
        log_error("LOCI-hw: LINES a échoué : %s", lup_client_error(&g_c));
        lup_close(&g_c);
        return -1;
    }
    g_romdis = (lines & LUP_L_NROMDIS) != 0;
    g_gen = g_c.gen;
    g_active = 1;
    g_phi2_khz  = getenv("LOCI_HW_PHI2_KHZ") ? atol(getenv("LOCI_HW_PHI2_KHZ")) : 1000;
    if (g_phi2_khz <= 0) g_phi2_khz = 1000;
    g_high_ns   = getenv("LOCI_HW_PHI2_HIGH_NS") ? atof(getenv("LOCI_HW_PHI2_HIGH_NS")) : period_ns() / 3;
    g_tdsr_ns   = getenv("LOCI_HW_TDSR_NS") ? atof(getenv("LOCI_HW_TDSR_NS")) : 100;
    g_poll_ns   = getenv("LOCI_HW_POLL_NS") ? atof(getenv("LOCI_HW_POLL_NS")) : 0;
    g_faithful  = getenv("LOCI_HW_FAITHFUL") != NULL;
    g_timing    = (g_c.caps & LUP_CAP_TIMING) != 0;
    g_timed = g_late = g_stale = 0; g_serve_max = g_act_max = 0; g_worst_margin = 1e9;
    g_read_lost = 0; g_prev_act_cyc = 0; g_since_access = 1L << 30;
    g_bal_writes = g_bal_cmds = 0; g_rom_valid = 0; g_rom_refills = 0; op_defer_cancel();
    g_irq_pending = 0;
    g_post_off = getenv("LOCI_HW_NO_POST") != NULL; g_post_n = 0; g_posted = g_post_wrn = 0;
#ifdef LUP_CAP_XSTREAM
    g_xs_off = getenv("LOCI_HW_NO_XSTREAM") != NULL; g_xs_ch = -1; g_xs_last = 0; g_xs_served = g_xs_peeks = 0;
    g_xs_noarm[0] = g_xs_noarm[1] = 0;
#endif
    g_bal_timeout_ms = getenv("LOCI_HW_BAL_TIMEOUT_MS") ? atol(getenv("LOCI_HW_BAL_TIMEOUT_MS")) : 10000;
    log_info("LOCI-hw: pont « %s » prêt (proto %u, firmware pont %u, caps %02X%s) — nROMDIS=%d nRESET=%d ; "
             "cache ROM %s", dev, g_c.proto, g_c.fw, g_c.caps,
             (g_c.caps & LUP_CAP_VIRTUAL) ? ", VIRTUEL" : "", g_romdis, (lines & LUP_L_NRESET) != 0,
             g_rom_nocache ? "désactivé" : "actif");
    if (g_settle_us > 0) log_info("LOCI-hw: stabilisation %ld µs après chaque accès (LOCI_HW_SETTLE_US)", g_settle_us);
    log_info("LOCI-hw: poll en attente %s (LOCI_HW_IDLE_POLL=%ld cycles)", g_idle_poll_cycles > 0 ? "actif" : "désactivé", g_idle_poll_cycles);
    if (g_timing) timing_load();
    else log_info("LOCI-hw: course Φ2 non mesurée (firmware sans TIMING)");
    return 0;
}

void loci_emu_set_usb_image(const char *path)  { if (path) log_warning("LOCI-hw: --loci-emu-usb-image ignoré (clé USB réelle sur la cartouche)"); }
void loci_emu_set_flash_image(const char *path) { if (path) log_warning("LOCI-hw: --loci-emu-flash ignoré (flash réelle de la cartouche)"); }
void loci_emu_set_cdc_device(const char *path)  { if (path) log_warning("LOCI-hw: --loci-cdc ignoré (modem USB réel sur la cartouche)"); }

void loci_emu_stop(void)
{
    if (!g_active) return;
    log_info("LOCI-hw: fin de session — %lu requêtes, %lu rechargements du cache ROM, %lu polls en attente (%lu avec événement)",
             g_c.n_req, g_rom_refills, g_idle_polls, g_idle_polls_hit);
    bridge_sync();
    if (g_bal_writes)
        log_info("LOCI-hw: BAL — %lu écritures captées, %lu commandes", g_bal_writes, g_bal_cmds);
    if (g_posted)
        log_info("LOCI-hw: écritures postées $03A4/$03A8 — %lu octets en %lu WRN", g_posted, g_post_wrn);
#ifdef LUP_CAP_XSTREAM
    if (g_xs_served)
        log_info("LOCI-hw: lectures groupées $03A4/$03A8 — %lu octets servis, %lu XPEEK", g_xs_served, g_xs_peeks);
#endif
    if (g_timing)
        log_info("LOCI-hw: course Φ2 — %lu lectures mesurées, %lu EN RETARD, %lu sur iopage périmé ; "
                 "serve max %u cycles, act max %u cycles, marge minimale %.0f ns",
                 g_timed, g_late, g_stale, g_serve_max, g_act_max, g_timed ? g_worst_margin : 0.0);
    lup_close(&g_c);
    g_active = 0;
}

bool loci_emu_active(void)     { return g_active != 0; }
bool loci_emu_wait_boot(void)  { return g_active != 0; }

/* ── bouton MENU : logiciel (protocole v2, firmware LOCI_USB) ou physique ──
 * Dans les deux cas le firmware pilote nRESET : l'hôte le voit au prochain
 * loci_emu_reset_take() et resette alors son 6502 — on renvoie donc false ici. */
static bool press_button(uint8_t action)
{
    if (!g_active) return false;
    if (g_c.caps & LUP_CAP_FIRMWARE) {
        bridge_sync();
        if (lup_btn(&g_c, action) != 0) { link_error_once("BTN"); return false; }
        /* Le firmware traite le bouton et charge sa ROM en TEMPS RÉEL (secondes en
         * émulation, dizaines de ms sur silicium) pendant que l'Oric émulé, lui, court :
         * on attend ici le relâchement de nRESET (au plus 10 s) pour que le reset du 6502
         * tombe sur une ROM complète — comme un vrai Oric maintenu en reset par LOCI. */
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        for (int i = 0; i < 500; i++) {
            uint8_t lines = 0, irqs = 0, rsts = 0;
            if (lup_lines(&g_c, &lines, &irqs, &rsts) != 0) { link_error_once("LINES"); return false; }
            int romdis = (lines & LUP_L_NROMDIS) != 0;
            if (romdis != g_romdis) { g_romdis = romdis; rom_invalidate("nROMDIS (bouton)"); }
            note_gen();
            /* Appui À CHAUD (machine en marche) : pas de reset ; le firmware met $FFFE
             * sur son piège $03BA, pulse nIRQ et attend que le 6502 TOURNANT y entre
             * (sinon, après 2 s, reboot de secours → jam). On rend donc la main tout de
             * suite et lines_drain livrera l'impulsion au 6502. */
            if (irqs && !rsts) {
                g_irq_pending += irqs;
                log_info("LOCI-hw: bouton MENU %s → nIRQ (piège de sauvegarde à chaud) après %d ms",
                         action == 2 ? "long" : "court", i * 20);
                return false;
            }
            if (rsts) { g_reset_pending += rsts; op_defer_cancel(); rom_invalidate("front nRESET (bouton)");
                        log_info("LOCI-hw: bouton MENU %s → nRESET relâché après %d ms", action == 2 ? "long" : "court", i * 20); return false; }
            nanosleep(&ts, NULL);
        }
        log_warning("LOCI-hw: bouton MENU %s envoyé, mais pas de nRESET en 10 s", action == 2 ? "long" : "court");
    } else {
        log_info("LOCI-hw: le bouton MENU est PHYSIQUE (sur la cartouche) — appuyez dessus ; "
                 "le 6502 sera réinitialisé quand LOCI pilotera nRESET");
    }
    return false;
}
bool loci_emu_menu_button(void)     { return press_button(1); }
bool loci_emu_button_was_warm(void) { return false; }
bool loci_emu_diag_button(void)     { return press_button(2); }

/* ── overlay ROM ── */
static int rom_refill(void)
{
    bridge_sync();
    if (lup_rdn(&g_c, 0xC000, 16384, g_rom, g_rom_flags) != 0) { link_error_once("RDN ROM"); return 0; }
    g_rom_valid = 1; g_rom_refills++;
    g_gen = g_c.gen;                     /* l'image est cohérente avec cette génération */
    int romdis = (g_rom_flags[0] & LUP_F_NROMDIS) != 0;
    if (romdis != g_romdis) g_romdis = romdis;
    return 1;
}

bool loci_emu_rom_read(uint16_t address, uint8_t *out)
{
    if (!g_active || address < 0xC000 || !g_romdis) return false;
    uint8_t d, f;
    if (g_rom_nocache) {
        bridge_sync();
        if (lup_rd(&g_c, address, &d, &f) != 0) { link_error_once("lecture ROM"); return false; }
        note_flags(f);
    } else {
        if (!g_rom_valid && !rom_refill()) return false;
        d = g_rom[address - 0xC000]; f = g_rom_flags[address - 0xC000];
    }
    if (!(f & LUP_F_NROMDIS) || (f & LUP_F_NMAP)) return false;   /* sous MAP : RAM overlay Oric */
    *out = d;
    return true;
}

bool loci_emu_romdis(void) { return g_active && g_romdis; }

void loci_emu_ext_lines(int *nirq, int *nreset, int *nromdis)
{
    uint8_t lines = 0;
    if (g_active) bridge_sync();
    if (!g_active || lup_lines(&g_c, &lines, NULL, NULL) != 0) { lines = 0; if (g_active) link_error_once("LINES"); }
    if (nirq)    *nirq    = (lines & LUP_L_NIRQ) != 0;
    if (nreset)  *nreset  = (lines & LUP_L_NRESET) != 0;
    if (nromdis) *nromdis = (lines & LUP_L_NROMDIS) != 0;
}

/* ── page $03xx : API, Microdisc, cassette, ACIA — tous de vrais cycles ── */
void    loci_emu_api_write(uint16_t address, uint8_t value) { bus_wr(address, value); }
uint8_t loci_emu_api_read(uint16_t address)                 { return bus_rd(address); }
void    loci_emu_dsk_write(uint16_t address, uint8_t value) { bus_wr(address, value); }
uint8_t loci_emu_dsk_read(uint16_t address)                 { return bus_rd(address); }
void    loci_emu_tap_write(uint16_t address, uint8_t value) { bus_wr(address, value); }
uint8_t loci_emu_tap_read(uint16_t address)                 { return bus_rd(address); }
/* Le moteur cassette (VIA ORB $0300) : sur un vrai bus, LOCI snoope l'écriture
 * en $0300 — on la rejoue (nIO bas : page $03xx), mais SEULEMENT sur un changement
 * de PB6 : la ROM réécrit ORB à chaque colonne du balayage clavier, un aller-retour
 * USB par écriture effondrerait l'émulation. */
void loci_emu_tap_motor(uint8_t via_orb)
{
    static int last = -1;
    int motor = (via_orb >> 6) & 1;
    if (motor == last) return;
    last = motor;
    bus_wr(0x0300, via_orb);
}
void    loci_emu_dsk_tick(void)                             { }

/* Fronts nIRQ comptés par le pont depuis le dernier drain (une fois par frame).
 * Profite du même aller-retour pour relever nROMDIS et les fronts nRESET. */
static int lines_drain(void)
{
    uint8_t lines = 0, irqs = 0, rsts = 0;
    if (!g_active) return 0;
    if (g_irq_pending) { int n = g_irq_pending; g_irq_pending = 0; return n; }   /* bouton à chaud */
    /* Écritures encore en tampon : le firmware ne les a pas vues, aucune impulsion ne
     * peut en découler (reflect_nirq suit CHAQUE écriture MIA : vider ici renverrait
     * à un WRN + un LINES par octet). Relevé après le vidage. */
    if (g_post_n || xs_active()) return 0;
    if (lup_lines(&g_c, &lines, &irqs, &rsts) != 0) { link_error_once("LINES"); return 0; }
    int romdis = (lines & LUP_L_NROMDIS) != 0;
    if (romdis != g_romdis) { g_romdis = romdis; rom_invalidate(romdis ? "nROMDIS actif" : "nROMDIS relâché"); }
    note_gen();
    if (rsts) { g_reset_pending += rsts; op_defer_cancel(); rom_invalidate("front nRESET"); }
    return irqs;
}

int loci_emu_irq_take(void) { return lines_drain(); }

/* Poll « en attente » : sans accès LOCI depuis LOCI_HW_IDLE_POLL cycles, un LINES.
 * Gratuit quand le programme parle à LOCI (le compteur est remis à zéro à chaque
 * accès), borne la latence des IRQ/reset asynchrones à N cycles quand il attend. */
int loci_emu_idle_poll(int cycles)
{
    if (g_since_access < (1L << 30)) g_since_access += cycles;
    if ((g_post_n || xs_active()) && g_since_access >= LOCI_HW_POST_IDLE) bridge_sync();
    if (!g_active || g_idle_poll_cycles <= 0) return 0;
    g_idle_cycles += cycles;
    if (g_idle_cycles < g_idle_poll_cycles) return 0;
    g_idle_cycles = 0;
    g_idle_polls++;
    int before = g_reset_pending;
    int irqs = lines_drain();
    if (irqs || g_reset_pending != before) g_idle_polls_hit++;
    return irqs ? irqs : (g_reset_pending != before ? -1 : 0);  /* -1 = reset seul : l'appelant relève loci_emu_reset_take */
}

/* Fronts nRESET pilotés par LOCI (bouton MENU physique, gel) depuis le dernier
 * appel : l'hôte doit alors réinitialiser son 6502. */
int loci_emu_reset_take(void)
{
    int n = g_reset_pending;
    g_reset_pending = 0;
    if (n) {
        bridge_sync();
        log_info("LOCI-hw: nRESET piloté par LOCI (%d front%s) → reset du 6502", n, n > 1 ? "s" : "");
        timing_load();                    /* Φ2 et délais ont pu changer dans le menu */
    }
    return n;
}

/* ── ACIA : servie par la cartouche elle-même ($0380-$0383, mode 1) ── */
bool    loci_emu_acia_active(void)              { return false; }   /* pas de dongle CDC côté hôte */
bool    loci_emu_acia_served(uint16_t address)  { return g_active && address >= 0x0380 && address <= 0x0383; }
void    loci_emu_acia_write(uint16_t address, uint8_t value) { bus_wr(address, value); }
uint8_t loci_emu_acia_read(uint16_t address)    { return bus_rd(address); }
/* Observation « non destructive » : sur du vrai matériel, lire $0380 consomme
 * l'octet reçu → on ne le lit pas ; les registres d'état/commande/contrôle, si. */
uint8_t loci_emu_acia_peek(uint16_t address)    { return address == 0x0380 ? 0xFF : bus_rd(address); }
void    loci_emu_acia_tick(void)                { }

void loci_emu_tick(long steps) { (void)steps; }   /* le firmware réel avance tout seul */

/* HID : avec le firmware LOCI_USB (caps FIRMWARE), le clavier/la souris de l'hôte
 * sont injectés par le protocole (kbd_report()/mou_report() réels du firmware) ;
 * sinon ce sont les périphériques branchés sur la cartouche. */
bool loci_emu_mou_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan)
{
    if (!g_active || !(g_c.caps & LUP_CAP_FIRMWARE)) return false;
    bridge_sync();
    if (lup_mou(&g_c, buttons, dx, dy, wheel, pan) != 0) { link_error_once("MOU"); return false; }
    return true;
}
bool loci_emu_mou_armed(void) { return g_active && (g_c.caps & LUP_CAP_FIRMWARE); }
bool loci_emu_kbd_report(uint8_t modifier, const uint8_t keycodes[6])
{
    if (!g_active || !(g_c.caps & LUP_CAP_FIRMWARE)) return false;
    bridge_sync();
    if (lup_kbd(&g_c, modifier, keycodes) != 0) { link_error_once("KBD"); return false; }
    return true;
}
bool loci_emu_kbd_armed(void) { return g_active && (g_c.caps & LUP_CAP_FIRMWARE); }

/* Écriture 6502 en page $FF sous nROMDIS : boîte aux lettres (BAL) de loci-fw. Le
 * firmware recopie l'octet dans l'image servie et répond SERVED (« captée ») sans
 * changer gen8 : le cache est mis à jour ici. Un octet non nul en $FF00 lance une
 * commande (sauf le groupe 2, Console, exécuté par le 6502 lui-même) ; on attend
 * sa fin ($FF00 relu à 0 par RD non caché, 10 s au plus,
 * LOCI_HW_BAL_TIMEOUT_MS) pour que la boucle
 * d'attente du kernel la voie tout de suite, au lieu du prochain LINES. Le dispatcher
 * change gen8 en rendant ses résultats : le cache est alors invalidé. L'ancien
 * firmware ne répond jamais SERVED ici (écriture ignorée) → false, comme avant. */
static long elapsed_ms(const struct timespec *t0)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)(t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}
bool loci_emu_rom_write(uint16_t address, uint8_t value)
{
    if (!g_active || (address & 0xFF00u) != 0xFF00u || !(g_c.caps & LUP_CAP_FIRMWARE)) return false;
    uint8_t f;
    bridge_sync();
    if (lup_wr(&g_c, address, value, &f) != 0) { link_error_once("écriture BAL"); return false; }
    note_flags(f);
    if (!(f & LUP_F_SERVED)) return false;          /* non captée : RAM overlay ou ROM */
    if (g_rom_valid) g_rom[address - 0xC000] = value;
    g_bal_writes++;
    /* Le groupe 2 (Console) n'est jamais traité par le firmware : c'est le kernel 6502
     * qui l'exécute (ADR-004 de loci-fw, dispatch.c) ; l'attendre figerait le 6502
     * jusqu'au délai. */
    if (address == 0xFF00u && value && value != BAL_GROUP_CONSOLE) {
        /* Délai en temps réel : une commande MSC (lecture de clé USB) ou console peut
         * durer des centaines de lectures sur silicium. */
        struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            uint8_t d = 0xFF, rf;
            if (lup_rd(&g_c, 0xFF00, &d, &rf) != 0) { link_error_once("attente BAL"); break; }
            note_flags(rf);
            if (d == 0) break;
            if (elapsed_ms(&t0) >= g_bal_timeout_ms) {
                log_warning("LOCI-hw: commande BAL $%02X toujours en cours après %ld ms", value, g_bal_timeout_ms);
                break;
            }
        }
        g_bal_cmds++;
    }
    return true;
}

/* Page I/O entière par cycles bus : propre au backend neo (loci-fw). */
bool    loci_emu_io_page(void) { return false; }
bool    loci_emu_io_read(uint16_t address, uint8_t *out) { (void)address; (void)out; return false; }
void    loci_emu_io_write(uint16_t address, uint8_t value) { (void)address; (void)value; }

/* Compteurs de la course Φ2 (tests ; hors interface loci_emu.h). */
void loci_hw_timing_stats(unsigned long *timed, unsigned long *late, unsigned long *stale)
{
    if (timed) *timed = g_timed;
    if (late)  *late  = g_late;
    if (stale) *stale = g_stale;
}
