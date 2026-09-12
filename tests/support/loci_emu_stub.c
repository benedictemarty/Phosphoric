/* SPDX-License-Identifier: EUPL-1.2 */
/* Stub du backend LOCI co-sim (--loci-emu) pour les suites de test qui linkent
 * memory.c sans avoir besoin du vrai firmware RP2040. memory.c appelle
 * loci_emu_active()/loci_emu_rom_read() pour l'overlay ROM ; ces stubs répondent
 * « inactif » afin d'éviter de tirer libemul.a (émulateur RP2040) dans chaque
 * binaire de test. Les suites qui testent réellement le co-sim linkent la vraie
 * src/io/loci_emu.c + libemul.a à la place de ce fichier. */
#include "io/loci_emu.h"

bool loci_emu_active(void) { return false; }
bool loci_emu_menu_button(void) { return false; }
bool loci_emu_diag_button(void) { return false; }

bool loci_emu_rom_read(uint16_t address, uint8_t *out)
{
    (void)address; (void)out;
    return false;
}

/* loci_core.c route les rapports HID vers le firmware quand la co-sim est
 * active ; inactive ici, ces stubs ne sont jamais atteints mais doivent
 * exister au link. */
bool loci_emu_mou_report(uint8_t buttons, int8_t dx, int8_t dy,
                         int8_t wheel, int8_t pan)
{
    (void)buttons; (void)dx; (void)dy; (void)wheel; (void)pan;
    return false;
}

bool loci_emu_kbd_report(uint8_t modifier, const uint8_t keycodes[6])
{
    (void)modifier; (void)keycodes;
    return false;
}
