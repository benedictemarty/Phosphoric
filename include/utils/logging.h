/**
 * @file logging.h
 * @brief Logging system for Phosphoric
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-01-31
 * @version 0.1.0-alpha
 */

#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>

/**
 * @brief Log levels
 */
typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR
} log_level_t;

/**
 * @brief Initialize logging system
 *
 * @param level Minimum log level
 */
void log_init(log_level_t level);

/* Sprint 35a — redirect log output to a custom FILE* (e.g. stderr for
 * IPC control mode). Pass NULL to revert to default (stdout). */
void log_set_stream(FILE* fp);

/**
 * @brief Cleanup logging system
 */
void log_cleanup(void);

/**
 * @brief Log debug message
 */
void log_debug(const char* format, ...);

/**
 * @brief Log info message
 */
void log_info(const char* format, ...);

/**
 * @brief Log warning message
 */
void log_warning(const char* format, ...);

/**
 * @brief Log error message
 */
void log_error(const char* format, ...);

#endif /* LOGGING_H */
