/* SPDX-License-Identifier: EUPL-1.2 */
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
 *    avec les flags nROMDIS/nMAP échantillonnés PAR ADRESSE dans le cycle. Invalidé
 *    à chaque écriture d'opcode ($03AF), à chaque front nRESET et à chaque changement
 *    de nROMDIS. LOCI_HW_ROM_NOCACHE=1 : pas de cache (un cycle par fetch, exact, lent) ;
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

static lup_client_t g_c;
static int  g_active;             /* pont ouvert et PING OK */
static int  g_romdis;             /* dernier état connu de nROMDIS (1 = actif) */
static int  g_reset_pending;      /* fronts nRESET vus depuis le dernier loci_emu_reset_take */
static int  g_link_err_logged;

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

static void note_flags(uint8_t flags)
{
    int romdis = (flags & LUP_F_NROMDIS) != 0;
    if (romdis != g_romdis) { g_romdis = romdis; rom_invalidate(romdis ? "nROMDIS actif" : "nROMDIS relâché"); }
}

/* Cycle de bus générique (page $03xx). */
static uint8_t bus_rd(uint16_t addr)
{
    uint8_t d = 0xFF, f;
    if (!g_active) return 0xFF;
    if (lup_rd(&g_c, addr, &d, &f) != 0) { link_error_once("lecture"); return 0xFF; }
    note_flags(f);
    return d;
}

static void bus_wr(uint16_t addr, uint8_t v)
{
    uint8_t f;
    if (!g_active) return;
    if (lup_wr(&g_c, addr, v, &f) != 0) { link_error_once("écriture"); return; }
    note_flags(f);
    if (addr == 0x03AF) rom_invalidate("opcode $03AF");   /* un appel API peut changer la ROM servie / le banking */
}

/* ── cycle de vie ── */
int loci_emu_start(const char *dev)
{
    g_rom_nocache = getenv("LOCI_HW_ROM_NOCACHE") != NULL;
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
    g_active = 1;
    log_info("LOCI-hw: pont « %s » prêt (proto %u, firmware pont %u, caps %02X%s) — nROMDIS=%d nRESET=%d ; "
             "cache ROM %s", dev, g_c.proto, g_c.fw, g_c.caps,
             (g_c.caps & LUP_CAP_VIRTUAL) ? ", VIRTUEL" : "", g_romdis, (lines & LUP_L_NRESET) != 0,
             g_rom_nocache ? "désactivé" : "actif");
    return 0;
}

void loci_emu_set_usb_image(const char *path)  { if (path) log_warning("LOCI-hw: --loci-emu-usb-image ignoré (clé USB réelle sur la cartouche)"); }
void loci_emu_set_flash_image(const char *path) { if (path) log_warning("LOCI-hw: --loci-emu-flash ignoré (flash réelle de la cartouche)"); }
void loci_emu_set_cdc_device(const char *path)  { if (path) log_warning("LOCI-hw: --loci-cdc ignoré (modem USB réel sur la cartouche)"); }

void loci_emu_stop(void)
{
    if (!g_active) return;
    log_info("LOCI-hw: fin de session — %lu requêtes, %lu rechargements du cache ROM", g_c.n_req, g_rom_refills);
    lup_close(&g_c);
    g_active = 0;
}

bool loci_emu_active(void)     { return g_active != 0; }
bool loci_emu_wait_boot(void)  { return g_active != 0; }

/* ── bouton MENU : physique ── */
bool loci_emu_menu_button(void)
{
    log_info("LOCI-hw: le bouton MENU est PHYSIQUE (sur la cartouche) — appuyez dessus ; "
             "le 6502 sera réinitialisé quand LOCI pilotera nRESET");
    return false;
}
bool loci_emu_button_was_warm(void) { return false; }
bool loci_emu_diag_button(void)     { return loci_emu_menu_button(); }

/* ── overlay ROM ── */
static int rom_refill(void)
{
    if (lup_rdn(&g_c, 0xC000, 16384, g_rom, g_rom_flags) != 0) { link_error_once("RDN ROM"); return 0; }
    g_rom_valid = 1; g_rom_refills++;
    note_flags(g_rom_flags[0]);
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
int loci_emu_irq_take(void)
{
    uint8_t lines = 0, irqs = 0, rsts = 0;
    if (!g_active) return 0;
    if (lup_lines(&g_c, &lines, &irqs, &rsts) != 0) { link_error_once("LINES"); return 0; }
    int romdis = (lines & LUP_L_NROMDIS) != 0;
    if (romdis != g_romdis) { g_romdis = romdis; rom_invalidate(romdis ? "nROMDIS actif" : "nROMDIS relâché"); }
    if (rsts) { g_reset_pending += rsts; rom_invalidate("front nRESET"); }
    return irqs;
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

/* HID : les périphériques USB sont ceux de la cartouche. */
bool loci_emu_mou_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan)
{ (void)buttons; (void)dx; (void)dy; (void)wheel; (void)pan; return false; }
bool loci_emu_mou_armed(void) { return false; }
bool loci_emu_kbd_report(uint8_t modifier, const uint8_t keycodes[6]) { (void)modifier; (void)keycodes; return false; }
bool loci_emu_kbd_armed(void) { return false; }
