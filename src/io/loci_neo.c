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
#include "bus.h"
#include "soc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static emul_t g_emul;
static int    g_active;
static int    g_reset_pending;   /* /RESET Oric affirmé par le firmware, à livrer au 6502 */
static char   g_usb_image[1024]; /* --loci-usb-image : clé USB émulée (image FAT) */
static int    g_hid;             /* clavier/souris USB émulés (hooks usbhid_*) */

/* Observe la ligne /RESET pendant que le firmware tourne (14/5 : ROM tierce). */
static void neo_run(long steps)
{
    emul_step(&g_emul, steps);
    int nreset = 0; emul_ext_lines(&g_emul, NULL, &nreset, NULL);
    if (nreset) g_reset_pending = 1;
}

/* Préchargement de fichiers dans le FS interne (test) : LOCI_NEO_FILES="NOM=chemin,NOM=chemin".
 * Écrit chaque fichier par la BAL (groupe 3, canal 7, tranches de 190 octets via $FF10). */
static void neo_preload_files(void)
{
    const char *spec = getenv("LOCI_NEO_FILES");
    if (!spec || !*spec) return;
    char buf[4096]; strncpy(buf, spec, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char *name = tok, *path = eq + 1;
        FILE *f = fopen(path, "rb");
        if (!f) { log_warning("LOCI-neo: préchargement « %s » : %s illisible", name, path); continue; }
        size_t nl = strlen(name); if (nl > 79) nl = 79;
        neo_bus_write(&g_emul, 0xFF10, (uint8_t)nl);
        for (size_t i = 0; i < nl; i++) neo_bus_write(&g_emul, (uint16_t)(0xFF11 + i), (uint8_t)name[i]);
        uint8_t po[4] = { 7, 0x10, 0xFF, 3 };
        if (!neo_bal_call(&g_emul, 3, 4, po, 4)) { fclose(f); continue; }
        uint8_t chunk[190]; size_t n, total = 0;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
            for (size_t i = 0; i < n; i++) neo_bus_write(&g_emul, (uint16_t)(0xFF10 + i), chunk[i]);
            uint8_t pw[5] = { 7, 0x10, 0xFF, (uint8_t)n, (uint8_t)(n >> 8) };
            if (!neo_bal_call(&g_emul, 3, 9, pw, 5)) break;
            total += n;
        }
        fclose(f);
        uint8_t pc[1] = { 7 }; neo_bal_call(&g_emul, 3, 5, pc, 1);
        log_info("LOCI-neo: préchargé « %s » (%zu octets) depuis %s", name, total, path);
    }
}

int loci_emu_start(const char *elf_path)
{
    if (emul_init(&g_emul, elf_path, 0x10000100u) != 0) {
        log_error("LOCI-neo: emul_init a échoué (%s)", elf_path);
        return -1;
    }
    g_hid = neo_hid_enable(&g_emul);
    if (!g_hid) log_warning("LOCI-neo: symboles usbhid_* absents — clavier/souris USB non émulés");
    if (g_usb_image[0]) {
        if (neo_usb_set_image(&g_emul, g_usb_image)) log_info("LOCI-neo: clé USB émulée « %s » (volume 1:)", g_usb_image);
        else log_warning("LOCI-neo: image USB « %s » illisible ou symboles usbdisk_* absents", g_usb_image);
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
    neo_preload_files();
    return 0;
}

void loci_emu_set_usb_image(const char *path) { if (path) { strncpy(g_usb_image, path, sizeof g_usb_image - 1); g_usb_image[sizeof g_usb_image - 1] = 0; } }
void loci_emu_set_flash_image(const char *path) { (void)path; }
void loci_emu_set_cdc_device(const char *path) { (void)path; }
void loci_emu_stop(void) { if (g_active) { emul_free(&g_emul); g_active = 0; } }
bool loci_emu_active(void) { return g_active != 0; }
const char *loci_emu_backend_name(void) { return "neo"; }
int  loci_emu_reset_take(void)
{
    if (!g_reset_pending) return 0;
    /* attendre le relâchement de /RESET par le firmware */
    for (int k = 0; k < 200; k++) {
        int nreset = 0; emul_ext_lines(&g_emul, NULL, &nreset, NULL);
        if (!nreset) break;
        emul_step(&g_emul, 1000L);
    }
    g_reset_pending = 0;
    log_info("LOCI-neo: /RESET Oric relâché par le firmware → redémarrage du 6502");
    return 1;
}
int  loci_emu_idle_poll(int cycles) { (void)cycles; return 0; }
bool loci_emu_wait_boot(void) { return g_active != 0; }
/* Bouton de la cartouche (Ctrl+Alt+M) : appui court → le firmware ramène l'Oric sur le
 * kernel (/RESET). Le firmware scrute le bouton toutes les 20 ms : on avance le temps émulé. */
bool loci_emu_menu_button(void)
{
    if (!g_active) return false;
    emul_ioexp_set(&g_emul, EXT_REG_IN, 0x00);            /* appuyé (actif bas) */
    int seen = 0, nreset = 0;
    for (int k = 0; k < 4000; k++) {
        soc_timer_advance_us(5000);
        emul_step(&g_emul, 1000L);
        emul_ext_lines(&g_emul, NULL, &nreset, NULL);
        if (nreset) seen = 1;
        if (seen && !nreset) break;
    }
    emul_ioexp_set(&g_emul, EXT_REG_IN, 0x04);            /* relâché */
    emul_step(&g_emul, 100000L);
    g_reset_pending = 0;                                   /* livré ici : l'appelant fait cpu_reset */
    if (seen) log_info("LOCI-neo: bouton → retour au kernel (/RESET)");
    return seen != 0;
}
bool loci_emu_button_was_warm(void) { return false; }
bool loci_emu_diag_button(void) { return false; }

bool loci_emu_rom_read(uint16_t address, uint8_t *out)
{
    if (!g_active) return false;
    int served = neo_serve_read(&g_emul, address, out);
    /* Scrutation de $FF00 pendant une commande (WaitMessage) : le modèle est événementiel,
     * le firmware ne tourne pas entre deux accès → l'avancer à chaque lecture, sinon une
     * commande longue (commit littlefs) ne se termine jamais (même principe que le poll de
     * loci_emu.c). */
    if (served && address == 0xFF00u && out && *out != 0 && !g_reset_pending) {
        neo_run(2000L);
        neo_serve_read(&g_emul, address, out);
    }
    return served != 0;
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
            if (g == 0 || g_reset_pending) break;
            neo_run(1000L);
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
void loci_emu_tick(long steps) { if (g_active) neo_run(steps); }

/* HID USB émulé (clavier/souris de l'hôte → contrat usbhid_* du firmware). */
bool loci_emu_mou_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan)
{ (void)pan; if (!g_hid) return false; neo_hid_mouse_report(buttons, dx, dy, wheel, 1); return true; }
bool loci_emu_mou_armed(void) { return g_hid != 0; }
bool loci_emu_kbd_report(uint8_t modifier, const uint8_t keycodes[6])
{ if (!g_hid) return false; neo_hid_key_report(modifier, keycodes); return true; }
bool loci_emu_kbd_armed(void) { return g_hid != 0; }
