/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file bas2tap.c
 * @brief BASIC to TAP converter
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-01-31
 * @version 0.1.0-alpha
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/tap.h"
#include "utils/logging.h"

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <input.bas> -o <output.tap> [--auto-run]\n", argv[0]);
        return 1;
    }

    const char* input_file = argv[1];
    const char* output_file = NULL;
    bool auto_run = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--auto-run") == 0) {
            auto_run = true;
        }
    }

    if (!output_file) {
        fprintf(stderr, "Error: Output file not specified\n");
        return 1;
    }

    printf("Converting BASIC to TAP...\n");
    printf("Input:  %s\n", input_file);
    printf("Output: %s\n", output_file);
    printf("Auto-run: %s\n", auto_run ? "yes" : "no");

    if (tap_from_basic(input_file, output_file, auto_run)) {
        printf("Conversion successful!\n");
        return 0;
    } else {
        fprintf(stderr, "Conversion failed (not yet implemented)\n");
        return 1;
    }
}
