/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file tape_patches.c
 * @brief ROM CLOAD/CSAVE PC-matching patches — moved verbatim from main.c.
 * @author bmarty <bmarty@mailo.com>
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io/tape_patches.h"
#include "io/cassette.h"
#include "io/loci_sdimg.h"
#include "memory/memory.h"
#include "utils/logging.h"

/**
 * @brief ROM patching for CLOAD support
 *
 * Intercepts ROM cassette routines by checking CPU PC after each instruction.
 * When PC hits known ROM entry points (getsync, readbyte), we inject tape
 * data directly into CPU registers and skip to the routine's RTS.
 * This is the same approach used by Oricutron.
 *
 * Addresses are ROM-version-specific, loaded from emu->rom_patches:
 *   BASIC 1.0 (ORIC-1):  getsync=$E696, readbyte=$E630, loop=$E681
 *   BASIC 1.1 (Atmos):   getsync=$E735, readbyte=$E6C9, loop=$E720
 */
void tape_patches(emulator_t* emu) {
    if (!emu->rom_patches)
        return;

    /* Mode capture tape-OUT (voie A CSAVE) : la ROM bit-bange la vraie broche
     * PB7 (Timer 1). Neutraliser TOUS les hooks CSAVE PC-1.1, sinon ils
     * court-circuitent l'encodeur (cf. SPEC-voie-A §1). */
    if (emu->tape_capture.active)
        return;

    const rom_patches_t* p = emu->rom_patches;
    uint16_t pc = emu->cpu.PC;

    /* CSAVE patches work even without a tape loaded */
    if (pc == p->writeleader_entry || pc == p->putbyte_entry || pc == p->csave_end ||
        (p->writefileheader_entry && pc == p->writefileheader_entry)) {
        goto do_patch;  /* Skip tape_loaded check for CSAVE */
    }

    /* Signal-level mode (Sprint 90): the real ROM read routine samples the
     * waveform on CB1, so the getsync/readbyte read patches must stay off.
     * CSAVE (write) patches above still apply via the goto.
     *
     * The PB6 motor line is unusable as a gate: the keyboard column scan writes
     * the same ORB bits during CLOAD and at the READY prompt. Instead gate the
     * waveform on the CPU executing inside the ROM tape-read routines
     * [readbyte_entry .. getsync_end], rewinding at the first entry. Emission
     * pauses (position preserved) while the caller processes a byte or block —
     * exactly what multi-block custom loaders need. */
    if (emu->cassette.signal_mode) {
        /* --tape-signal-free : le moteur est piloté par ORB PB6 (cf. via write),
         * pas par le PC 1.1 -> ne pas l'écraser ici (ROM clean-room). */
        if (emu->cassette.free_gate)
            return;
        bool reading = (pc >= p->readbyte_entry && pc <= p->getsync_end);
        if (reading && !emu->cassette.started) {
            cassette_rewind(&emu->cassette);
            emu->cassette.started = true;
        }
        cassette_set_motor(&emu->cassette, reading);
        return;
    }

    if (!emu->tape_loaded)
        return;

do_patch:
    if (pc == p->getsync_entry) {
        /* getsync: scan forward to first 0x16 sync byte.
         * Leave tapeoffs pointing AT the 0x16 so readbyte will
         * read the sync bytes (ROM confirmation loop needs them).
         * The ORIC ROM reads 9 header bytes after $24, which
         * correctly parses start/end addresses from the raw TAP. */
        if (emu->tapebuf[emu->tapeoffs] != 0x16) {
            while (emu->tapeoffs < emu->tapelen &&
                   emu->tapebuf[emu->tapeoffs] != 0x16) {
                emu->tapeoffs++;
            }
            if (emu->tapeoffs >= emu->tapelen)
                return;
        }
        log_info("TAPE: getsync at tapeoffs=%d/%d", emu->tapeoffs, emu->tapelen);
        /* Save stack pointer for sync loop recovery */
        emu->tape_syncstack = emu->cpu.SP;
        /* Jump to end of getsync */
        emu->cpu.PC = p->getsync_end;
    } else if (pc == p->readbyte_entry) {
        /* readbyte: feed next byte from tape buffer to ROM. Sprint 34ar
         * (senior-review fix): mirror what the real GetTapeByte does
         * version-by-version — on Atmos, $02B1 is the parity accumulator
         * and the routine exits with C=1 ; on Oric-1, neither applies.
         * Without these two effects, BASIC 1.1's VERIFY logic accumulates
         * a phantom error count and prints "Errors found" cosmetically
         * even though the data loaded correctly. Reference: Oricutron's
         * .pch tape patch + Atmos GetTapeByte disassembly. */
        if (emu->tapeoffs < emu->tapelen) {
            uint8_t byte = emu->tapebuf[emu->tapeoffs++];
            emu->cpu.A = byte;
            if (byte == 0) emu->cpu.P |= FLAG_ZERO;
            else           emu->cpu.P &= ~FLAG_ZERO;
            if (p->readbyte_setcarry) emu->cpu.P |=  FLAG_CARRY;
            else                      emu->cpu.P &= ~FLAG_CARRY;
            memory_write(&emu->memory, p->readbyte_store, byte);
            if (p->readbyte_storezero) {
                memory_write(&emu->memory, p->readbyte_storezero, 0x00);
            }
            emu->cpu.PC = p->readbyte_end;
            emu->tape_readbyte_active = true;
        }
        /* Tape exhausted: don't intercept — let the ROM bit-decoder time
         * out naturally. With $02B1/carry now correct, the silence handler
         * is no longer needed to avoid spurious "Errors found", and not
         * patching here means BASIC's CLOAD termination signals the end
         * of tape via its own logic instead of running on synthesised $00. */
    } else if (pc == p->getsync_loop) {
        /* Sync loop recovery */
        if (emu->tape_syncstack >= 0) {
            emu->cpu.SP = (uint8_t)emu->tape_syncstack;
            emu->tape_syncstack = -1;
            if (emu->tapebuf[emu->tapeoffs] != 0x16) {
                while (emu->tapeoffs < emu->tapelen &&
                       emu->tapebuf[emu->tapeoffs] != 0x16)
                    emu->tapeoffs++;
                if (emu->tapeoffs >= emu->tapelen) {
                    emu->tape_loaded = false;
                    return;
                }
            }
            emu->cpu.PC = p->getsync_end;
        }
    } else if (p->writefileheader_entry && pc == p->writefileheader_entry) {
        /* Sprint 34at : snapshot the header staging buffer and the
         * filename buffer at WriteFileHeader entry — before any data-write
         * loop reuses the staging ZP as a work pointer. On ORIC-1, $5F/$60
         * holds TXTTAB right now but will be advanced to VARTAB during the
         * data write, so reading it at csave_end would give the WRONG
         * start address. */
        if (p->csave_header_buf) {
            for (int i = 0; i < 9; i++) {
                emu->csave_header_snap[i] = emu->memory.ram[p->csave_header_buf + i];
            }
        }
        if (p->csave_filename_buf) {
            for (int i = 0; i < 16; i++) {
                emu->csave_fname_snap[i] = (char)emu->memory.ram[p->csave_filename_buf + i];
            }
            emu->csave_fname_snap[16] = 0;
        }
        emu->csave_snap_valid = true;
        /* Do NOT modify PC — let the ROM execute WriteFileHeader normally. */
    } else if (pc == p->writeleader_entry) {
        /* CSAVE: write tape leader — open output file if needed */
        if (!emu->csave_file) {
            /* Read filename from $0035 keeping only [A-Z0-9_-.] up to 11
             * chars. BASIC stores the name with surrounding quotes and
             * sometimes a length-prefix byte; the raw bytes are not
             * filesystem-safe. */
            char csave_name[16] = {0};
            int nlen = 0;
            for (int i = 0; i < 16 && nlen < 11; i++) {
                unsigned char ch = emu->memory.ram[0x0035 + i];
                if (ch == 0) break;
                if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
                if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                    ch == '_' || ch == '-' || ch == '.') {
                    csave_name[nlen++] = (char)ch;
                }
            }

            /* Build filename: name.tap (or csave_output.tap if empty) */
            char csave_path[64];
            if (nlen > 0) {
                snprintf(csave_path, sizeof(csave_path), "%s.tap", csave_name);
            } else {
                snprintf(csave_path, sizeof(csave_path), "csave_output.tap");
            }

            emu->csave_file = fopen(csave_path, "wb");
            if (emu->csave_file) {
                uint8_t leader[] = { 0x16, 0x16, 0x16 };
                fwrite(leader, 1, 3, emu->csave_file);
                emu->csave_byte_count = 0;
                emu->csave_snap_valid = false;  /* 34at : reset between CSAVEs */
                emu->csave_in_progress = true;  /* 34at : guard against shared-path re-entry */
                strncpy(emu->csave_last_path, csave_path,
                        sizeof(emu->csave_last_path) - 1);
                emu->csave_last_path[sizeof(emu->csave_last_path) - 1] = 0;
                log_info("CSAVE: saving to %s", csave_path);
            }
        } else {
            /* Subsequent leader (between header and data) */
            uint8_t leader[] = { 0x16, 0x16, 0x16 };
            fwrite(leader, 1, 3, emu->csave_file);
        }
        emu->cpu.PC = p->writeleader_end;
    } else if (pc == p->putbyte_entry) {
        /* CSAVE: putbyte is intercepted but we ignore the byte. The TAP
         * is rebuilt from RAM at csave_end (which produces a properly
         * structured Oric TAP, unlike the byte-stream produced here by
         * the BASIC ROM, which proved unreliable). */
        emu->csave_byte_count++;
        emu->cpu.PC = p->putbyte_end;
    } else if (pc == p->csave_end) {
        /* 34at : guard against re-entry on shared code paths (ORIC-1
         * $E80A is reached from CLOAD's exit too). */
        if (!emu->csave_in_progress) return;
        emu->csave_in_progress = false;
        /* CSAVE complete — rebuild the TAP (Sprint 34as).
         *
         * Sourcing priority :
         *  1. If p->csave_header_buf is set (Atmos), read the 9-byte
         *     header staging buffer the ROM populated before WriteFileHeader.
         *     This buffer is CSAVE-variant-agnostic : works for BASIC
         *     programs AND machine-code (`,A start,E end`) without any
         *     special-casing.
         *  2. Fallback (BASIC 1.0 for now) : TXTTAB/VARTAB pointers in
         *     zero-page. Works for BASIC programs only.
         *
         * The header buffer layout (Atmos, memory address → tape byte) :
         *   $02A8 → byte 9 (null sep)
         *   $02A9 → byte 8 (start_lo)
         *   $02AA → byte 7 (start_hi)
         *   $02AB → byte 6 (end_lo)
         *   $02AC → byte 5 (end_hi)
         *   $02AD → byte 4 (auto-flag, $C7)
         *   $02AE → byte 3 (type, $00=BASIC)
         *   $02AF → byte 2 (padding)
         *   $02B0 → byte 1 (padding)
         * Tape order is the buffer read in reverse (X=9 down to X=1).
         */
        if (emu->csave_file) {
            fclose(emu->csave_file);
            emu->csave_file = NULL;
            emu->csave_byte_count = 0;
        }

        uint16_t start_addr, end_addr;
        uint8_t  header_type, header_auto;
        /* Sprint 34at : prefer the snapshot captured at writefileheader_entry,
         * because data-write loops on ORIC-1 reuse $5F/$60 as a work pointer
         * and the live RAM no longer holds the original start address. */
        if (emu->csave_snap_valid) {
            /* Snapshot layout matches the live buffer indexing. */
            start_addr  = (uint16_t)(emu->csave_header_snap[1] |
                                     (emu->csave_header_snap[2] << 8));
            end_addr    = (uint16_t)(emu->csave_header_snap[3] |
                                     (emu->csave_header_snap[4] << 8));
            header_auto = emu->csave_header_snap[5];
            header_type = emu->csave_header_snap[6];
        } else if (p->csave_header_buf) {
            uint16_t b = p->csave_header_buf;
            start_addr  = (uint16_t)(emu->memory.ram[b + 1] |
                                     (emu->memory.ram[b + 2] << 8));
            end_addr    = (uint16_t)(emu->memory.ram[b + 3] |
                                     (emu->memory.ram[b + 4] << 8));
            header_auto = emu->memory.ram[b + 5];
            header_type = emu->memory.ram[b + 6];
        } else {
            /* Legacy fallback : TXTTAB / VARTAB. */
            start_addr =
                (uint16_t)(emu->memory.ram[0x9A] | (emu->memory.ram[0x9B] << 8));
            end_addr =
                (uint16_t)(emu->memory.ram[0x9C] | (emu->memory.ram[0x9D] << 8));
            /* VARTAB points to first byte AFTER program → subtract 1. */
            if (end_addr > start_addr) end_addr--;
            header_auto = 0xC7;
            header_type = 0x00;
        }
        int prog_len = (int)end_addr - (int)start_addr + 1;
        if (prog_len < 0) prog_len = 0;

        /* Sanitize the filename. Prefer snapshot if valid (consistent with
         * the header source). */
        char clean_name[12] = {0};
        int ci = 0;
        for (int i = 0; i < 16 && ci < 11; i++) {
            unsigned char c;
            if (emu->csave_snap_valid) {
                c = (unsigned char)emu->csave_fname_snap[i];
            } else {
                uint16_t fn_addr =
                    p->csave_filename_buf ? p->csave_filename_buf : 0x0035;
                c = emu->memory.ram[fn_addr + i];
            }
            if (c == 0) break;
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-' || c == '.') {
                clean_name[ci++] = (char)c;
            }
        }
        if (ci == 0) snprintf(clean_name, sizeof(clean_name), "CSAVE");

        /* Build the canonical TAP. Single data block — Atmos BASIC's
         * CLOAD verify still prints "Errors found" cosmetically even
         * when data loads correctly (parity counters set by tape
         * input mock), but the program is fully loaded and auto-runs. */
        int tap_cap = 4 /*leader+sync*/ + 9 /*header: 2 pad+type+auto+2 end+2 start+1 reserved*/ +
                      (int)strlen(clean_name) + 1 + prog_len;
        uint8_t* tap = (uint8_t*)malloc((size_t)tap_cap);
        if (tap) {
            int t = 0;
            tap[t++] = 0x16; tap[t++] = 0x16; tap[t++] = 0x16;
            tap[t++] = 0x24;
            tap[t++] = 0x00; tap[t++] = 0x00;                    /* 2 padding */
            tap[t++] = header_type;                              /* $00=BASIC, $80=array, $C0=string, ... */
            tap[t++] = header_auto;                              /* $C7 auto-run usually */
            tap[t++] = (uint8_t)(end_addr >> 8);
            tap[t++] = (uint8_t)(end_addr & 0xFF);
            tap[t++] = (uint8_t)(start_addr >> 8);
            tap[t++] = (uint8_t)(start_addr & 0xFF);
            tap[t++] = 0x00;                                     /* reserved */
            size_t nlen = strlen(clean_name);
            memcpy(tap + t, clean_name, nlen); t += (int)nlen;
            tap[t++] = 0x00;                                     /* name null */
            for (int i = 0; i < prog_len; i++) {
                tap[t++] = emu->memory.ram[start_addr + i];
            }

            /* Non-regression invariant (Sprint 58): the hand-computed tap_cap
             * MUST equal the number of bytes the tap[t++] sequence wrote. A
             * mismatch means the allocation and the writer have drifted apart
             * — exactly the +8/+9 heap overflow this guard exists to catch.
             * Kept always-on: the release build defines NDEBUG, which would
             * strip a bare assert(). Under ASan the overflow itself fires at
             * the offending write; here we flag the drift and refuse to emit
             * a corrupt TAP if t < tap_cap (or after-the-fact if t > tap_cap). */
            if (t != tap_cap) {
                log_error("CSAVE: TAP size invariant violated (wrote %d bytes, "
                          "allocated %d) — aborting TAP emission", t, tap_cap);
                free(tap);
                /* Mirror the normal-path cleanup: drop the stale snapshot so a
                 * later csave_end hit at the same PC does not rebuild from it. */
                emu->csave_snap_valid = false;
                return;
            }

            /* Overwrite the host file with the proper TAP. */
            FILE* fw = fopen(emu->csave_last_path, "wb");
            if (fw) {
                fwrite(tap, 1, (size_t)t, fw);
                fclose(fw);
                log_info("CSAVE: built TAP %s (%d bytes, prog $%04X-$%04X)",
                         emu->csave_last_path, t, start_addr, end_addr);
            }

            /* Re-buffer for in-session CLOAD. */
            if (emu->tapebuf) free(emu->tapebuf);
            emu->tapebuf = (uint8_t*)malloc((size_t)t);
            if (emu->tapebuf) {
                memcpy(emu->tapebuf, tap, (size_t)t);
                emu->tapelen = t;
                emu->tapeoffs = 0;
                emu->tape_loaded = true;
                emu->tape_syncstack = -1;
                log_info("CSAVE: re-buffered %d bytes for CLOAD", t);
            }

            /* Persist to SDIMG so the file survives a restart. */
            if (emu->has_loci && emu->loci.sdimg && t > 0) {
                char sd_name[16] = {0};
                int sci = 0;
                for (int i = 0; clean_name[i] && sci < 8 && clean_name[i] != '.'; i++) {
                    sd_name[sci++] = clean_name[i];
                }
                if (sci == 0) {
                    snprintf(sd_name, sizeof(sd_name), "CSAVE.TAP");
                } else {
                    sd_name[sci] = 0;
                    snprintf(sd_name + sci, sizeof(sd_name) - sci, ".TAP");
                }
                int fd = loci_sdimg_fopen_ex(
                    (loci_sdimg_t*)emu->loci.sdimg, sd_name, 1);
                if (fd >= 0) {
                    int written = 0;
                    while (written < t) {
                        int chunk = t - written;
                        if (chunk > 256) chunk = 256;
                        int bw = loci_sdimg_fwrite(
                            (loci_sdimg_t*)emu->loci.sdimg, fd,
                            tap + written, (uint16_t)chunk);
                        if (bw <= 0) break;
                        written += bw;
                    }
                    loci_sdimg_fclose((loci_sdimg_t*)emu->loci.sdimg, fd);
                    loci_sdimg_sync((loci_sdimg_t*)emu->loci.sdimg);
                    log_info("CSAVE: persisted %d bytes to SDIMG as %s",
                             written, sd_name);
                } else {
                    log_warning("CSAVE: SDIMG persist failed (errno=%d)", -fd);
                }
            }
            free(tap);
            /* 34at : invalidate snapshot so a subsequent csave_end hit that
             * shares the same PC (ORIC-1 $E80A is reached from CLOAD's exit
             * path too) does not re-rebuild from stale state. */
            emu->csave_snap_valid = false;
        } else {
            log_warning("CSAVE: OOM rebuilding TAP");
        }
    }
}
