/**
 * @file savestate.c
 * @brief Phosphoric save state serialization/deserialization
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.4.0-alpha
 *
 * Implements .ost (Oric Save sTate) format:
 * - 48-byte header (magic, version, file size, CRC32, emu version)
 * - Sequential sections: CPU, MEM, VIA, PSG, VID, KBD, FDC, MDC, TAP, SER, META
 * - CRC32 integrity check over all data after the header
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "savestate.h"
#include "emulator.h"
#include "utils/logging.h"

/* ═══════════════════════════════════════════════════════════════════ */
/*  CRC32 (table-driven, ISO 3309 / ITU-T V.42)                     */
/* ═══════════════════════════════════════════════════════════════════ */

static uint32_t crc32_table[256];
static bool crc32_table_ready = false;

static void crc32_init_table(void) {
    if (crc32_table_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
    crc32_table_ready = true;
}

static uint32_t crc32_calc(const uint8_t* data, size_t len) {
    crc32_init_table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Little-endian write helpers                                       */
/* ═══════════════════════════════════════════════════════════════════ */

static void write_u8(FILE* fp, uint8_t v) {
    fwrite(&v, 1, 1, fp);
}

static void write_u16le(FILE* fp, uint16_t v) {
    uint8_t buf[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };
    fwrite(buf, 1, 2, fp);
}

static void write_u32le(FILE* fp, uint32_t v) {
    uint8_t buf[4] = {
        (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
        (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF)
    };
    fwrite(buf, 1, 4, fp);
}

static void write_u64le(FILE* fp, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)((v >> (i * 8)) & 0xFF);
        fwrite(&b, 1, 1, fp);
    }
}

static void write_bool(FILE* fp, bool v) {
    uint8_t b = v ? 1 : 0;
    fwrite(&b, 1, 1, fp);
}

static void write_i32le(FILE* fp, int32_t v) {
    write_u32le(fp, (uint32_t)v);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Little-endian read helpers                                        */
/* ═══════════════════════════════════════════════════════════════════ */

static uint8_t read_u8(FILE* fp) {
    uint8_t v = 0;
    fread(&v, 1, 1, fp);
    return v;
}

static uint16_t read_u16le(FILE* fp) {
    uint8_t buf[2] = {0};
    fread(buf, 1, 2, fp);
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t read_u32le(FILE* fp) {
    uint8_t buf[4] = {0};
    fread(buf, 1, 4, fp);
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static uint64_t read_u64le(FILE* fp) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t b = 0;
        fread(&b, 1, 1, fp);
        v |= (uint64_t)b << (i * 8);
    }
    return v;
}

static bool read_bool(FILE* fp) {
    return read_u8(fp) != 0;
}

static int32_t read_i32le(FILE* fp) {
    return (int32_t)read_u32le(fp);
}

static bool read_section_header(FILE* fp, char tag_out[4], uint32_t* size_out) {
    if (fread(tag_out, 1, 4, fp) != 4) return false;
    *size_out = read_u32le(fp);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Section write helpers (dynamic size calculation)                   */
/* ═══════════════════════════════════════════════════════════════════ */

/* Write section header with placeholder size, return header position */
static long begin_section(FILE* fp, const char tag[4]) {
    long pos = ftell(fp);
    fwrite(tag, 1, 4, fp);
    write_u32le(fp, 0); /* placeholder */
    return pos;
}

/* Fix section size after writing data */
static void end_section(FILE* fp, long header_pos) {
    long end_pos = ftell(fp);
    uint32_t size = (uint32_t)(end_pos - header_pos - 8);
    fseek(fp, header_pos + 4, SEEK_SET);
    write_u32le(fp, size);
    fseek(fp, end_pos, SEEK_SET);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  savestate_save                                                    */
/* ═══════════════════════════════════════════════════════════════════ */

bool savestate_save(const emulator_t* emu, const char* filename) {
    if (!emu || !filename) return false;

    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        log_error("savestate: cannot open '%s' for writing", filename);
        return false;
    }

    /* ── Header (48 bytes) ── */
    fwrite(SAVESTATE_MAGIC, 1, 4, fp);          /* 0x0000: magic */
    write_u32le(fp, SAVESTATE_VERSION);          /* 0x0004: version */
    write_u32le(fp, 0);                          /* 0x0008: file_size placeholder */
    write_u32le(fp, 0);                          /* 0x000C: crc32 placeholder */
    char version_str[32];
    memset(version_str, 0, sizeof(version_str));
    strncpy(version_str, EMU_VERSION, sizeof(version_str) - 1);
    fwrite(version_str, 1, 32, fp);             /* 0x0010: emu version */

    long sec;

    /* ── CPU Section ── */
    sec = begin_section(fp, "CPU\0");
    write_u8(fp, emu->cpu.A);
    write_u8(fp, emu->cpu.X);
    write_u8(fp, emu->cpu.Y);
    write_u8(fp, emu->cpu.SP);
    write_u16le(fp, emu->cpu.PC);
    write_u8(fp, emu->cpu.P);
    write_u64le(fp, emu->cpu.cycles);
    write_u8(fp, emu->cpu.irq);
    end_section(fp, sec);

    /* ── MEM Section ── */
    sec = begin_section(fp, "MEM\0");
    fwrite(emu->memory.ram, 1, RAM_SIZE, fp);
    fwrite(emu->memory.upper_ram, 1, ROM_SIZE, fp);
    write_u8(fp, (uint8_t)emu->memory.charset_bank);
    write_bool(fp, emu->memory.rom_enabled);
    write_bool(fp, emu->memory.overlay_active);
    write_bool(fp, emu->memory.basic_rom_disabled);
    end_section(fp, sec);

    /* ── VIA Section ── */
    sec = begin_section(fp, "VIA\0");
    write_u8(fp, emu->via.ora);
    write_u8(fp, emu->via.orb);
    write_u8(fp, emu->via.ira);
    write_u8(fp, emu->via.irb);
    write_u8(fp, emu->via.ddra);
    write_u8(fp, emu->via.ddrb);
    write_u16le(fp, emu->via.t1_counter);
    write_u16le(fp, emu->via.t1_latch);
    write_u16le(fp, emu->via.t2_counter);
    write_u8(fp, emu->via.t2_latch);
    write_bool(fp, emu->via.t1_running);
    write_bool(fp, emu->via.t2_running);
    write_u8(fp, emu->via.sr);
    write_u8(fp, emu->via.sr_count);
    write_u8(fp, emu->via.acr);
    write_u8(fp, emu->via.pcr);
    write_u8(fp, emu->via.ifr);
    write_u8(fp, emu->via.ier);
    write_bool(fp, emu->via.cb1_pin);
    write_bool(fp, emu->via.irq_line);
    end_section(fp, sec);

    /* ── PSG Section ── */
    sec = begin_section(fp, "PSG\0");
    fwrite(emu->psg.registers, 1, AY_NUM_REGISTERS, fp);
    write_u8(fp, emu->psg.selected_reg);
    for (int i = 0; i < 3; i++) write_u16le(fp, emu->psg.tone_period[i]);
    for (int i = 0; i < 3; i++) write_u32le(fp, emu->psg.tone_counter[i]);
    for (int i = 0; i < 3; i++) write_u8(fp, emu->psg.tone_output[i]);
    write_u16le(fp, emu->psg.noise_period);
    write_u32le(fp, emu->psg.noise_counter);
    write_u32le(fp, emu->psg.noise_shift);
    write_u8(fp, emu->psg.noise_output);
    write_u16le(fp, emu->psg.env_period);
    write_u32le(fp, emu->psg.env_counter);
    write_u8(fp, emu->psg.env_shape);
    write_u8(fp, emu->psg.env_step);
    write_u8(fp, emu->psg.env_volume);
    write_bool(fp, emu->psg.env_holding);
    write_u32le(fp, emu->psg.clock_rate);
    end_section(fp, sec);

    /* ── VID Section ── */
    sec = begin_section(fp, "VID\0");
    write_u8(fp, emu->video.hires_mode ? 1 : 0);
    write_u8(fp, emu->video.vid_mode);
    end_section(fp, sec);

    /* ── KBD Section ── */
    sec = begin_section(fp, "KBD\0");
    fwrite(emu->keyboard.matrix, 1, 8, fp);
    end_section(fp, sec);

    /* ── FDC Section (if Microdisc present) ── */
    if (emu->has_microdisc) {
        sec = begin_section(fp, "FDC\0");
        const fdc_t* fdc = &emu->microdisc.fdc;
        write_u8(fp, fdc->status);
        write_u8(fp, fdc->command);
        write_u8(fp, fdc->track);
        write_u8(fp, fdc->sector);
        write_u8(fp, fdc->data);
        write_u8(fp, fdc->direction);
        write_u8(fp, fdc->c_track);
        write_u8(fp, fdc->c_sector);
        write_u8(fp, fdc->side);
        write_u8(fp, (uint8_t)fdc->currentop);
        write_u16le(fp, fdc->cur_sector_len);
        write_u16le(fp, fdc->cur_offset);
        write_u8(fp, fdc->sec_type);
        write_i32le(fp, fdc->delayed_drq);
        write_i32le(fp, fdc->delayed_int);
        write_i32le(fp, fdc->di_status);
        write_i32le(fp, fdc->dd_status);
        end_section(fp, sec);

        /* ── MDC Section ── */
        sec = begin_section(fp, "MDC\0");
        write_u8(fp, emu->microdisc.status);
        write_u8(fp, emu->microdisc.intrq);
        write_u8(fp, emu->microdisc.drq);
        write_bool(fp, emu->microdisc.diskrom);
        write_bool(fp, emu->microdisc.romdis);
        write_bool(fp, emu->microdisc.intena);
        write_u8(fp, emu->microdisc.drive);
        write_u8(fp, emu->microdisc.side);
        end_section(fp, sec);

        /* ── DSK Section: the actual disk images, so in-game saves (sectors the
         * guest wrote this session) survive save/load. One record per loaded
         * drive: index, geometry, size, then the raw bytes. ── */
        sec = begin_section(fp, "DSK\0");
        uint8_t ndrives = 0;
        for (int i = 0; i < MICRODISC_MAX_DRIVES; i++)
            if (emu->disks[i]) ndrives++;
        write_u8(fp, ndrives);
        for (int i = 0; i < MICRODISC_MAX_DRIVES; i++) {
            if (!emu->disks[i]) continue;
            write_u8(fp, (uint8_t)i);
            write_u32le(fp, emu->disks[i]->size);
            write_u8(fp, emu->disks[i]->tracks);
            write_u8(fp, emu->disks[i]->sectors);
            write_u8(fp, emu->disks[i]->sides);
            fwrite(emu->disks[i]->data, 1, emu->disks[i]->size, fp);
        }
        end_section(fp, sec);
    }

    /* ── TAP Section ── */
    sec = begin_section(fp, "TAP\0");
    write_u8(fp, emu->tape_loaded ? 1 : 0);
    write_i32le(fp, emu->tapelen);
    write_i32le(fp, emu->tapeoffs);
    write_i32le(fp, emu->tape_syncstack);
    end_section(fp, sec);

    /* ── SER Section (if serial present) ── */
    if (emu->has_serial) {
        sec = begin_section(fp, "SER\0");
        write_u8(fp, emu->acia.tdr);
        write_u8(fp, emu->acia.rdr);
        write_u8(fp, emu->acia.status);
        write_u8(fp, emu->acia.command);
        write_u8(fp, emu->acia.control);
        write_bool(fp, emu->acia.tx_pending);
        write_bool(fp, emu->acia.rx_full);
        write_bool(fp, emu->acia.irq_line);
        write_i32le(fp, emu->acia.tx_cycles);
        write_i32le(fp, emu->acia.rx_cycles);
        write_bool(fp, emu->acia.v23_mode);
        write_bool(fp, emu->acia.dcd);
        write_bool(fp, emu->acia.dsr);
        write_bool(fp, emu->acia.cts);
        end_section(fp, sec);
    }

    /* ── META Section ── */
    const char* rom_path = emu->rom_path ? emu->rom_path : "";
    const char* disk_path = emu->disk_path ? emu->disk_path : "";
    const char* diskrom_path = emu->diskrom_path ? emu->diskrom_path : "";
    const char* tape_path = emu->tape_path ? emu->tape_path : "";
    sec = begin_section(fp, "META");
    fwrite(rom_path, 1, strlen(rom_path) + 1, fp);
    fwrite(disk_path, 1, strlen(disk_path) + 1, fp);
    fwrite(diskrom_path, 1, strlen(diskrom_path) + 1, fp);
    fwrite(tape_path, 1, strlen(tape_path) + 1, fp);
    end_section(fp, sec);

    /* ── Fix header: file size ── */
    long file_size = ftell(fp);
    fseek(fp, 8, SEEK_SET);
    write_u32le(fp, (uint32_t)file_size);

    /* ── Fix header: CRC32 over data after header ── */
    fseek(fp, 0, SEEK_SET);
    uint8_t* file_data = (uint8_t*)malloc((size_t)file_size);
    if (!file_data) {
        fclose(fp);
        log_error("savestate: memory allocation failed for CRC32");
        return false;
    }
    /* Re-read the file for CRC32 calculation */
    fclose(fp);
    fp = fopen(filename, "rb");
    if (!fp || fread(file_data, 1, (size_t)file_size, fp) != (size_t)file_size) {
        free(file_data);
        if (fp) fclose(fp);
        log_error("savestate: failed to re-read file for CRC32");
        return false;
    }
    fclose(fp);

    /* CRC32 over bytes after the 48-byte header */
    uint32_t crc = 0;
    if (file_size > SAVESTATE_HEADER_SIZE) {
        crc = crc32_calc(file_data + SAVESTATE_HEADER_SIZE,
                         (size_t)(file_size - SAVESTATE_HEADER_SIZE));
    }
    free(file_data);

    /* Write CRC32 at offset 0x0C */
    fp = fopen(filename, "r+b");
    if (!fp) {
        log_error("savestate: failed to re-open file for CRC32 write");
        return false;
    }
    fseek(fp, 0x0C, SEEK_SET);
    write_u32le(fp, crc);
    fclose(fp);

    log_info("savestate: saved to '%s' (%ld bytes, CRC32=%08X)", filename, file_size, crc);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  savestate_load                                                    */
/* ═══════════════════════════════════════════════════════════════════ */

bool savestate_load(emulator_t* emu, const char* filename) {
    if (!emu || !filename) return false;

    FILE* fp = fopen(filename, "rb");
    if (!fp) {
        log_error("savestate: cannot open '%s' for reading", filename);
        return false;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size < SAVESTATE_HEADER_SIZE) {
        log_error("savestate: file too small (%ld bytes)", file_size);
        fclose(fp);
        return false;
    }

    /* Read entire file for CRC32 verification */
    uint8_t* file_data = (uint8_t*)malloc((size_t)file_size);
    if (!file_data) {
        log_error("savestate: memory allocation failed");
        fclose(fp);
        return false;
    }
    if (fread(file_data, 1, (size_t)file_size, fp) != (size_t)file_size) {
        log_error("savestate: read error");
        free(file_data);
        fclose(fp);
        return false;
    }
    fclose(fp);

    /* ── Verify header ── */
    if (memcmp(file_data, SAVESTATE_MAGIC, 4) != 0) {
        log_error("savestate: invalid magic (not an OST1 file)");
        free(file_data);
        return false;
    }

    uint32_t version = (uint32_t)file_data[4] | ((uint32_t)file_data[5] << 8) |
                       ((uint32_t)file_data[6] << 16) | ((uint32_t)file_data[7] << 24);
    if (version != SAVESTATE_VERSION) {
        log_error("savestate: unsupported version %u (expected %u)", version, SAVESTATE_VERSION);
        free(file_data);
        return false;
    }

    uint32_t stored_crc = (uint32_t)file_data[12] | ((uint32_t)file_data[13] << 8) |
                          ((uint32_t)file_data[14] << 16) | ((uint32_t)file_data[15] << 24);

    /* Verify CRC32 */
    uint32_t calc_crc = 0;
    if (file_size > SAVESTATE_HEADER_SIZE) {
        calc_crc = crc32_calc(file_data + SAVESTATE_HEADER_SIZE,
                              (size_t)(file_size - SAVESTATE_HEADER_SIZE));
    }
    if (stored_crc != calc_crc) {
        log_error("savestate: CRC32 mismatch (stored=%08X, calculated=%08X)", stored_crc, calc_crc);
        free(file_data);
        return false;
    }

    char emu_version[33] = {0};
    memcpy(emu_version, file_data + 16, 32);
    log_info("savestate: loading from '%s' (emu version: %s)", filename, emu_version);

    /* ── Parse sections ── */
    /* Use FILE* for convenience with read helpers */
    fp = fopen(filename, "rb");
    if (!fp) {
        free(file_data);
        return false;
    }
    fseek(fp, SAVESTATE_HEADER_SIZE, SEEK_SET);

    char tag[4];
    uint32_t sec_size;

    while (read_section_header(fp, tag, &sec_size)) {
        long sec_start = ftell(fp);

        if (memcmp(tag, "CPU\0", 4) == 0) {
            emu->cpu.A = read_u8(fp);
            emu->cpu.X = read_u8(fp);
            emu->cpu.Y = read_u8(fp);
            emu->cpu.SP = read_u8(fp);
            emu->cpu.PC = read_u16le(fp);
            emu->cpu.P = read_u8(fp);
            emu->cpu.cycles = read_u64le(fp);
            emu->cpu.irq = read_u8(fp);
        } else if (memcmp(tag, "MEM\0", 4) == 0) {
            fread(emu->memory.ram, 1, RAM_SIZE, fp);
            fread(emu->memory.upper_ram, 1, ROM_SIZE, fp);
            emu->memory.charset_bank = (memory_bank_t)read_u8(fp);
            emu->memory.rom_enabled = read_bool(fp);
            emu->memory.overlay_active = read_bool(fp);
            emu->memory.basic_rom_disabled = read_bool(fp);
        } else if (memcmp(tag, "VIA\0", 4) == 0) {
            emu->via.ora = read_u8(fp);
            emu->via.orb = read_u8(fp);
            emu->via.ira = read_u8(fp);
            emu->via.irb = read_u8(fp);
            emu->via.ddra = read_u8(fp);
            emu->via.ddrb = read_u8(fp);
            emu->via.t1_counter = read_u16le(fp);
            emu->via.t1_latch = read_u16le(fp);
            emu->via.t2_counter = read_u16le(fp);
            emu->via.t2_latch = read_u8(fp);
            emu->via.t1_running = read_bool(fp);
            emu->via.t2_running = read_bool(fp);
            emu->via.sr = read_u8(fp);
            emu->via.sr_count = read_u8(fp);
            emu->via.acr = read_u8(fp);
            emu->via.pcr = read_u8(fp);
            emu->via.ifr = read_u8(fp);
            emu->via.ier = read_u8(fp);
            emu->via.cb1_pin = read_bool(fp);
            emu->via.irq_line = read_bool(fp);
        } else if (memcmp(tag, "PSG\0", 4) == 0) {
            fread(emu->psg.registers, 1, AY_NUM_REGISTERS, fp);
            emu->psg.selected_reg = read_u8(fp);
            for (int i = 0; i < 3; i++) emu->psg.tone_period[i] = read_u16le(fp);
            for (int i = 0; i < 3; i++) emu->psg.tone_counter[i] = read_u32le(fp);
            for (int i = 0; i < 3; i++) emu->psg.tone_output[i] = read_u8(fp);
            emu->psg.noise_period = read_u16le(fp);
            emu->psg.noise_counter = read_u32le(fp);
            emu->psg.noise_shift = read_u32le(fp);
            emu->psg.noise_output = read_u8(fp);
            emu->psg.env_period = read_u16le(fp);
            emu->psg.env_counter = read_u32le(fp);
            emu->psg.env_shape = read_u8(fp);
            emu->psg.env_step = read_u8(fp);
            emu->psg.env_volume = read_u8(fp);
            emu->psg.env_holding = read_bool(fp);
            emu->psg.clock_rate = read_u32le(fp);
        } else if (memcmp(tag, "VID\0", 4) == 0) {
            bool hires = read_u8(fp) != 0;
            emu->video.vid_mode = read_u8(fp);
            emu->video.hires_mode = hires;
            /* Recalculate video pointers */
            emu->video.charset = emu->memory.charset;
            if (hires) {
                emu->video.screen_ram = emu->memory.ram + 0xA000;
            } else {
                emu->video.screen_ram = emu->memory.ram + 0xBB80;
            }
            /* Regenerate framebuffer */
            video_render_frame(&emu->video, emu->memory.ram);
        } else if (memcmp(tag, "KBD\0", 4) == 0) {
            fread(emu->keyboard.matrix, 1, 8, fp);
        } else if (memcmp(tag, "FDC\0", 4) == 0) {
            fdc_t* fdc = &emu->microdisc.fdc;
            fdc->status = read_u8(fp);
            fdc->command = read_u8(fp);
            fdc->track = read_u8(fp);
            fdc->sector = read_u8(fp);
            fdc->data = read_u8(fp);
            fdc->direction = read_u8(fp);
            fdc->c_track = read_u8(fp);
            fdc->c_sector = read_u8(fp);
            fdc->side = read_u8(fp);
            fdc->currentop = (fdc_op_t)read_u8(fp);
            fdc->cur_sector_len = read_u16le(fp);
            fdc->cur_offset = read_u16le(fp);
            fdc->sec_type = read_u8(fp);
            fdc->delayed_drq = read_i32le(fp);
            fdc->delayed_int = read_i32le(fp);
            fdc->di_status = read_i32le(fp);
            fdc->dd_status = read_i32le(fp);
        } else if (memcmp(tag, "MDC\0", 4) == 0) {
            emu->microdisc.status = read_u8(fp);
            emu->microdisc.intrq = read_u8(fp);
            emu->microdisc.drq = read_u8(fp);
            emu->microdisc.diskrom = read_bool(fp);
            emu->microdisc.romdis = read_bool(fp);
            emu->microdisc.intena = read_bool(fp);
            emu->microdisc.drive = read_u8(fp);
            emu->microdisc.side = read_u8(fp);
            /* Recalculate FDC pointers from microdisc state */
            emu->microdisc.fdc.side = emu->microdisc.side;
            uint8_t drive = emu->microdisc.drive;
            if (drive < MICRODISC_MAX_DRIVES) {
                emu->microdisc.fdc.disk_data = emu->microdisc.disk_data[drive];
                emu->microdisc.fdc.disk_size = emu->microdisc.disk_size[drive];
                emu->microdisc.fdc.tracks = emu->microdisc.disk_tracks[drive];
                emu->microdisc.fdc.sectors_per_track = emu->microdisc.disk_sectors[drive];
            }
            /* Reset cur_sector_data — will be recalculated on next FDC operation */
            emu->microdisc.fdc.cur_sector_data = NULL;
        } else if (memcmp(tag, "DSK\0", 4) == 0) {
            /* Restore the disk images captured at save time (in-game saves). */
            uint8_t ndrives = read_u8(fp);
            for (uint8_t k = 0; k < ndrives; k++) {
                uint8_t drive = read_u8(fp);
                uint32_t dsize = read_u32le(fp);
                uint8_t dtracks = read_u8(fp);
                uint8_t dsectors = read_u8(fp);
                uint8_t dsides = read_u8(fp);
                if (drive >= MICRODISC_MAX_DRIVES) {  /* defensive: skip payload */
                    fseek(fp, dsize, SEEK_CUR);
                    continue;
                }
                /* Ensure a sedoric_disk_t with a buffer of the right size. */
                if (!emu->disks[drive]) {
                    emu->disks[drive] = (sedoric_disk_t*)calloc(1, sizeof(sedoric_disk_t));
                }
                if (emu->disks[drive] &&
                    (emu->disks[drive]->data == NULL ||
                     emu->disks[drive]->size != dsize)) {
                    uint8_t* nd = (uint8_t*)realloc(emu->disks[drive]->data, dsize);
                    if (nd) emu->disks[drive]->data = nd;
                }
                if (emu->disks[drive] && emu->disks[drive]->data) {
                    fread(emu->disks[drive]->data, 1, dsize, fp);
                    emu->disks[drive]->size = dsize;
                    emu->disks[drive]->tracks = dtracks;
                    emu->disks[drive]->sectors = dsectors;
                    emu->disks[drive]->sides = dsides;
                    /* Re-point the Microdisc (and FDC if this is the live drive)
                     * at the restored buffer. */
                    microdisc_set_disk(&emu->microdisc, drive,
                                       emu->disks[drive]->data, dsize,
                                       dtracks, dsectors);
                } else {
                    fseek(fp, dsize, SEEK_CUR);  /* allocation failed — skip */
                }
            }
        } else if (memcmp(tag, "TAP\0", 4) == 0) {
            emu->tape_loaded = read_u8(fp) != 0;
            emu->tapelen = read_i32le(fp);
            emu->tapeoffs = read_i32le(fp);
            emu->tape_syncstack = read_i32le(fp);
        } else if (memcmp(tag, "SER\0", 4) == 0) {
            emu->acia.tdr = read_u8(fp);
            emu->acia.rdr = read_u8(fp);
            emu->acia.status = read_u8(fp);
            emu->acia.command = read_u8(fp);
            emu->acia.control = read_u8(fp);
            emu->acia.tx_pending = read_bool(fp);
            emu->acia.rx_full = read_bool(fp);
            emu->acia.irq_line = read_bool(fp);
            emu->acia.tx_cycles = read_i32le(fp);
            emu->acia.rx_cycles = read_i32le(fp);
            emu->acia.v23_mode = read_bool(fp);
            emu->acia.dcd = read_bool(fp);
            emu->acia.dsr = read_bool(fp);
            emu->acia.cts = read_bool(fp);
            /* Recalculate timing from restored registers */
            // Note: acia_update_timing needs to exist - just call it if possible
        } else if (memcmp(tag, "META", 4) == 0) {
            /* Read and log metadata (info only, don't override loaded paths) */
            char meta_buf[1024] = {0};
            uint32_t to_read = sec_size < sizeof(meta_buf) - 1 ? sec_size : (uint32_t)(sizeof(meta_buf) - 1);
            fread(meta_buf, 1, to_read, fp);
            log_info("savestate: metadata: rom='%s'", meta_buf);
        } else {
            /* Unknown section — skip for forward compatibility */
            log_info("savestate: skipping unknown section '%.4s' (%u bytes)", tag, sec_size);
        }

        /* Ensure we're at the right position for the next section */
        fseek(fp, sec_start + (long)sec_size, SEEK_SET);
    }

    fclose(fp);
    free(file_data);

    /* ── Restore internal pointers ── */
    emu->cpu.memory = &emu->memory;

    log_info("savestate: loaded successfully from '%s'", filename);
    return true;
}
