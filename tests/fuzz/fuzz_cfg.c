/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_cfg.c
 * @brief Fuzzing: phosphoric.cfg → iomenu_config_load
 * @author bmarty <bmarty@mailo.com>
 */
#include "fuzz_common.h"
#include "utils/logging.h"
#include "cli/cli_opts.h"
#include "iomenu_glue.h"

FUZZ_NO_SEED()

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FUZZ_QUIET_LOGS();
    static cli_opts_t cfg;
    cli_opts_init(&cfg);
    (void)iomenu_config_load(fuzz_tmpfile(data, size), &cfg);
    /* Duplicated strings (program lifetime in real use). printer_file may
     * point to a constant ("impression.txt"): never freed here, hence
     * -detect_leaks=0 and ASAN_OPTIONS=detect_leaks=0 for this target
     * (Makefile, fuzz target). */
    for (int i = 0; i < MICRODISC_MAX_DRIVES; i++) free((void*)cfg.disk_files[i]);
    free((void*)cfg.tape_file);
    free((void*)cfg.disk_rom_file);
    free((void*)cfg.jasmin_rom_file);
    return 0;
}
