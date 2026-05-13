/**
 * @file bin2tap.c
 * @brief Binary to TAP converter
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-01-31
 * @version 0.1.0-alpha
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "storage/tap.h"
#include "utils/logging.h"

int main(int argc, char* argv[]) {
    if (argc < 5) {
        printf("Usage: %s <input.bin> --start <addr> --exec <addr> -o <output.tap> [--name <name>] [--no-autorun]\n", argv[0]);
        printf("  --no-autorun  Set TAP auto-run flag to $00 (override --exec).\n");
        printf("                Useful for binaries sensitive to ROM init timing\n");
        printf("                under -f fast-load; user must CALL/USR manually.\n");
        return 1;
    }

    const char* input_file = argv[1];
    const char* output_file = NULL;
    const char* name = "PROGRAM";
    uint16_t start_addr = 0;
    uint16_t exec_addr = 0;
    bool no_autorun = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
            start_addr = (uint16_t)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--exec") == 0 && i + 1 < argc) {
            exec_addr = (uint16_t)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--no-autorun") == 0) {
            no_autorun = true;
        }
    }

    if (no_autorun) exec_addr = 0;  /* tap_from_binary uses exec_addr==0 → auto-run $00 */

    if (!output_file) {
        fprintf(stderr, "Error: Output file not specified\n");
        return 1;
    }

    printf("Converting binary to TAP...\n");
    printf("Input:       %s\n", input_file);
    printf("Output:      %s\n", output_file);
    printf("Start addr:  0x%04X\n", start_addr);
    printf("Exec addr:   0x%04X\n", exec_addr);
    printf("Name:        %s\n", name);

    if (tap_from_binary(input_file, output_file, start_addr, exec_addr, name)) {
        printf("Conversion successful!\n");
        return 0;
    } else {
        fprintf(stderr, "Conversion failed (not yet implemented)\n");
        return 1;
    }
}
