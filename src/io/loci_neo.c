/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file loci_neo.c
 * @brief Backend co-sim pour le NOUVEAU firmware LOCI « loci-fw » (reprise de zéro)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-17
 *
 * Même interface que loci_emu.c (backend du firmware amont) mais aucun symbole de ce
 * firmware : tout passe par le pont `emul_neo` de ~/loci/emul (contrat d'émulation
 * ADR-003 : `loci_rom_image`/`loci_rom_base`, expandeur I²C, cycles bus PIO).
 *   - lectures $C000-$FFFF : servies O(1) quand /ROMDIS est actif ;
 *   - écritures $C000-$FFFF (page BAL $FF, ADR-002) : vrais cycles bus vers le PIO ;
 *   - page $03xx : non décodée par loci-fw pour l'instant → bus flottant ($FF).
 * Sélection : `make LOCI_NEO=1`, puis `--loci-emu <loci-fw.elf>`.
 */
#include "io/loci_emu.h"
#include "utils/logging.h"
#include "emul_neo.h"
#include <string.h>

static emul_t g_emul;
static int    g_active;

int loci_emu_start(const char *elf_path)
{
    if (emul_init(&g_emul, elf_path, 0x10000100u) != 0) {
        log_error("LOCI-neo: emul_init a échoué (%s)", elf_path);
        return -1;
    }
    log_info("LOCI-neo: firmware loci-fw (%s) — boot…", elf_path);
    if (!neo_boot(&g_emul)) {
        log_error("LOCI-neo: le firmware n'a pas armé le service ROM (/ROMDIS, nOE, /RESET)");
        return -1;
    }
    g_active = 1;
    uint8_t lo = 0, hi = 0;
    neo_serve_read(&g_emul, 0xFFFC, &lo); neo_serve_read(&g_emul, 0xFFFD, &hi);
    log_info("LOCI-neo: ROM servie, vecteur reset = $%02X%02X", hi, lo);
    return 0;
}

void loci_emu_set_usb_image(const char *path) { (void)path; }
void loci_emu_set_flash_image(const char *path) { (void)path; }
void loci_emu_set_cdc_device(const char *path) { (void)path; }
void loci_emu_stop(void) { if (g_active) { emul_free(&g_emul); g_active = 0; } }
bool loci_emu_active(void) { return g_active != 0; }
const char *loci_emu_backend_name(void) { return "neo"; }
int  loci_emu_reset_take(void) { return 0; }
int  loci_emu_idle_poll(int cycles) { (void)cycles; return 0; }
bool loci_emu_wait_boot(void) { return g_active != 0; }
bool loci_emu_menu_button(void) { return false; }
bool loci_emu_button_was_warm(void) { return false; }
bool loci_emu_diag_button(void) { return false; }

bool loci_emu_rom_read(uint16_t address, uint8_t *out)
{
    if (!g_active) return false;
    return neo_serve_read(&g_emul, address, out) != 0;
}

bool loci_emu_rom_write(uint16_t address, uint8_t value)
{
    if (!g_active || address < 0xC000u) return false;
    neo_bus_write(&g_emul, address, value);
    /* Écriture du groupe = début de commande : faire avancer le firmware jusqu'à la fin
     * ($FF00 = 0) ou un plafond — le modèle est événementiel, le firmware ne tourne pas
     * entre deux accès du 6502 (même principe que le poll de loci_emu.c). */
    if (address == 0xFF00u && value) {
        for (int k = 0; k < 500; k++) {
            uint8_t g = 0xFF;
            neo_peek(&g_emul, 0xFF00, &g);
            if (g == 0) break;
            emul_step(&g_emul, 1000L);
        }
    }
    return true;
}

bool loci_emu_romdis(void)
{
    if (!g_active) return false;
    int nromdis = 0;
    emul_ext_lines(&g_emul, NULL, NULL, &nromdis);
    return nromdis != 0;
}

void loci_emu_ext_lines(int *nirq, int *nreset, int *nromdis)
{
    if (!g_active) { if (nirq) *nirq = 0; if (nreset) *nreset = 0; if (nromdis) *nromdis = 0; return; }
    emul_ext_lines(&g_emul, nirq, nreset, nromdis);
}

/* Page $03xx : rien de décodé par loci-fw au lot 1. */
void    loci_emu_api_write(uint16_t address, uint8_t value) { (void)address; (void)value; }
uint8_t loci_emu_api_read(uint16_t address) { (void)address; return 0xFF; }
void    loci_emu_dsk_write(uint16_t address, uint8_t value) { (void)address; (void)value; }
uint8_t loci_emu_dsk_read(uint16_t address) { (void)address; return 0xFF; }
void    loci_emu_tap_write(uint16_t address, uint8_t value) { (void)address; (void)value; }
uint8_t loci_emu_tap_read(uint16_t address) { (void)address; return 0xFF; }
void    loci_emu_tap_motor(uint8_t via_orb) { (void)via_orb; }
void    loci_emu_dsk_tick(void) { }
int     loci_emu_irq_take(void) { return 0; }
bool    loci_emu_acia_active(void) { return false; }
bool    loci_emu_acia_served(uint16_t address) { (void)address; return false; }
void    loci_emu_acia_write(uint16_t address, uint8_t value) { (void)address; (void)value; }
uint8_t loci_emu_acia_read(uint16_t address) { (void)address; return 0xFF; }
uint8_t loci_emu_acia_peek(uint16_t address) { (void)address; return 0xFF; }
void    loci_emu_acia_tick(void) { }

/* Fond de tâche : le firmware avance librement (boucle principale) une fois par trame. */
void loci_emu_tick(long steps) { if (g_active) emul_step(&g_emul, steps); }

bool loci_emu_mou_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan)
{ (void)buttons; (void)dx; (void)dy; (void)wheel; (void)pan; return false; }
bool loci_emu_mou_armed(void) { return false; }
bool loci_emu_kbd_report(uint8_t modifier, const uint8_t keycodes[6]) { (void)modifier; (void)keycodes; return false; }
bool loci_emu_kbd_armed(void) { return false; }
