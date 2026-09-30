/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cli_opts.c
 * @brief Valeurs par défaut des options de la ligne de commande.
 * @author bmarty <bmarty@mailo.com>
 */
#include <string.h>
#include "cli/cli_opts.h"

void cli_opts_init(cli_opts_t* o) {
    memset(o, 0, sizeof(*o));
    o->max_cycles = -1;
    o->frame_dump_interval = 50;
    o->video_avi_fps = 50;
    o->video_avi_quality = 85;
    o->gdb_port = GDB_DEFAULT_PORT;
    o->sp0256_base_addr = SP0256_BASE_DEFAULT;
    o->mea8000_base_addr = MEA8000_BASE_DEFAULT;
    o->scale_factor = 3;
    o->cpu_microseq = true;
    o->ula_per_cycle = true;
    o->loci_usb_autoscan = true;
    o->loci_mia_win_lo = -1;
    o->loci_mia_win_hi = -1;
    o->loci_serve_subticks = -1;
    o->loci_latch_subtick = -1;
    o->loci_serve_jitter = -1;
}
