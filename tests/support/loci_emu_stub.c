/* SPDX-License-Identifier: EUPL-1.2 */
/* Stub of the LOCI co-sim backend (--loci-emu) for the test suites that link
 * memory.c without needing the real RP2040 firmware. memory.c calls
 * loci_emu_active()/loci_emu_rom_read() for the ROM overlay; these stubs answer
 * "inactive" so as to avoid pulling libemul.a (RP2040 emulator) into every
 * test binary. The suites that actually test the co-sim link the real
 * src/io/loci_emu.c + libemul.a instead of this file. */
#include "io/loci_emu.h"

bool loci_emu_active(void) { return false; }
const char *loci_emu_backend_name(void) { return "stub"; }
int loci_emu_reset_take(void) { return 0; }
int loci_emu_idle_poll(int cycles) { (void)cycles; return 0; }
bool loci_emu_menu_button(void) { return false; }
bool loci_emu_button_was_warm(void) { return false; }
bool loci_emu_wait_boot(void) { return false; }
bool loci_emu_diag_button(void) { return false; }

bool loci_emu_rom_read(uint16_t address, uint8_t *out)
{
    (void)address; (void)out;
    return false;
}

/* loci_core.c routes the HID reports to the firmware when the co-sim is
 * active; inactive here, these stubs are never reached but must
 * exist at link time. */
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

/* memory.c: RAM overlay under MAP in co-sim (inactive here → never reached). */
bool loci_emu_romdis(void) { return false; }
void loci_emu_tap_motor(uint8_t via_orb) { (void)via_orb; }

bool loci_emu_rom_write(uint16_t address, uint8_t value) { (void)address; (void)value; return false; }
bool loci_emu_select(const char *backend) { (void)backend; return false; }
bool loci_emu_backend_available(const char *backend) { (void)backend; return false; }
