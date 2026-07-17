/**
 * @file io_bus.h
 * @brief Adaptateur bus I/O : table des périphériques + dispatch.
 * @author bmarty <bmarty@mailo.com>
 *
 * Extrait de main.c (Epic 7 / US2, Sprint 126). Ce module est la **couche
 * d'adaptation** entre le contrat générique `io_device_t` et les modules de
 * périphériques concrets + le contexte `emulator_t`. Il a le droit de connaître
 * `emulator_t` (les claims sont croisés : l'ACIA à $0380 consulte le LOCI, le
 * Microdisc synchronise la mémoire d'overlay). Les modules purs (acia6551.c,
 * microdisc.c, …) restent, eux, découplés d'`emulator.h`.
 */
#ifndef IO_BUS_H
#define IO_BUS_H

#include <stdint.h>
#include "io/io_device.h"

struct emulator_s;

/** Renvoie le device qui possède `addr` en **lecture** (NULL sinon).
 *  Ordre de la table = priorité (LOCI en tête, ULA-NG en dernier). */
const io_device_t* io_bus_find(struct emulator_s* emu, uint16_t addr);

/** Renvoie le device qui possède `addr` en **écriture** (claims_write si fourni,
 *  sinon claims). NULL si aucun. */
const io_device_t* io_bus_find_write(struct emulator_s* emu, uint16_t addr);

/** Expose la table pour l'enregistrement savestate (`savestate_set_io_devices`).
 *  @param count  reçoit le nombre d'entrées. */
const io_device_t* io_bus_devices(int* count);

#endif /* IO_BUS_H */
