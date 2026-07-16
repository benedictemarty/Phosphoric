/**
 * @file memory.c
 * @brief ORIC-1 Memory management - complete implementation
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-beta.7
 *
 * Memory Map:
 * $0000-$00FF: Zero Page
 * $0100-$01FF: Stack
 * $0200-$02FF: System variables
 * $0300-$03FF: I/O area (VIA at $0300-$030F)
 * $0400-$BFFF: User RAM / Screen RAM
 * $C000-$F7FF: BASIC ROM (or RAM overlay)
 * $F800-$FFFF: Monitor ROM (always ROM for vectors)
 */

#include "memory/memory.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

bool memory_init(memory_t* mem) {
    memset(mem, 0, sizeof(memory_t));

    /* Initialize RAM with Oricutron-compatible pattern (rampattern=0):
     * Per 256-byte page: first 128 bytes = 0x00, next 128 bytes = 0xFF.
     * This is critical for Sedoric boot: the boot code at $B932 checksums
     * upper_ram $C980-$FFFF. If all zeros, only 4 sectors are loaded
     * (mini-loader), missing the full SYSTEM.DOS (60 sectors). */
    for (uint32_t i = 0; i < RAM_SIZE; i += 256) {
        /* First 128 bytes already 0x00 from memset */
        uint32_t end = i + 256;
        if (end > RAM_SIZE) end = RAM_SIZE;
        uint32_t half = i + 128;
        if (half < end) {
            memset(&mem->ram[half], 0xFF, end - half);
        }
    }
    for (uint32_t i = 0; i < ROM_SIZE; i += 256) {
        /* upper_ram: same pattern */
        uint32_t end = i + 256;
        if (end > ROM_SIZE) end = ROM_SIZE;
        uint32_t half = i + 128;
        if (half < end) {
            memset(&mem->upper_ram[half], 0xFF, end - half);
        }
    }

    mem->rom_enabled = true;
    mem->charset_bank = BANK_ROM;
    return true;
}

void memory_cleanup(memory_t* mem) { (void)mem; }

bool memory_load_rom(memory_t* mem, const char* filename, uint16_t offset) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (offset + size > ROM_SIZE) {
        fclose(fp);
        return false;
    }

    size_t rd = fread(mem->rom + offset, 1, (size_t)size, fp);
    fclose(fp);
    return rd == (size_t)size;
}

bool memory_load_charset(memory_t* mem, const char* filename) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) return false;

    size_t rd = fread(mem->charset, 1, sizeof(mem->charset), fp);
    fclose(fp);
    return rd > 0;
}

/* Fire both trace hooks: the watchpoint callback (gated by trace_enabled) and
 * the independent secondary hook (conditional CPU trace), if set. */
static inline void mem_notify(memory_t* mem, uint16_t addr, uint8_t val,
                              mem_access_type_t type) {
    if (mem->trace_enabled && mem->trace_callback)
        mem->trace_callback(addr, val, type);
    if (mem->trace_callback2)
        mem->trace_callback2(addr, val, type);
}

uint8_t memory_read(memory_t* mem, uint16_t address) {
    uint8_t val;

    /* I/O space: VIA 6522 at $0300-$030F (mirrored in $0300-$03FF) */
    if (address >= 0x0300 && address <= 0x03FF) {
        if (mem->io_read) {
            val = mem->io_read(address, mem->io_userdata);
            mem_notify(mem, address, val, MEM_READ);
            return val;
        }
    }

    /* RAM: $0000-$BFFF */
    if (address < 0xC000) {
        val = mem->ram[address];
        /* RAM stuck-bit fault injection (US6): zero cost when no faults set. */
        if (mem->stuck0 | mem->stuck1)
            val = (uint8_t)((val & (uint8_t)~mem->stuck0) | mem->stuck1);
        mem_notify(mem, address, val, MEM_READ);
        return val;
    }

    /* ROM area: $C000-$FFFF
     *
     * Microdisc memory map (matching Oricutron):
     * | romdis | diskrom | $C000-$DFFF | $E000-$FFFF      |
     * |--------|---------|-------------|-------------------|
     * | false  | any     | BASIC ROM   | BASIC ROM         |
     * | true   | true    | RAM         | microdis.rom      |
     * | true   | false   | RAM         | RAM               |
     */
    if (mem->basic_rom_disabled) {
        /* Microdisc mode: BASIC ROM is disabled (romdis=true) */
        if (mem->overlay_active && mem->overlay_rom && address >= 0xE000) {
            /* diskrom=true: overlay ROM (microdis.rom) at $E000-$FFFF */
            uint16_t rom_offset = address - 0xE000;
            if (rom_offset < mem->overlay_rom_size) {
                val = mem->overlay_rom[rom_offset];
            } else {
                val = mem->upper_ram[address - 0xC000];
            }
        } else {
            /* diskrom=false or $C000-$DFFF: RAM */
            val = mem->upper_ram[address - 0xC000];
        }
    } else {
        /* Normal mode: BASIC ROM at $C000-$FFFF */
        val = mem->rom[address - 0xC000];
    }

    mem_notify(mem, address, val, MEM_READ);
    return val;
}

void memory_write(memory_t* mem, uint16_t address, uint8_t value) {
    mem_notify(mem, address, value, MEM_WRITE);

    /* I/O space: VIA at $0300-$030F */
    if (address >= 0x0300 && address <= 0x03FF) {
        if (mem->io_write) {
            mem->io_write(address, value, mem->io_userdata);
        }
        return;
    }

    /* RAM: $0000-$BFFF always writable */
    if (address < 0xC000) {
        mem->ram[address] = value;
        return;
    }

    /* ROM overlay area: $C000-$FFFF */
    if (mem->basic_rom_disabled) {
        /* Microdisc mode: RAM writable at $C000-$FFFF (except overlay ROM) */
        if (mem->overlay_active && address >= 0xE000) {
            /* Overlay ROM area: writes ignored (ROM is read-only) */
        } else {
            mem->upper_ram[address - 0xC000] = value;
        }
    } else if (!mem->rom_enabled) {
        /* Legacy mode: Write to overlay (stored in rom array when ROM is disabled) */
        mem->rom[address - 0xC000] = value;
    }
    /* ROM enabled: the chip ignores the write. */
}

uint16_t memory_read_word(memory_t* mem, uint16_t address) {
    uint8_t lo = memory_read(mem, address);
    uint8_t hi = memory_read(mem, (uint16_t)(address + 1));
    return (uint16_t)((hi << 8) | lo);
}

void memory_write_word(memory_t* mem, uint16_t address, uint16_t value) {
    memory_write(mem, address, (uint8_t)(value & 0xFF));
    memory_write(mem, (uint16_t)(address + 1), (uint8_t)(value >> 8));
}

void memory_set_io_callbacks(memory_t* mem,
                             uint8_t (*read_cb)(uint16_t, void*),
                             void (*write_cb)(uint16_t, uint8_t, void*),
                             void* userdata) {
    mem->io_read = read_cb;
    mem->io_write = write_cb;
    mem->io_userdata = userdata;
}

void memory_set_trace(memory_t* mem, bool enabled,
                     void (*callback)(uint16_t, uint8_t, mem_access_type_t)) {
    mem->trace_enabled = enabled;
    mem->trace_callback = callback;
}

void memory_set_trace2(memory_t* mem,
                     void (*callback)(uint16_t, uint8_t, mem_access_type_t)) {
    mem->trace_callback2 = callback;
}

void memory_set_stuck_bits(memory_t* mem, uint8_t stuck0, uint8_t stuck1) {
    mem->stuck0 = stuck0;
    mem->stuck1 = stuck1;
}

void memory_clear_ram(memory_t* mem, uint8_t pattern) {
    if (pattern == 0) {
        /* Oricutron-compatible pattern: 128x 0x00 + 128x 0xFF per page */
        for (uint32_t i = 0; i < RAM_SIZE; i += 256) {
            uint32_t end = i + 256;
            if (end > RAM_SIZE) end = RAM_SIZE;
            uint32_t half = i + 128;
            if (half > end) half = end;
            memset(&mem->ram[i], 0x00, half - i);
            if (half < end)
                memset(&mem->ram[half], 0xFF, end - half);
        }
        for (uint32_t i = 0; i < ROM_SIZE; i += 256) {
            uint32_t end = i + 256;
            if (end > ROM_SIZE) end = ROM_SIZE;
            uint32_t half = i + 128;
            if (half > end) half = end;
            memset(&mem->upper_ram[i], 0x00, half - i);
            if (half < end)
                memset(&mem->upper_ram[half], 0xFF, end - half);
        }
    } else {
        memset(mem->ram, pattern, RAM_SIZE);
        memset(mem->upper_ram, pattern, ROM_SIZE);
    }
}

uint8_t* memory_get_ptr(memory_t* mem, uint16_t address) {
    if (address < RAM_SIZE) {
        return &mem->ram[address];
    }
    return NULL;
}
