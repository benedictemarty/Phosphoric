/* SPDX-License-Identifier: EUPL-1.2 */
/* Stub du backend LOCI co-sim (--loci-emu) pour les suites de test qui linkent
 * memory.c sans avoir besoin du vrai firmware RP2040. memory.c appelle
 * loci_emu_active()/loci_emu_rom_read() pour l'overlay ROM ; ces stubs répondent
 * « inactif » afin d'éviter de tirer libemul.a (émulateur RP2040) dans chaque
 * binaire de test. Les suites qui testent réellement le co-sim linkent la vraie
 * src/io/loci_emu.c + libemul.a à la place de ce fichier. */
#include "io/loci_emu.h"

bool loci_emu_active(void) { return false; }

bool loci_emu_rom_read(uint16_t address, uint8_t *out)
{
    (void)address; (void)out;
    return false;
}
