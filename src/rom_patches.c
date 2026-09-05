/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file rom_patches.c
 * @brief ROM-patch tables + detection — moved verbatim from main.c (Epic 9/US3).
 * @author bmarty <bmarty@mailo.com>
 */
#include "rom_patches.h"

/* ═══════════════════════════════════════════════════════════════════ */
/*  ROM patch tables (version-specific tape loading addresses)         */
/* ═══════════════════════════════════════════════════════════════════ */

static const rom_patches_t rom_patches_basic10 = {
    .name              = "BASIC 1.0 (ORIC-1)",
    .getsync_entry     = 0xE696,
    .getsync_end       = 0xE6B9,
    .getsync_loop      = 0xE681,
    .readbyte_entry    = 0xE630,
    .readbyte_end      = 0xE65B,
    .readbyte_store    = 0x002F,
    .readbyte_storezero= 0,         /* GetTapeByte on Oric-1 does not maintain $02B1 */
    .readbyte_setcarry = false,     /* and exits with C=0 */
    .csave_header_buf  = 0x005E,    /* Sprint 34at — ZP staging buffer $5E..$66
                                      * (read via LDX#9 / LDA $5D,X / DEX, see
                                      * disasm at $E585). Senior-approved. */
    .csave_filename_buf= 0x0035,    /* filename at $0035 (16 bytes, null-term) */
    .writefileheader_entry = 0xE57B,/* Snapshot trap point — captures $5E..$66
                                      * + $0035 BEFORE the data-write loop
                                      * mutates $5F/$60 as a work pointer. */
    .cload_data_rts    = 0xE502,
    .putbyte_entry     = 0xE5C6,
    .putbyte_end       = 0xE5F2,
    .csave_end         = 0xE80A,    /* Sprint 34at (senior-approved Option A):
                                      * $E80A is the JMP $EBD0 that terminates
                                      * the CSAVE outer routine. $E7FE never
                                      * fires on ORIC-1 (verified via PCLOG in
                                      * cpu_step) — the JSR $E804 at $E7F5 calls
                                      * a sub-routine that JMPs to warm-start
                                      * instead of RTSing. The PHP-orphaned
                                      * stack is reset by the warm-start handler. */
    .writeleader_entry = 0xE6BA,
    .writeleader_end   = 0xE6C9,
    .tape_type_addr    = 0x0064     /* CLOAD header read at $E4BC stores the 9
                                      * header bytes reversed (STA $5D,X / DEX),
                                      * so on-tape byte 3 (file type) lands at
                                      * $64 — NOT $66 ($66 = reserved byte 1). */
};

static const rom_patches_t rom_patches_basic11 = {
    .name              = "BASIC 1.1 (ORIC Atmos)",
    .getsync_entry     = 0xE735,
    .getsync_end       = 0xE759,
    .getsync_loop      = 0xE720,
    .readbyte_entry    = 0xE6C9,
    .readbyte_end      = 0xE6FB,
    .readbyte_store    = 0x002F,
    .readbyte_storezero= 0x02B1,    /* Atmos GetTapeByte zeroes the parity accumulator */
    .readbyte_setcarry = true,      /* and exits with C=1 — VERIFY logic relies on both */
    .csave_header_buf  = 0x02A8,    /* Atmos WriteFileHeader staging : $02A8..$02B0 (reversed
                                      * on-tape order, see disasm at $E60F-$E618) */
    .csave_filename_buf= 0x027F,    /* Atmos filename buffer (16 chars, null-terminated) */
    .writefileheader_entry = 0xE607,/* Sprint 34at: snapshot point for Atmos —
                                      * same defensive pattern (cheap, harmless
                                      * if $02A8..$02B0 isn't mutated post-call). */
    .cload_data_rts    = 0xE50A,
    .putbyte_entry     = 0xE65E,
    .putbyte_end       = 0xE68A,
    .csave_end         = 0xE93C,
    .writeleader_entry = 0xE75A,
    .writeleader_end   = 0xE769,
    .tape_type_addr    = 0x02AE    /* CLOAD header read at $E4B9 stores the 9
                                     * header bytes reversed (STA $02A7,X / DEX),
                                     * so on-tape byte 3 (file type) lands at
                                     * $02AE. */
};

/**
 * @brief Auto-detect ROM version from loaded ROM data
 *
 * Checks the JMP target at ROM offset 0 (address $C000):
 * - BASIC 1.0: JMP $EA59 (4C 59 EA)
 * - BASIC 1.1: JMP $ECCC (4C CC EC)
 *
 * @return Detected model, or ORIC_MODEL_ORIC1 as default
 */
oric_model_t detect_rom_version(const memory_t* mem) {
    /* ROM starts at $C000, which is rom[0] */
    if (mem->rom[0] == 0x4C) {  /* JMP instruction */
        uint16_t target = (uint16_t)mem->rom[1] | ((uint16_t)mem->rom[2] << 8);
        if (target == 0xECCC) {
            return ORIC_MODEL_ATMOS;
        }
    }
    return ORIC_MODEL_ORIC1;
}

const rom_patches_t* get_rom_patches(oric_model_t model) {
    switch (model) {
        case ORIC_MODEL_ATMOS: return &rom_patches_basic11;
        default:               return &rom_patches_basic10;
    }
}
