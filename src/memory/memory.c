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

void memory_cleanup(memory_t* mem) {
    free(mem->ocula_bank_mem);
    mem->ocula_bank_mem = NULL;
    mem->ocula_bank = 0;
}

bool memory_ocula_set_bank(memory_t* mem, uint8_t bank) {
    bank &= 0x07;
    if (bank != 0 && !mem->ocula_bank_mem) {
        mem->ocula_bank_mem = calloc(OCULA_BANK_COUNT - 1, OCULA_BANK_SIZE);
        if (!mem->ocula_bank_mem) return false;
    }
    mem->ocula_bank = bank;
    return true;
}

uint8_t memory_ocula_get_bank(const memory_t* mem) {
    return mem->ocula_bank;
}

void memory_ocula_unlock_write(memory_t* mem, uint8_t value) {
    switch (value) {
        case OCULA_UNLOCK_O:
            mem->ocula_unlock_knock = 1;       /* first knock byte seen */
            break;
        case OCULA_UNLOCK_C:
            if (mem->ocula_unlock_knock == 1)  /* 'O' then 'C': arm */
                mem->ocula_unlocked = true;
            mem->ocula_unlock_knock = 0;
            break;
        case OCULA_UNLOCK_LOCK:
            mem->ocula_unlocked = false;       /* explicit re-lock */
            mem->ocula_unlock_knock = 0;
            mem->ocula_regs_armed = false;     /* register file goes inert too */
            mem->ocula_map_page = 0;           /* unmap the page-3 window */
            break;
        default:
            mem->ocula_unlock_knock = 0;       /* any other byte resets */
            break;
    }
}

void memory_ocula_reg_write(memory_t* mem, uint8_t page, uint8_t value) {
    if (!mem->ocula_unlocked) return;          /* gated by the opt-in unlock */
    if (page >= OCULA_REG_PAL_PAGE && page <= OCULA_REG_PAL_PAGE + 7) {
        mem->ocula_reg_pal[page - OCULA_REG_PAL_PAGE] = value;
        mem->ocula_regs_armed = true;
    } else if (page == OCULA_REG_BORDER_PAGE) {
        mem->ocula_reg_border = value;
        mem->ocula_regs_armed = true;
    } else if (page == OCULA_MAP_PAGE_REG) {
        mem->ocula_map_page = value;           /* sprint 67: map the page-3 window */
    }
}

/* Phase C (sprint 67): is `address` inside the mapped 256-byte OCULA window? */
bool memory_ocula_page_mapped(const memory_t* mem, uint16_t address) {
    return mem->ocula_unlocked && mem->ocula_map_page &&
           (uint8_t)(address >> 8) == mem->ocula_map_page;
}

/* Read one byte of the mapped OCULA window: a R/W view of the OCULA state. */
uint8_t memory_ocula_page_read(const memory_t* mem, uint8_t off) {
    switch (off) {
        case OCULA_PG_SIG_O: return 'O';
        case OCULA_PG_SIG_C: return 'C';
        case OCULA_PG_CAPS:  return 0x1F;      /* 80col|exthires|pal|bank|gpu */
        case OCULA_PG_BANK:  return mem->ocula_bank;
        case OCULA_PG_BORDER: return mem->ocula_reg_border;
        default:
            if (off >= OCULA_PG_PAL && off < OCULA_PG_PAL + 8)
                return mem->ocula_reg_pal[off - OCULA_PG_PAL];
            return 0;                          /* reserved */
    }
}

/* Write one byte of the mapped OCULA window. Palette/border writes arm the
 * register file (same state as the ROM-space writes of Phase A); the bank
 * slot drives the CPU banking; signature/caps/reserved are read-only. */
void memory_ocula_page_write(memory_t* mem, uint8_t off, uint8_t value) {
    if (off == OCULA_PG_BANK) {
        memory_ocula_set_bank(mem, (uint8_t)(value & 0x07));
    } else if (off == OCULA_PG_BORDER) {
        mem->ocula_reg_border = value;
        mem->ocula_regs_armed = true;
    } else if (off >= OCULA_PG_PAL && off < OCULA_PG_PAL + 8) {
        mem->ocula_reg_pal[off - OCULA_PG_PAL] = value;
        mem->ocula_regs_armed = true;
    }
    /* signature / caps / reserved: ignored */
}

bool memory_ocula_regs_armed(const memory_t* mem) {
    return mem->ocula_regs_armed;
}

bool memory_ocula_unlocked(const memory_t* mem) {
    return mem->ocula_unlocked;
}

/* CPU view of the $A000-$BFFF window under OCULA banking. */
static inline uint8_t* ocula_window_ptr(memory_t* mem, uint16_t address) {
    return &mem->ocula_bank_mem[(mem->ocula_bank - 1) * OCULA_BANK_SIZE +
                                (address - OCULA_BANK_BASE)];
}

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

    /* OCULA page-3 window (sprint 67, Phase C): when mapped + unlocked, this
     * CPU page is a R/W view of the OCULA state, overriding normal memory. */
    if (memory_ocula_page_mapped(mem, address)) {
        val = memory_ocula_page_read(mem, (uint8_t)(address & 0xFF));
        mem_notify(mem, address, val, MEM_READ);
        return val;
    }

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
        /* OCULA banking: CPU sees the selected side bank at $A000-$BFFF.
         * The ULA always scans bank 0 (mem->ram). */
        if (mem->ocula_bank != 0 && address >= OCULA_BANK_BASE)
            val = *ocula_window_ptr(mem, address);
        else
            val = mem->ram[address];
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

    /* OCULA page-3 window (sprint 67, Phase C): mapped + unlocked → writes hit
     * the OCULA state (bank/palette/border), overriding normal memory. */
    if (memory_ocula_page_mapped(mem, address)) {
        memory_ocula_page_write(mem, (uint8_t)(address & 0xFF), value);
        return;
    }

    /* I/O space: VIA at $0300-$030F */
    if (address >= 0x0300 && address <= 0x03FF) {
        if (mem->io_write) {
            mem->io_write(address, value, mem->io_userdata);
        }
        return;
    }

    /* RAM: $0000-$BFFF always writable */
    if (address < 0xC000) {
        if (mem->ocula_bank != 0 && address >= OCULA_BANK_BASE)
            *ocula_window_ptr(mem, address) = value;
        else
            mem->ram[address] = value;

        /* OCULA 80-col BASIC mirror: reflect 40-col screen writes to the
         * 80-col screen at $A000. Screen $BB80-$BFDF (40×28) → $A000 left
         * half (col 0-39 of 80). Catches STA ($12),Y, STA $BB80,X and the
         * scroll fill STA $BB7F,Y via the unified write path. */
        if (mem->ocula_80col_mirror &&
            address >= 0xBB80 && address <= 0xBFDF) {
            uint16_t off = address - 0xBB80;
            uint16_t row = off / 40;
            uint16_t col = off % 40;
            if (row < 28) {
                mem->ram[0xA000 + row * 80 + col] = value;
                /* Col 39 (last of 40-col row) is mirrored to col 79 too so
                 * that CAPS/status chars written at the 40-col right edge
                 * appear at the 80-col right edge, not the middle. */
                if (col == 39)
                    mem->ram[0xA000 + row * 80 + 79] = value;
            }
        }
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
    } else {
        /* ROM enabled: the chip ignores the write, but the OCULA snoops
         * ROM-space blind-writes as a write-only register space. The
         * unlock register (page $FB) arms the opt-in extensions. This is
         * only reachable when genuine ROM is mapped — never a RAM-overlay
         * write — matching what the real ULA sees on the bus. */
        uint8_t page = (uint8_t)(address >> 8);
        if ((address & 0xFF00) == OCULA_UNLOCK_PAGE)
            memory_ocula_unlock_write(mem, value);
        else if (page == OCULA_REG_BORDER_PAGE || page == OCULA_MAP_PAGE_REG ||
                 (page >= OCULA_REG_PAL_PAGE && page <= OCULA_REG_PAL_PAGE + 7))
            memory_ocula_reg_write(mem, page, value);   /* sprint 66/67 registers */
    }
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
