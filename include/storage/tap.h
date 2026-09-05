/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file tap.h
 * @brief ORIC .TAP tape format support
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-01-31
 * @version 0.1.0-alpha
 *
 * The .TAP format stores ORIC tape data in a file.
 * Format structure:
 * - Sync bytes ($16 x N)
 * - Header block: $24, name, type, start, end, checksum
 * - Sync bytes ($16 x N)
 * - Data block: $24, data, checksum
 */

#ifndef TAP_H
#define TAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define TAP_SYNC_BYTE   0x16  /**< Sync byte */
#define TAP_MARKER      0x24  /**< Block marker */
#define TAP_NAME_LEN    16    /**< Program name length */

/**
 * @brief Program types
 */
typedef enum {
    TAP_BASIC = 0x00,       /**< BASIC program */
    TAP_MACHINE = 0x80,     /**< Machine code */
    TAP_SCREEN = 0xC0       /**< Screen data */
} tap_program_type_t;

/**
 * @brief TAP file header
 */
typedef struct tap_header_s {
    uint8_t  sync_len;              /**< Number of sync bytes */
    char     name[TAP_NAME_LEN];    /**< Program name */
    uint8_t  type;                  /**< Program type */
    uint8_t  auto_run;              /**< Auto-run flag */
    uint16_t start_addr;            /**< Start address */
    uint16_t end_addr;              /**< End address */
    uint8_t  checksum;              /**< Header checksum */
} tap_header_t;

/**
 * @brief TAP file context
 */
typedef struct tap_file_s {
    FILE*    file;          /**< File handle */
    bool     writing;       /**< Write mode flag */
    uint32_t position;      /**< Current position in tape */
    uint32_t size;          /**< Total tape size */

    /* Fast load support */
    bool     fast_load;     /**< Fast load enabled */
    uint8_t* data;          /**< Complete tape data (for fast load) */
} tap_file_t;

/**
 * @brief Open TAP file for reading
 *
 * @param filename Path to .TAP file
 * @param fast_load Enable fast load (loads entire file to memory)
 * @return TAP file context or NULL on error
 */
tap_file_t* tap_open_read(const char* filename, bool fast_load);

/**
 * @brief Open TAP file for writing
 *
 * @param filename Path to .TAP file
 * @return TAP file context or NULL on error
 */
tap_file_t* tap_open_write(const char* filename);

/**
 * @brief Close TAP file
 *
 * @param tap TAP file context
 */
void tap_close(tap_file_t* tap);

/**
 * @brief Read header from TAP file
 *
 * @param tap TAP file context
 * @param header Output header structure
 * @return true on success, false on error
 */
bool tap_read_header(tap_file_t* tap, tap_header_t* header);

/**
 * @brief Write header to TAP file
 *
 * @param tap TAP file context
 * @param header Header structure to write
 * @return true on success, false on error
 */
bool tap_write_header(tap_file_t* tap, const tap_header_t* header);

/**
 * @brief Read data block from TAP file
 *
 * @param tap TAP file context
 * @param buffer Output buffer
 * @param size Size to read
 * @return Number of bytes read, or -1 on error
 */
int tap_read_data(tap_file_t* tap, uint8_t* buffer, size_t size);

/**
 * @brief Write data block to TAP file
 *
 * @param tap TAP file context
 * @param buffer Data buffer
 * @param size Size to write
 * @return true on success, false on error
 */
bool tap_write_data(tap_file_t* tap, const uint8_t* buffer, size_t size);

/**
 * @brief Rewind TAP file to beginning
 *
 * @param tap TAP file context
 */
void tap_rewind(tap_file_t* tap);

/**
 * @brief Get current position in TAP file
 *
 * @param tap TAP file context
 * @return Position in bytes
 */
uint32_t tap_tell(const tap_file_t* tap);

/**
 * @brief Get total size of TAP file
 *
 * @param tap TAP file context
 * @return Size in bytes
 */
uint32_t tap_size(const tap_file_t* tap);

/**
 * @brief Check if at end of TAP file
 *
 * @param tap TAP file context
 * @return true if at end, false otherwise
 */
bool tap_eof(const tap_file_t* tap);

/**
 * @brief Calculate checksum for data block
 *
 * @param data Data buffer
 * @param size Size of data
 * @return Checksum byte
 */
uint8_t tap_checksum(const uint8_t* data, size_t size);

/**
 * @brief Convert BASIC text file to TAP format
 *
 * @param basic_file Path to BASIC text file
 * @param tap_file Path to output .TAP file
 * @param auto_run Enable auto-run
 * @return true on success, false on error
 */
bool tap_from_basic(const char* basic_file, const char* tap_file, bool auto_run);

/**
 * @brief Convert binary file to TAP format
 *
 * @param bin_file Path to binary file
 * @param tap_file Path to output .TAP file
 * @param start_addr Load address
 * @param exec_addr Execution address (0 = no auto-run)
 * @param name Program name (max 16 chars)
 * @return true on success, false on error
 */
bool tap_from_binary(const char* bin_file, const char* tap_file,
                    uint16_t start_addr, uint16_t exec_addr, const char* name);

#endif /* TAP_H */
