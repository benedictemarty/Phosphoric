/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cli_args.h
 * @brief Reading the command line into a cli_opts_t.
 * @author bmarty <bmarty@mailo.com>
 */
#ifndef CLI_ARGS_H
#define CLI_ARGS_H

#include "cli/cli_opts.h"
#include "emulator.h"

/* Reads argv with getopt_long and fills cfg (and a few fields of emu:
 * breakpoint, no_border, export_border, realtime).
 * Returns -1 = continue; otherwise the program's exit code (0 after --help
 * or an unknown option, 1 on an invalid argument). */
int cli_parse_args(int argc, char* argv[], cli_opts_t* cfg, emulator_t* emu);

#endif /* CLI_ARGS_H */
