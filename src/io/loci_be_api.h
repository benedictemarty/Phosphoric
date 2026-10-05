/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file loci_be_api.h
 * @brief Liste des fonctions de l'interface loci_emu.h servies par chaque backend
 *        LOCI (aiguillage au lancement, loci_backend.c)
 * @author bmarty <bmarty@mailo.com>
 *
 * LOCI_BE_API(F, V, H) : F(type, nom, (paramètres), (arguments)) pour une
 * fonction qui rend une valeur, V(nom, (paramètres), (arguments)) pour une
 * fonction void, H comme F pour les deux fonctions appelées très souvent
 * (active, idle_poll), que loci_backend.c aiguille à la main.
 * Même liste, dans le même ordre, que les #define de loci_be_rename.h : une
 * fonction ajoutée à loci_emu.h s'ajoute aux deux (sinon l'édition de liens
 * échoue : symbole en double ou manquant).
 */
#ifndef LOCI_BE_API_H
#define LOCI_BE_API_H

#include <stdbool.h>
#include <stdint.h>

#define LOCI_BE_API(F, V, H) \
    F(int, start, (const char *elf_path), (elf_path)) \
    V(set_usb_image, (const char *path), (path)) \
    V(set_flash_image, (const char *path), (path)) \
    V(stop, (void), ()) \
    H(bool, active, (void), ()) \
    F(bool, wait_boot, (void), ()) \
    F(bool, menu_button, (void), ()) \
    F(bool, button_was_warm, (void), ()) \
    F(bool, diag_button, (void), ()) \
    F(bool, rom_read, (uint16_t address, uint8_t *out), (address, out)) \
    F(bool, romdis, (void), ()) \
    F(bool, rom_write, (uint16_t address, uint8_t value), (address, value)) \
    V(ext_lines, (int *nirq, int *nreset, int *nromdis), (nirq, nreset, nromdis)) \
    V(api_write, (uint16_t address, uint8_t value), (address, value)) \
    F(uint8_t, api_read, (uint16_t address), (address)) \
    F(bool, io_page, (void), ()) \
    F(bool, io_read, (uint16_t address, uint8_t *out), (address, out)) \
    V(io_write, (uint16_t address, uint8_t value), (address, value)) \
    F(bool, read_lost, (void), ()) \
    V(dsk_write, (uint16_t address, uint8_t value), (address, value)) \
    F(uint8_t, dsk_read, (uint16_t address), (address)) \
    V(tap_write, (uint16_t address, uint8_t value), (address, value)) \
    F(uint8_t, tap_read, (uint16_t address), (address)) \
    V(tap_motor, (uint8_t via_orb), (via_orb)) \
    V(dsk_tick, (void), ()) \
    F(int, irq_take, (void), ()) \
    V(set_cdc_device, (const char *path), (path)) \
    F(bool, acia_active, (void), ()) \
    F(bool, acia_served, (uint16_t address), (address)) \
    V(acia_write, (uint16_t address, uint8_t value), (address, value)) \
    F(uint8_t, acia_read, (uint16_t address), (address)) \
    F(uint8_t, acia_peek, (uint16_t address), (address)) \
    V(acia_tick, (void), ()) \
    V(tick, (long steps), (steps)) \
    F(bool, mou_report, (uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan), (buttons, dx, dy, wheel, pan)) \
    F(bool, mou_armed, (void), ()) \
    F(bool, kbd_report, (uint8_t modifier, const uint8_t keycodes[6]), (modifier, keycodes)) \
    F(bool, kbd_armed, (void), ()) \
    F(const char *, backend_name, (void), ()) \
    F(int, reset_take, (void), ()) \
    H(int, idle_poll, (int cycles), (cycles))

#endif /* LOCI_BE_API_H */
