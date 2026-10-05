/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cli_opts.c
 * @brief Default values of the command-line options.
 * @author bmarty <bmarty@mailo.com>
 */
#include <string.h>
#include "cli/cli_opts.h"
#include "card_module.h"

void cli_opts_init(cli_opts_t* o) {
    memset(o, 0, sizeof(*o));
    o->max_cycles = -1;
    o->frame_dump_interval = 50;
    o->video_avi_fps = 50;
    o->video_avi_quality = 85;
    o->gdb_port = GDB_DEFAULT_PORT;
    o->scale_factor = 3;
    o->cpu_microseq = true;
    o->ula_per_cycle = true;
    o->loci_usb_autoscan = true;
    o->loci_mia_win_lo = -1;
    o->loci_mia_win_hi = -1;
    o->loci_serve_cycles = -1;
    o->loci_tdsr_ns = -1;
    o->loci_serve_jitter = -1;
    o->card_cfg = card_modules_cfg_new();
}
