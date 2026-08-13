/**
 * @file cli_usage.h
 * @brief CLI --help / usage text (extracted from main.c, Epic 7/US3).
 * @author bmarty <bmarty@mailo.com>
 */
#ifndef CLI_USAGE_H
#define CLI_USAGE_H

/* Print the full --help / usage banner (moved verbatim out of main.c to keep
 * the god-object from growing; the text is byte-identical). */
void cli_print_usage(const char* program_name);

#endif /* CLI_USAGE_H */
