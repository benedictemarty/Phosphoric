/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file appsignal.h
 * @brief Handling of the process termination signals (SIGINT/SIGTERM).
 * @author bmarty <bmarty@mailo.com>
 *
 * Extracted from main.c (Epic 7 / US1, Sprint 125). Encapsulates the global
 * "should the process keep running?" flag and the handler installation.
 */
#ifndef APPSIGNAL_H
#define APPSIGNAL_H

#include <stdbool.h>

/** Installs the SIGINT/SIGTERM handlers (they set the flag to false). */
void app_install_signal_handlers(void);

/** True as long as no termination signal has been received. */
bool app_should_run(void);

#endif /* APPSIGNAL_H */
