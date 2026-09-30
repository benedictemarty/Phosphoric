/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cli_args.h
 * @brief Lecture de la ligne de commande dans un cli_opts_t.
 * @author bmarty <bmarty@mailo.com>
 */
#ifndef CLI_ARGS_H
#define CLI_ARGS_H

#include "cli/cli_opts.h"
#include "emulator.h"

/* Lit argv avec getopt_long et remplit cfg (et quelques champs de emu :
 * breakpoint, no_border, export_border, realtime).
 * Retour : -1 = continuer ; sinon code de sortie du programme (0 après
 * --help ou option inconnue, 1 sur argument invalide). */
int cli_parse_args(int argc, char* argv[], cli_opts_t* cfg, emulator_t* emu);

#endif /* CLI_ARGS_H */
