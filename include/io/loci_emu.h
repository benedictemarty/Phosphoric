/*
 * loci_emu.h — LOCI backend by EMULATION of the real RP2040 firmware.
 *
 * Alternative to the behavioural backend (loci_core.c): instead of re-implementing
 * each MIA opcode in C, the REAL LOCI firmware runs inside an RP2040
 * emulator (library ~/loci/emul, libemul.a). Enabled by --loci-emu ELF.
 *
 * Step 1 (here): ROM OVERLAY. After operational boot (simulated BTN_A), the
 * firmware disables the Oric's BASIC ROM (nROMDIS) and SERVES
 * $C000-$FFFF itself via its PIO+DMA read-serve. Phosphoric calls loci_emu_rom_read()
 * from memory_read() to let the REAL firmware provide the boot ROM →
 * the 6502 starts in the real LOCI menu (reset vector $FF4C).
 *
 * Step 2: co-simulated MIA API window $0300-$03FF (loci_emu_api_read/write) —
 * a write is a full bus dialogue (action SM + act_loop core1); a read
 * serves in O(1) the byte iopage[addr] that the read-serve DMA would serve.
 */
#ifndef LOCI_EMU_H
#define LOCI_EMU_H

#include <stdint.h>
#include <stdbool.h>

/* Instantiates the emulator, loads the .elf and starts a DEDICATED EMULATOR THREAD that
 * boots the firmware and then runs it in continuous FREE-RUN (a host core distinct
 * from Phosphoric's 6502). Returns immediately (zero wait at launch);
 * LOCI stays TRANSPARENT as long as the MENU button is not pressed. Returns 0 if OK.
 * loci_emu_active() becomes true when the boot is complete. Access to the emulator
 * is serialised by a mutex (the functions below are safe from the main
 * thread). */
int loci_emu_start(const char *elf_path);

/* Declares the FAT image served as the emulated USB disk (drive « 1: »). Call BEFORE
 * loci_emu_start (mounting happens right after the boot). */
void loci_emu_set_usb_image(const char *path);

/* PERSISTENT flash image (internal littlefs FS, drive 0:) — like the cartridge's NOR
 * flash, it survives from one session to the next. Default: <elf>.flash; « - »
 * = volatile (blank flash at every launch, as before). Call BEFORE
 * loci_emu_start. */
void loci_emu_set_flash_image(const char *path);

/* End of session: persists the flash (written pages) into the image above.
 * No-op if the backend is not active. Safe not to call it (the session's
 * FS is then lost). */
void loci_emu_stop(void);

/* True when the firmware has finished booting (idle). False during the background boot
 * → the host lets the Oric serve its own ROM. */
bool loci_emu_active(void);
/* If a background boot is in progress, waits for it (the 6502 touches the LOCI page:
 * the internal model is not served in its place). Returns loci_emu_active(). */
bool loci_emu_wait_boot(void);

/* Simulates pressing LOCI's physical MENU BUTTON: the firmware loads its boot
 * ROM, arms the service ($C000-$FFFF via nROMDIS) and drives nRESET. Returns
 * true if the service is armed. The caller must THEN reset the 6502
 * (cpu_reset) so that it restarts in the served LOCI menu. */
bool loci_emu_menu_button(void);
/* true if the last loci_emu_menu_button() was a HOT freeze: the firmware has
 * set its IRQ trap and pulsed nIRQ (delivered by the usual drain); the function
 * deliberately returned false (do not reset the 6502). */
bool loci_emu_button_was_warm(void);

/** @brief LONG press (≥ 2 s) on the MENU button: the firmware boots its embedded
 *  diagnostic ROM (test108k, EXT_BOOT_DIAG). Returns true if the ROM service
 *  is armed — the host must then reset its 6502. false if the firmware has
 *  no embedded diagnostic ROM (build without EMBEDDED_TEST108K_ROM). */
bool loci_emu_diag_button(void);

/* Co-sim ROM overlay: if the firmware serves this address (nROMDIS active +
 * $C000-$FFFF), writes the served byte into *out and returns true; otherwise returns
 * false (memory_read() then keeps the Oric ROM/RAM). Call BEFORE serving
 * the internal ROM in memory_read(). */
bool loci_emu_rom_read(uint16_t address, uint8_t *out);

/* true when the firmware asserts ROMDIS (internal BASIC ROM switched off). A high
 * address that loci_emu_rom_read does NOT serve while ROMDIS is active is under
 * MAP: the Oric's overlay RAM answers (Microdisc: $C000-$DFFF in RAM
 * while microdis.rom is served on $E000-$FFFF) — and writes go there. */
bool loci_emu_romdis(void);
/* 6502 write to $C000-$FFFF while LOCI serves the ROM: true if the backend
 * consumed it (mailbox in page $FF of the loci-fw firmware), false = overlay RAM. */
bool loci_emu_rom_write(uint16_t address, uint8_t value);

/* State of the control lines driven by the firmware (via the I²C expander):
 * returns 1 when the line is active. Lets the host mirror nROMDIS/
 * nRESET/nIRQ onto its 6502/ULA. */
void loci_emu_ext_lines(int *nirq, int *nreset, int *nromdis);

/* Co-simulated MIA API $03xx (step 2). Call when loci_emu_active(): the REAL
 * firmware handles the access (action SM pio0 sm1 + act_loop core1). A write is a
 * full bus dialogue; a read returns the served byte (O(1)). Until the firmware
 * has finished booting, write is ignored and read returns 0xFF (floating bus). */
void    loci_emu_api_write(uint16_t address, uint8_t value);
uint8_t loci_emu_api_read(uint16_t address);

/* WHOLE I/O page $0310-$03FF served by real bus cycles (neo backend: loci-fw,
 * batch 8.0, ADR-009 — this is what /IO CONTROL does on the board). io_page() = true if the
 * backend claims it; io_read() returns true if LOCI drove the bus (otherwise floating
 * bus); any nIRQs are drained by loci_emu_irq_take(). */
bool    loci_emu_io_page(void);
bool    loci_emu_io_read(uint16_t address, uint8_t *out);
void    loci_emu_io_write(uint16_t address, uint8_t value);

/* Co-simulated Microdisc $0310-$0314/$0318: the WD1793 controller emulated by the REAL
 * firmware (oric/dsk.c) serves the 6502 — sector read/write, seek, RNF,
 * end-of-command IRQ (drained as for the API). Until the boot is
 * complete: write ignored, read = 0xFF. The cassette window $0315-$0317 (TAP)
 * stays served by the internal model. */
void    loci_emu_dsk_write(uint16_t address, uint8_t value);
uint8_t loci_emu_dsk_read(uint16_t address);
/* Co-simulated cassette $0315-$0317 (oric/tap.c): the firmware's ROM patches
 * drive CMD/STAT/DATA; the motor (VIA ORB PB6, $0300) is snooped as on
 * the real bus — main.c calls loci_emu_tap_motor on each ORB write. */
void    loci_emu_tap_write(uint16_t address, uint8_t value);
uint8_t loci_emu_tap_read(uint16_t address);
void    loci_emu_tap_motor(uint8_t via_orb);

/* Once per frame: advances a WD command in progress without any 6502 access
 * (the Microdisc ROM waits for the end-of-RESTORE/SEEK IRQ without reading anything). Then
 * drain loci_emu_irq_take(). No-op until the boot is complete. */
void    loci_emu_dsk_tick(void);

/* Number of nIRQ pulses asserted by the firmware since the last call (reset to
 * zero). Call once per frame; deliver as many EDGE IRQs to the 6502. */
int loci_emu_irq_take(void);

/* ── ACIA $0380-$0383 served by the real firmware (Phase 2 CDC) ──────
 * When a CDC dongle is attached (--loci-cdc <dev>), the Oric's 6551 ACIA is
 * served by the REAL firmware (oric/acia.c ↔ USB modem) instead of the behavioural
 * 6551. `loci_emu_set_cdc_device` must be called BEFORE loci_emu_start
 * (attachment happens after the boot). `loci_emu_acia_active` = booted AND dongle attached.
 * `loci_emu_acia_tick` pumps the CDC↔registers exchange (asynchronous RX) — call once
 * per frame. */
void    loci_emu_set_cdc_device(const char *path);
bool    loci_emu_acia_active(void);
/* true if the co-simulated firmware SERVES this address as an ACIA register (window
 * $0380-$0383 in mode 1, $0340 in mode 2), modem mounted or not — on a real Oric
 * LOCI answers there from boot ($0381 = $70 without modem), never the VIA. */
bool    loci_emu_acia_served(uint16_t address);
void    loci_emu_acia_write(uint16_t address, uint8_t value);
uint8_t loci_emu_acia_read(uint16_t address);
uint8_t loci_emu_acia_peek(uint16_t address);   /* non-destructive observation */
void    loci_emu_acia_tick(void);

/* BOUNDED free-run (deterministic, main thread): advances the firmware by `steps`
 * RP2040 steps WITHOUT driving the bus (Phi2 held HIGH → the action SM stays parked),
 * so that background tasks progress and ASYNCHRONOUS nIRQ pulses
 * (timers, button trap) arise between two transactions. Call once per
 * frame when loci_emu_active(), BEFORE loci_emu_irq_take(). No-op until the boot
 * is complete. */
void loci_emu_tick(long steps);

/* USB HID mouse (co-sim). The co-simulated firmware does not enumerate USB: without
 * this bridge, the host mouse never reaches `mou_report()` and the 6502 reads
 * an empty `mou_xram` (the internal model's `loci_mou_report()` path writes
 * into an xram the 6502 no longer reads once loci_emu_active()). Deltas
 * accumulated on the firmware side, exactly like a real HID report.
 * No-op if the boot is not complete. Returns true if the report was applied. */
bool loci_emu_mou_report(uint8_t buttons, int8_t dx, int8_t dy,
                         int8_t wheel, int8_t pan);

/* true if the 6502 has armed the mouse via XREG (otherwise the firmware writes nothing). */
bool loci_emu_mou_armed(void);

/* USB HID keyboard (co-sim). Calls the firmware's REAL kbd_report() — it does
 * far more than fill a bitmap (layouts, stdio queue, repeat, LED), replicating
 * it would be fragile. `keycodes` = 6 HID usages (0 = empty). */
bool loci_emu_kbd_report(uint8_t modifier, const uint8_t keycodes[6]);
bool loci_emu_kbd_armed(void);
/* ── REAL HARDWARE backend (loci_hw.c, `make LOCI_HW=1`, --loci-hw DEV) ──
 * Name of the compiled backend: "emul" (loci_emu.c), "stub" (loci_emu_stub.c) or "hw"
 * (loci_hw.c, source ~/loci/loci-usb/phosphoric/). main.c rejects --loci-hw elsewhere. */
const char *loci_emu_backend_name(void);
/* nRESET edges driven by LOCI (physical MENU button, freeze) since the last call:
 * the host then resets its 6502 (once per frame, after loci_emu_irq_take).
 * Always 0 outside the hardware backend (in co-sim the button is simulated by the host). */
int loci_emu_reset_take(void);
/* « Idle » poll (hardware backend): called after EACH instruction with its
 * cycles; the backend accumulates and, with no LOCI access for N cycles, queries the
 * cartridge. Returns the number of nIRQ pulses to deliver (0 most of the time),
 * or -1 if only a reset occurred; the caller checks loci_emu_reset_take when
 * the value is non-zero. Always 0 outside the
 * hardware backend. Must stay nearly free: a counter. */
int loci_emu_idle_poll(int cycles);

#endif /* LOCI_EMU_H */
