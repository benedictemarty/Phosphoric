/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file rominfo.h
 * @brief ROM analysis tools — vector detection, subroutine map, string search
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.0.0-alpha
 *
 * Analyzes a loaded ORIC ROM to extract hardware vectors, map JSR/JMP
 * targets, detect ASCII strings, and compute region usage statistics.
 */

#ifndef ROMINFO_H
#define ROMINFO_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "memory/memory.h"

/** ROM base address in the ORIC memory map */
#define ROM_BASE_ADDR  0xC000

/** Maximum JSR/JMP targets to track */
#define ROMINFO_MAX_TARGETS  512

/** Maximum ASCII strings to detect */
#define ROMINFO_MAX_STRINGS  256

/** Minimum length for a detected ASCII string */
#define ROMINFO_MIN_STRING_LEN 4

/**
 * @brief Hardware vectors extracted from ROM
 */
typedef struct {
    uint16_t reset;   /**< RESET vector ($FFFC-$FFFD) */
    uint16_t irq;     /**< IRQ/BRK vector ($FFFE-$FFFF) */
    uint16_t nmi;     /**< NMI vector ($FFFA-$FFFB) */
} rom_vectors_t;

/**
 * @brief A subroutine/jump target found in ROM
 */
typedef struct {
    uint16_t address;     /**< Target address */
    uint16_t ref_count;   /**< Number of references (JSR/JMP to this address) */
    bool     is_jsr;      /**< At least one JSR reference (subroutine) */
    bool     is_jmp;      /**< At least one JMP reference */
} rom_target_t;

/**
 * @brief An ASCII string found in ROM
 */
typedef struct {
    uint16_t address;     /**< Start address in ROM */
    uint16_t length;      /**< String length */
} rom_string_t;

/**
 * @brief ROM region usage statistics
 */
typedef struct {
    uint16_t total_bytes;        /**< Total ROM size */
    uint16_t zero_bytes;         /**< Count of $00 bytes */
    uint16_t ff_bytes;           /**< Count of $FF bytes */
    uint16_t code_bytes;         /**< Estimated code bytes (valid opcodes) */
    uint16_t data_bytes;         /**< Estimated data bytes (non-code) */
} rom_usage_t;

/**
 * @brief Complete ROM analysis result
 */
typedef struct {
    rom_vectors_t vectors;

    rom_target_t  targets[ROMINFO_MAX_TARGETS];
    int           target_count;

    rom_string_t  strings[ROMINFO_MAX_STRINGS];
    int           string_count;

    rom_usage_t   usage;

    bool          valid;       /**< Analysis completed successfully */
} rom_analysis_t;

/**
 * @brief Initialize analysis result (zeroed)
 * @param analysis Pointer to analysis structure
 */
void rominfo_init(rom_analysis_t* analysis);

/**
 * @brief Analyze a ROM image
 *
 * Scans the ROM for vectors, JSR/JMP targets, ASCII strings,
 * and computes usage statistics.
 *
 * @param analysis Output structure
 * @param rom ROM data (16KB)
 * @param rom_size Size of ROM data in bytes
 * @return true on success
 */
bool rominfo_analyze(rom_analysis_t* analysis, const uint8_t* rom, size_t rom_size);

/**
 * @brief Write analysis report to FILE
 * @param analysis Pointer to analysis result
 * @param rom ROM data (for string extraction)
 * @param rom_size ROM size
 * @param fp Output file
 */
void rominfo_report(const rom_analysis_t* analysis, const uint8_t* rom,
                    size_t rom_size, FILE* fp);

/**
 * @brief Write analysis report to named file
 * @param analysis Pointer to analysis result
 * @param rom ROM data
 * @param rom_size ROM size
 * @param filename Output file path
 * @return true on success
 */
bool rominfo_report_to_file(const rom_analysis_t* analysis, const uint8_t* rom,
                            size_t rom_size, const char* filename);

/**
 * @brief Search for a byte pattern in ROM
 *
 * @param rom ROM data
 * @param rom_size ROM size
 * @param pattern Pattern to search
 * @param pattern_len Pattern length
 * @param results Array to store found addresses (ROM-relative)
 * @param max_results Maximum results to return
 * @return Number of matches found
 */
int rominfo_find_pattern(const uint8_t* rom, size_t rom_size,
                         const uint8_t* pattern, size_t pattern_len,
                         uint16_t* results, int max_results);

#endif /* ROMINFO_H */
