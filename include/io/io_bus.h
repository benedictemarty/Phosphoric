/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file io_bus.h
 * @brief I/O bus adapter: device table + dispatch.
 * @author bmarty <bmarty@mailo.com>
 *
 * Extracted from main.c (Epic 7 / US2, Sprint 126). This module is the **adaptation
 * layer** between the generic `io_device_t` contract and the concrete device
 * modules + the `emulator_t` context. It is allowed to know
 * `emulator_t` (claims are cross-linked: the ACIA at $0380 consults the LOCI, the
 * Microdisc synchronises the overlay memory). The pure modules (acia6551.c,
 * microdisc.c, …) remain decoupled from `emulator.h`.
 */
#ifndef IO_BUS_H
#define IO_BUS_H

#include <stdint.h>
#include "io/io_device.h"

struct emulator_s;

/** Returns the device that owns `addr` for **reading** (NULL otherwise).
 *  Table order = priority (LOCI first, ULA-NG last). */
const io_device_t* io_bus_find(struct emulator_s* emu, uint16_t addr);

/** Returns the device that owns `addr` for **writing** (claims_write if provided,
 *  otherwise claims). NULL if none. */
const io_device_t* io_bus_find_write(struct emulator_s* emu, uint16_t addr);

/** Exposes the table for savestate registration (`savestate_set_io_devices`).
 *  @param count  receives the number of entries. */
const io_device_t* io_bus_devices(int* count);

/** LOCI co-simulation: copies the real firmware's nIRQ line onto the CPU IRQ,
 *  after an access served by the firmware (DSK, io-page, ACIA $0380: shared with
 *  the ACIA card, src/cards/card_acia.c). */
void loci_emu_reflect_nirq(struct emulator_s* emu);

/** Advances the timed bus devices by one step of `cycles` CPU cycles
 *  (Microdisc/LOCI FDC, ACIA, DTL2000, Mageco), in the exact HISTORICAL ORDER of
 *  `cpu_cycle_tick` (same behaviour). The VIA and the cassette (core/port) stay
 *  wired in main.c. Epic 7 / US5. */
void io_bus_tick(struct emulator_s* emu, int cycles);

#endif /* IO_BUS_H */
