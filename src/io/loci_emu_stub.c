/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file loci_emu_stub.c
 * @brief Remplaçant de loci_emu.c quand l'émulateur RP2040 (libemul) est absent
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-11
 *
 * Le backend `--loci-emu` exécute le vrai firmware LOCI dans un émulateur
 * RP2040 externe (`~/loci/emul`, non versionné dans ce dépôt). Sans lui, le
 * Makefile lie ce fichier à la place : l'émulateur se construit et fonctionne
 * intégralement — y compris `--loci`, qui est le backend comportemental et ne
 * dépend pas du firmware — et `--loci-emu` échoue proprement au démarrage.
 * `make LOCI_EMU=1 LOCI_EMUL_DIR=...` rétablit la co-simulation.
 */
#include "io/loci_emu.h"
#include "utils/logging.h"

int loci_emu_start(const char *elf_path)
{
    log_error("--loci-emu: co-simulation RP2040 non compilée dans ce binaire "
              "(libemul absente au build) ; firmware ignoré : %s", elf_path ? elf_path : "");
    return -1;
}

void loci_emu_set_usb_image(const char *path) { (void)path; }
void loci_emu_stop(void) { }
bool loci_emu_active(void) { return false; }
bool loci_emu_menu_button(void) { return false; }

bool loci_emu_rom_read(uint16_t address, uint8_t *out)
{
    (void)address; (void)out;
    return false;
}

void loci_emu_ext_lines(int *nirq, int *nreset, int *nromdis)
{
    if (nirq) *nirq = 1;
    if (nreset) *nreset = 1;
    if (nromdis) *nromdis = 1;
}

void    loci_emu_api_write(uint16_t address, uint8_t value) { (void)address; (void)value; }
uint8_t loci_emu_api_read(uint16_t address) { (void)address; return 0xFF; }
int     loci_emu_irq_take(void) { return 0; }

void    loci_emu_set_cdc_device(const char *path) { (void)path; }
bool    loci_emu_acia_active(void) { return false; }
void    loci_emu_acia_write(uint16_t address, uint8_t value) { (void)address; (void)value; }
uint8_t loci_emu_acia_read(uint16_t address) { (void)address; return 0xFF; }
uint8_t loci_emu_acia_peek(uint16_t address) { (void)address; return 0xFF; }
void    loci_emu_acia_tick(void) { }

void loci_emu_tick(long steps) { (void)steps; }

bool loci_emu_mou_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan)
{
    (void)buttons; (void)dx; (void)dy; (void)wheel; (void)pan;
    return false;
}
bool loci_emu_mou_armed(void) { return false; }

bool loci_emu_kbd_report(uint8_t modifier, const uint8_t keycodes[6])
{
    (void)modifier; (void)keycodes;
    return false;
}
bool loci_emu_kbd_armed(void) { return false; }
