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

#include "io/io_device.h"   /* io_device_t : hooks de sérialisation par device */

#define SAVESTATE_MAGIC    "OST1"
#define SAVESTATE_VERSION  1
#define SAVESTATE_EXT      ".ost"
#define SAVESTATE_HEADER_SIZE 48

typedef struct emulator_s emulator_t;

/**
 * @brief Enregistre la table des périphériques de bus pour la sérialisation.
 *
 * Appelé une fois à l'initialisation (par main.c). savestate_save/load itèrent
 * ensuite cette table et appellent les hooks save/load des devices qui en
 * fournissent (save_tag != NULL) — sans que savestate.c connaisse io_bus.
 * `devices` doit rester valide pendant toute la durée de vie de l'émulateur
 * (la table io_bus est statique). count <= 0 ou devices NULL = aucun hook.
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
