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

/** Co-simulation LOCI : recopie la ligne nIRQ du vrai firmware sur l'IRQ du CPU,
 *  après un accès servi par le firmware (DSK, io-page, ACIA $0380 : partagé avec
 *  la carte ACIA, src/cards/card_acia.c). */
void loci_emu_reflect_nirq(struct emulator_s* emu);

/** Avance d'un pas de `cycles` cycles CPU les périphériques de bus temporisés
 *  (FDC Microdisc/LOCI, ACIA, DTL2000, Mageco), dans l'ORDRE HISTORIQUE exact de
 *  `cpu_cycle_tick` (iso-comportement). Le VIA et la cassette (cœur/port) restent
 *  câblés dans main.c. Epic 7 / US5. */
void io_bus_tick(struct emulator_s* emu, int cycles);

#endif /* IO_BUS_H */
