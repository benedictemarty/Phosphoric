/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file loci_be_rename.h
 * @brief Renomme l'interface loci_emu_* d'un backend LOCI en LOCI_BE_<nom>
 * @author bmarty <bmarty@mailo.com>
 *
 * Inclus d'office (-include) par le Makefile dans loci_emu.c / loci_neo.c,
 * loci_hw.c et loci_emu_stub.c, compilés avec -DLOCI_BE=lbe_emul, lbe_hw ou
 * lbe_stub : les backends gardent leur source inchangée (loci_hw.c reste la
 * copie de ~/loci/loci-usb/phosphoric/) et cohabitent dans un même binaire ;
 * loci_backend.c définit les vraies fonctions loci_emu_* et aiguille vers le
 * backend choisi au lancement. Liste : celle de loci_be_api.h.
 */
#ifndef LOCI_BE_RENAME_H
#define LOCI_BE_RENAME_H

#ifndef LOCI_BE
#error "loci_be_rename.h : compiler avec -DLOCI_BE=<préfixe>"
#endif
#define LOCI_BE_CAT2(a, b) a##b
#define LOCI_BE_CAT(a, b) LOCI_BE_CAT2(a, b)
#define LOCI_BE_NAME(n) LOCI_BE_CAT(LOCI_BE, LOCI_BE_CAT2(_, n))

#define loci_emu_start LOCI_BE_NAME(start)
#define loci_emu_set_usb_image LOCI_BE_NAME(set_usb_image)
#define loci_emu_set_flash_image LOCI_BE_NAME(set_flash_image)
#define loci_emu_stop LOCI_BE_NAME(stop)
#define loci_emu_active LOCI_BE_NAME(active)
#define loci_emu_wait_boot LOCI_BE_NAME(wait_boot)
#define loci_emu_menu_button LOCI_BE_NAME(menu_button)
#define loci_emu_button_was_warm LOCI_BE_NAME(button_was_warm)
#define loci_emu_diag_button LOCI_BE_NAME(diag_button)
#define loci_emu_rom_read LOCI_BE_NAME(rom_read)
#define loci_emu_romdis LOCI_BE_NAME(romdis)
#define loci_emu_rom_write LOCI_BE_NAME(rom_write)
#define loci_emu_ext_lines LOCI_BE_NAME(ext_lines)
#define loci_emu_api_write LOCI_BE_NAME(api_write)
#define loci_emu_api_read LOCI_BE_NAME(api_read)
#define loci_emu_io_page LOCI_BE_NAME(io_page)
#define loci_emu_io_read LOCI_BE_NAME(io_read)
#define loci_emu_io_write LOCI_BE_NAME(io_write)
#define loci_emu_read_lost LOCI_BE_NAME(read_lost)
#define loci_emu_dsk_write LOCI_BE_NAME(dsk_write)
#define loci_emu_dsk_read LOCI_BE_NAME(dsk_read)
#define loci_emu_tap_write LOCI_BE_NAME(tap_write)
#define loci_emu_tap_read LOCI_BE_NAME(tap_read)
#define loci_emu_tap_motor LOCI_BE_NAME(tap_motor)
#define loci_emu_dsk_tick LOCI_BE_NAME(dsk_tick)
#define loci_emu_irq_take LOCI_BE_NAME(irq_take)
#define loci_emu_set_cdc_device LOCI_BE_NAME(set_cdc_device)
#define loci_emu_acia_active LOCI_BE_NAME(acia_active)
#define loci_emu_acia_served LOCI_BE_NAME(acia_served)
#define loci_emu_acia_write LOCI_BE_NAME(acia_write)
#define loci_emu_acia_read LOCI_BE_NAME(acia_read)
#define loci_emu_acia_peek LOCI_BE_NAME(acia_peek)
#define loci_emu_acia_tick LOCI_BE_NAME(acia_tick)
#define loci_emu_tick LOCI_BE_NAME(tick)
#define loci_emu_mou_report LOCI_BE_NAME(mou_report)
#define loci_emu_mou_armed LOCI_BE_NAME(mou_armed)
#define loci_emu_kbd_report LOCI_BE_NAME(kbd_report)
#define loci_emu_kbd_armed LOCI_BE_NAME(kbd_armed)
#define loci_emu_backend_name LOCI_BE_NAME(backend_name)
#define loci_emu_reset_take LOCI_BE_NAME(reset_take)
#define loci_emu_idle_poll LOCI_BE_NAME(idle_poll)

#endif /* LOCI_BE_RENAME_H */
