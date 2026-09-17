/* SPDX-License-Identifier: EUPL-1.2 */
#define _POSIX_C_SOURCE 200809L
/*
 * loci_hw.c — backend LOCI « MATÉRIEL RÉEL » de Phosphoric (--loci-hw DEV).
 *
 * Même interface que loci_emu.h, mais au lieu d'exécuter le firmware dans
 * l'émulateur RP2040 (loci_emu.c) ou de ne rien faire (loci_emu_stub.c), chaque
 * accès du 6502 émulé à la page LOCI ($03xx) ou à la ROM servie ($C000-$FFFF sous
 * nROMDIS) devient un VRAI cycle de bus sur la cartouche, via le pont Pico
 * loci-usb (bridge/) branché sur CN1 et parlant proto/loci_usb_proto.h sur USB CDC.
 *
 * Choisi à la compilation : `make LOCI_HW=1` (remplace loci_emu.c ; un binaire
 * Phosphoric = un backend). Source de vérité : ~/loci/loci-usb/phosphoric/loci_hw.c.
 *
 * Modèle :
 *  - le firmware réel tourne en continu : « booté » dès que le pont répond (PING) ;
 *  - $03xx : un aller-retour USB par accès (stop-and-wait, ~0,1-1 ms) ;
 *  - ROM servie : CACHE hôte de 16 Ko rempli par RDN (une banque en une requête),
 *    avec les flags nROMDIS/nMAP par adresse. Invalidé quand la GÉNÉRATION de la vue
 *    ROM (gen8, renvoyée par chaque réponse : base/MAP/trap/chargement changés côté
 *    firmware) bouge, à chaque front nRESET et à chaque changement de nROMDIS.
 *    LOCI_HW_ROM_NOCACHE=1 : pas de cache (un cycle par fetch, exact, lent) ;
 *  - nIRQ / nRESET : fronts comptés par le pont, drainés une fois par frame
 *    (loci_emu_irq_take / loci_emu_reset_take) ; le bouton MENU est PHYSIQUE : l'hôte
 *    voit le nRESET qui en résulte et resette son 6502 ;
 *  - clavier/souris USB : ceux branchés sur la cartouche (pas d'injection possible).
 */
#include "io/loci_emu.h"
#include "utils/logging.h"
#include "loci_usb_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static lup_client_t g_c;
static int  g_active;             /* pont ouvert et PING OK */
static int  g_romdis;             /* dernier état connu de nROMDIS (1 = actif) */
static int  g_reset_pending;      /* fronts nRESET vus depuis le dernier loci_emu_reset_take */
static int  g_link_err_logged;
static long g_settle_us;          /* LOCI_HW_SETTLE_US : pause après chaque accès $03xx (banc émulé) */
static long g_idle_poll_cycles;   /* LOCI_HW_IDLE_POLL : cycles sans accès LOCI avant un LINES (0 = jamais) */
static long g_idle_cycles;        /* cycles 6502 écoulés depuis le dernier accès LOCI */
static unsigned long g_idle_polls, g_idle_polls_hit;

/* Cache de la ROM servie ($C000-$FFFF) */
static uint8_t g_rom[16384], g_rom_flags[16384];
static int     g_rom_valid, g_rom_nocache;
static unsigned long g_rom_refills;

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
static void note_gen(void)
{
    if (g_c.gen != g_gen) { g_gen = g_c.gen; rom_invalidate("génération"); }
}
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

/* Cycle de bus générique (page $03xx). */
static uint8_t bus_rd(uint16_t addr)
{
    uint8_t d = 0xFF, f;
    if (!g_active) return 0xFF;
    if (lup_rd(&g_c, addr, &d, &f) != 0) { link_error_once("lecture"); return 0xFF; }
    note_flags(f);
    trace_access('R', addr, d, f);
    settle();
    g_idle_cycles = 0;
    return d;
}

static void bus_wr(uint16_t addr, uint8_t v)
{
    uint8_t f;
    if (!g_active) return;
    if (lup_wr(&g_c, addr, v, &f) != 0) { link_error_once("écriture"); return; }
    note_flags(f);
    trace_access('W', addr, v, f);
    settle();
    g_idle_cycles = 0;
}

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
    uint8_t lines = 0, irqs, rsts;
    if (lup_lines(&g_c, &lines, &irqs, &rsts) != 0) {
        log_error("LOCI-hw: LINES a échoué : %s", lup_client_error(&g_c));
        lup_close(&g_c);
        return -1;
    }
    g_romdis = (lines & LUP_L_NROMDIS) != 0;
    g_gen = g_c.gen;
    g_active = 1;
    log_info("LOCI-hw: pont « %s » prêt (proto %u, firmware pont %u, caps %02X%s) — nROMDIS=%d nRESET=%d ; "
             "cache ROM %s", dev, g_c.proto, g_c.fw, g_c.caps,
             (g_c.caps & LUP_CAP_VIRTUAL) ? ", VIRTUEL" : "", g_romdis, (lines & LUP_L_NRESET) != 0,
             g_rom_nocache ? "désactivé" : "actif");
    if (g_settle_us > 0) log_info("LOCI-hw: stabilisation %ld µs après chaque accès (LOCI_HW_SETTLE_US)", g_settle_us);
    log_info("LOCI-hw: poll en attente %s (LOCI_HW_IDLE_POLL=%ld cycles)", g_idle_poll_cycles > 0 ? "actif" : "désactivé", g_idle_poll_cycles);
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
            if (rsts) { g_reset_pending += rsts; rom_invalidate("front nRESET (bouton)");
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
    if (lup_lines(&g_c, &lines, &irqs, &rsts) != 0) { link_error_once("LINES"); return 0; }
    int romdis = (lines & LUP_L_NROMDIS) != 0;
    if (romdis != g_romdis) { g_romdis = romdis; rom_invalidate(romdis ? "nROMDIS actif" : "nROMDIS relâché"); }
    note_gen();
    if (rsts) { g_reset_pending += rsts; rom_invalidate("front nRESET"); }
    return irqs;
}

int loci_emu_irq_take(void) { return lines_drain(); }

/* Poll « en attente » : sans accès LOCI depuis LOCI_HW_IDLE_POLL cycles, un LINES.
 * Gratuit quand le programme parle à LOCI (le compteur est remis à zéro à chaque
 * accès), borne la latence des IRQ/reset asynchrones à N cycles quand il attend. */
int loci_emu_idle_poll(int cycles)
{
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
    if (n) log_info("LOCI-hw: nRESET piloté par LOCI (%d front%s) → reset du 6502", n, n > 1 ? "s" : "");
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
    if (lup_mou(&g_c, buttons, dx, dy, wheel, pan) != 0) { link_error_once("MOU"); return false; }
    return true;
}
bool loci_emu_mou_armed(void) { return g_active && (g_c.caps & LUP_CAP_FIRMWARE); }
bool loci_emu_kbd_report(uint8_t modifier, const uint8_t keycodes[6])
{
    if (!g_active || !(g_c.caps & LUP_CAP_FIRMWARE)) return false;
    if (lup_kbd(&g_c, modifier, keycodes) != 0) { link_error_once("KBD"); return false; }
    return true;
}
bool loci_emu_kbd_armed(void) { return g_active && (g_c.caps & LUP_CAP_FIRMWARE); }

bool loci_emu_rom_write(uint16_t address, uint8_t value) { (void)address; (void)value; return false; }
