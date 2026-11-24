/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file savestate.h
 * @brief Phosphoric save state API
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.4.0-alpha
 *
 * Save/restore complete emulator state to/from .ost files
 * (Oric Save sTate format).
 */

#ifndef SAVESTATE_H
#define SAVESTATE_H

#include <stdint.h>
#include <stdbool.h>

#include "io/io_device.h"   /* io_device_t: per-device serialization hooks */

#define SAVESTATE_MAGIC    "OST1"
#define SAVESTATE_VERSION  1
#define SAVESTATE_EXT      ".ost"
#define SAVESTATE_HEADER_SIZE 48

typedef struct emulator_s emulator_t;

/**
 * @brief Registers the bus device table for serialization.
 *
 * Called once at initialization (by main.c). savestate_save/load then iterate
 * over this table and call the save/load hooks of the devices that provide
 * them (save_tag != NULL) -- without savestate.c knowing about io_bus.
 * `devices` must remain valid for the whole lifetime of the emulator
 * (the io_bus table is static). count <= 0 or devices NULL = no hook.
 */
void savestate_set_io_devices(const io_device_t* devices, int count);

/**
 * @brief Save emulator state to file
 *
 * Serializes CPU, memory, VIA, PSG, video, keyboard, FDC, Microdisc,
 * and tape state into a binary .ost file with CRC32 integrity check.
 *
 * @param emu Pointer to emulator structure
 * @param filename Path to output file
 * @return true on success, false on failure
 */
bool savestate_save(const emulator_t* emu, const char* filename);

/**
 * @brief Load emulator state from file
 *
 * Deserializes hardware state from a .ost file. The emulator must
 * already be initialized (callbacks, ROM loaded). Only hardware state
 * (CPU, RAM, VIA, PSG, etc.) is overwritten. Internal pointers are
 * recabled after load.
 *
 * @param emu Pointer to emulator structure
 * @param filename Path to input file
 * @return true on success, false on failure
 */
bool savestate_load(emulator_t* emu, const char* filename);

#endif /* SAVESTATE_H */
