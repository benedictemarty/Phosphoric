/**
 * @file memory.h
 * @brief ORIC-1 Memory management (64KB addressable)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-01-31
 * @version 0.1.0-alpha
 *
 * ORIC-1 Memory Map:
 * $0000-$00FF: Zero Page
 * $0100-$01FF: Stack
 * $0200-$02FF: System variables
 * $0300-$030F: VIA 6522 (mirrored in $0300-$03FF)
 * $0400-$BFFF: User RAM / Screen RAM ($BB80-$BFDF)
 * $C000-$F7FF: BASIC ROM (or RAM overlay)
 * $F800-$FFFF: Monitor ROM (always ROM for vectors)
 */

#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stdbool.h>

#define MEMORY_SIZE 65536  /**< Total addressable memory (64KB) */
#define RAM_SIZE    49152  /**< User RAM size (48KB) */
#define ROM_SIZE    16384  /**< ROM size (16KB) */

/* OCULA memory banking (étape 4): the $A000-$BFFF window (8KB) can be
 * switched between bank 0 (main RAM, always scanned by the ULA) and 7
 * side banks held in the OCULA's internal SRAM. CPU-visible only. */
#define OCULA_BANK_COUNT 8       /**< bank 0 = main RAM + 7 side banks */
#define OCULA_BANK_SIZE  0x2000  /**< $A000-$BFFF */
#define OCULA_BANK_BASE  0xA000

/**
 * @brief Memory access types (for debugging/tracing)
 */
typedef enum {
    MEM_READ,
    MEM_WRITE,
    MEM_EXEC
} mem_access_type_t;

/**
 * @brief Memory banking mode
 */
typedef enum {
    BANK_ROM,   /**< ROM mapped */
    BANK_RAM    /**< RAM overlay mapped */
} memory_bank_t;

/**
 * @brief Memory subsystem structure
 */
typedef struct memory_s {
    uint8_t ram[RAM_SIZE];      /**< Main RAM */
    uint8_t rom[ROM_SIZE];      /**< ROM (BASIC + Monitor) */
    uint8_t charset[2048];      /**< Character set ROM */
    uint8_t upper_ram[ROM_SIZE]; /**< RAM behind ROM area ($C000-$FFFF) for Microdisc overlay */

    memory_bank_t charset_bank; /**< Character set banking */
    bool rom_enabled;           /**< ROM enable flag */

    /* Microdisc overlay ROM support */
    uint8_t* overlay_rom;         /**< Overlay ROM data (microdis.rom) */
    uint32_t overlay_rom_size;    /**< Overlay ROM size in bytes */
    bool overlay_active;          /**< Overlay ROM mapped at $E000-$FFFF */
    bool basic_rom_disabled;      /**< BASIC ROM disabled (romdis) */

    /* I/O device callbacks */
    uint8_t (*io_read)(uint16_t address, void* userdata);
    void (*io_write)(uint16_t address, uint8_t value, void* userdata);
    void* io_userdata;

    /* Memory access tracing (for debugging) */
    bool trace_enabled;
    void (*trace_callback)(uint16_t address, uint8_t value, mem_access_type_t type);

    /* OCULA banking ($A000-$BFFF): bank 0 = ram[], banks 1-7 live in
     * ocula_bank_mem (lazily allocated, 7 x OCULA_BANK_SIZE). The ULA
     * always scans bank 0 — banking is CPU-visible only. */
    uint8_t ocula_bank;          /**< Active CPU bank (0-7) */
    uint8_t* ocula_bank_mem;     /**< Side banks 1-7 storage, NULL until used */

    /* OCULA 80-col BASIC mirror (sprint 44): when true, every write to the
     * 40-col text screen ($BB80-$BFDF) is also mirrored to $A000 in 80-col
     * layout (left 40 of 80 columns per row). Lets standard BASIC PRINT/LIST
     * appear on the 80-col display without ROM modification. */
    bool ocula_80col_mirror;

} memory_t;

/**
 * @brief Initialize memory subsystem
 *
 * @param mem Pointer to memory structure
 * @return true on success, false on failure
 */
bool memory_init(memory_t* mem);

/**
 * @brief Cleanup memory subsystem
 *
 * @param mem Pointer to memory structure
 */
void memory_cleanup(memory_t* mem);

/**
 * @brief Load ROM file
 *
 * @param mem Pointer to memory structure
 * @param filename Path to ROM file
 * @param offset Offset in ROM to load at
 * @return true on success, false on failure
 */
bool memory_load_rom(memory_t* mem, const char* filename, uint16_t offset);

/**
 * @brief Load character set ROM
 *
 * @param mem Pointer to memory structure
 * @param filename Path to charset file
 * @return true on success, false on failure
 */
bool memory_load_charset(memory_t* mem, const char* filename);

/**
 * @brief Read byte from memory
 *
 * @param mem Pointer to memory structure
 * @param address Address to read from
 * @return Byte value
 */
uint8_t memory_read(memory_t* mem, uint16_t address);

/**
 * @brief Write byte to memory
 *
 * @param mem Pointer to memory structure
 * @param address Address to write to
 * @param value Byte value to write
 */
void memory_write(memory_t* mem, uint16_t address, uint8_t value);

/**
 * @brief Read 16-bit word (little-endian)
 *
 * @param mem Pointer to memory structure
 * @param address Address to read from
 * @return Word value
 */
uint16_t memory_read_word(memory_t* mem, uint16_t address);

/**
 * @brief Write 16-bit word (little-endian)
 *
 * @param mem Pointer to memory structure
 * @param address Address to write to
 * @param value Word value to write
 */
void memory_write_word(memory_t* mem, uint16_t address, uint16_t value);

/**
 * @brief Set I/O callbacks for memory-mapped I/O
 *
 * @param mem Pointer to memory structure
 * @param read_cb Read callback function
 * @param write_cb Write callback function
 * @param userdata User data passed to callbacks
 */
void memory_set_io_callbacks(memory_t* mem,
                             uint8_t (*read_cb)(uint16_t, void*),
                             void (*write_cb)(uint16_t, uint8_t, void*),
                             void* userdata);

/**
 * @brief Enable/disable memory access tracing
 *
 * @param mem Pointer to memory structure
 * @param enabled true to enable, false to disable
 * @param callback Trace callback function
 */
void memory_set_trace(memory_t* mem, bool enabled,
                     void (*callback)(uint16_t, uint8_t, mem_access_type_t));

/**
 * @brief Clear all RAM
 *
 * @param mem Pointer to memory structure
 * @param pattern Fill pattern (0x00 or 0xFF typical)
 */
void memory_clear_ram(memory_t* mem, uint8_t pattern);

/**
 * @brief Get pointer to memory region (direct access for speed)
 * WARNING: Use with caution, bypasses banking and I/O
 *
 * @param mem Pointer to memory structure
 * @param address Starting address
 * @return Pointer to memory region or NULL if invalid
 */
uint8_t* memory_get_ptr(memory_t* mem, uint16_t address);

/**
 * @brief Select the OCULA CPU bank for $A000-$BFFF (0-7)
 *
 * Allocates the side-bank storage on first non-zero selection.
 *
 * @param mem Pointer to memory structure
 * @param bank Bank number 0-7 (values are masked to 3 bits)
 * @return true on success, false on allocation failure
 */
bool memory_ocula_set_bank(memory_t* mem, uint8_t bank);

/**
 * @brief Get the active OCULA CPU bank (0 when banking unused)
 */
uint8_t memory_ocula_get_bank(const memory_t* mem);

#endif /* MEMORY_H */
