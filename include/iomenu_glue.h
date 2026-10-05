/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file iomenu_glue.h
 * @brief Glue between the peripherals menu (F1) and the emulator.
 * @author bmarty <bmarty@mailo.com>
 *
 * - Media operations, shared by the F1 menu and the F6 picker:
 *   insert / eject a floppy or a cassette.
 * - iomenu_refresh(): fills the displayed state from emulator_t.
 * - iomenu_apply(): executes an action returned by the menu.
 * - phosphoric.cfg: saving (menu button) and reloading at startup.
 */
#ifndef IOMENU_GLUE_H
#define IOMENU_GLUE_H

#include <stdbool.h>
#include <stddef.h>
#include "emulator.h"
#include "cli/cli_opts.h"
#include "video/iomenu.h"

typedef enum {
    MEDIA_OK = 0,
    MEDIA_NO_IFACE,        /* no disk interface (Microdisc / Jasmin) */
    MEDIA_LOCI_MENU,       /* co-simulated/real LOCI: floppies mounted from its own menu */
    MEDIA_BAD_DRIVE,       /* drive outside the interface */
    MEDIA_EMPTY,           /* nothing to eject */
    MEDIA_LOAD_FAILED      /* unreadable / invalid file */
} media_result_t;

/* Rewrites the drive image if it was modified and --disk-writeback is active. */
bool           media_disk_writeback(emulator_t* emu, int drv);
media_result_t media_disk_insert(emulator_t* emu, int drv, const char* path);
media_result_t media_disk_eject(emulator_t* emu, int drv);
media_result_t media_tape_insert(emulator_t* emu, const char* path);
media_result_t media_tape_eject(emulator_t* emu);

void iomenu_refresh(emulator_t* emu);
/* Executes the action; returns true if the menu must close. */
bool iomenu_apply(emulator_t* emu, const iom_action_t* act);

/* Default configuration file (current directory). */
#define IOMENU_CONFIG_DEFAULT "phosphoric.cfg"

/* Saves the settings managed by the menu to `path`, keeping the file's other
 * lines (comments, unknown keys). */
bool iomenu_config_save(emulator_t* emu, const char* path);
/* Reads `path` back and completes `cfg`: an option already given on the command
 * line takes priority. Returns the number of settings applied, -1 if the file
 * does not exist. */
int  iomenu_config_load(const char* path, cli_opts_t* cfg);

/* Renders the menu (current emulator state) to a 640 × 640 PPM file. */
bool iomenu_screenshot(emulator_t* emu, const char* path);

#endif /* IOMENU_GLUE_H */
