/**
 * @file ocula_gpu.h
 * @brief OCULA-GPU command window ($03E8-$03EF) — étape 5
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-13
 * @version 1.21.0-alpha
 *
 * Spec: docs/ocula_extensions.md (v0.6, étape 5). Tier 2: programs MUST
 * probe $03E0/$03E1 ('O','C') and bit 4 of $03E2 before using the GPU.
 *
 * Hardware rationale: the RP2350B has two Cortex-M33 cores — core 1
 * does the DVI scanout, core 0 is free to execute commands against the
 * internal SRAM (OCULA-is-the-RAM) without stealing 6502 bus cycles.
 * Blocking variants stretch PHI0 (OCULA generates the system clock).
 *
 * Registers:
 *   $03E8 (1000) W : GPU_CMD — opcode, write triggers execution.
 *                    Bit 7 = blocking variant (PHI0 stretched).
 *   $03E9 (1001) R : GPU_STATUS — $00 ready, $01 busy, >=$80 error
 *   $03EA (1002) RW: GPU_PTR low  — 16-byte argument block address
 *   $03EB (1003) RW: GPU_PTR high
 *   $03EC-$03EF    : reserved v2 (read $00)
 *
 * Commands v1: $01 INFO, $02 FILL, $03 COPY, $04 SCROLL,
 * $05 WAIT_VBL (blocking-only: $85).
 */

#ifndef OCULA_GPU_H
#define OCULA_GPU_H

#include <stdint.h>
#include <stdbool.h>
#include "memory/memory.h"
#include "video/video.h"

#define OCULA_GPU_BASE   0x03E8
#define OCULA_GPU_END    0x03EF
#define OCULA_GPU_CMD    0x03E8
#define OCULA_GPU_STATUS 0x03E9
#define OCULA_GPU_PTRL   0x03EA
#define OCULA_GPU_PTRH   0x03EB

/* Raster sync (Sprint 76) — read-only beam-position registers, so a program
 * can time palette/border writes to the scan (copper bars) without flicker.
 * Requested by Dbug (forum t=2709, p=34925: "VSync/HSync absolutely
 * necessary... don't flash crazily"). Computed live from the frame cycle
 * counter; PAL: 312 lines x 64 cycles, vertical blank from line 256. */
#define OCULA_GPU_RASTER_LO     0x03EC  /* R: current scanline & 0xFF (0..311) */
#define OCULA_GPU_RASTER_STATUS 0x03ED  /* R: beam status flags (below) */
/* RASTER_STATUS bits */
#define OCULA_RASTER_LINE8   0x01  /* bit 8 of the scanline (line >= 256) */
#define OCULA_RASTER_VBLANK  0x02  /* in vertical blank (line >= 256) */
#define OCULA_RASTER_HBLANK  0x04  /* in horizontal blank (late in the line) */

/* Opcodes (bit 7 = blocking variant) */
#define OCULA_GPU_OP_INFO     0x01
#define OCULA_GPU_OP_FILL     0x02
#define OCULA_GPU_OP_COPY     0x03
#define OCULA_GPU_OP_SCROLL   0x04
#define OCULA_GPU_OP_WAIT_VBL 0x05

/* Status codes */
#define OCULA_GPU_ST_READY    0x00
#define OCULA_GPU_ST_BUSY     0x01
#define OCULA_GPU_ERR_BADOP   0x80  /* unknown opcode */
#define OCULA_GPU_ERR_BADADDR 0x81  /* arg block / target below $0400 */
#define OCULA_GPU_ERR_NOBLOCK 0x82  /* WAIT_VBL requires the blocking form */

/* INFO response layout (written into the argument block) */
#define OCULA_GPU_INFO_VERSION 1     /* byte 0 */
#define OCULA_GPU_INFO_SPRITES 0     /* byte 1: none in v1 */
#define OCULA_GPU_INFO_OPMASK  0x1F  /* byte 2: ops $01-$05 supported */

typedef struct ocula_gpu_s {
    uint8_t status;     /* GPU_STATUS register */
    uint16_t arg_ptr;   /* GPU_PTR register */
    bool wait_vbl;      /* WAIT_VBL pending: main loop stretches PHI0
                         * until the beam enters vertical blanking */
} ocula_gpu_t;

void ocula_gpu_init(ocula_gpu_t* gpu);

static inline bool ocula_gpu_addr_in_window(uint16_t address) {
    return address >= OCULA_GPU_BASE && address <= OCULA_GPU_END;
}

uint8_t ocula_gpu_read(const ocula_gpu_t* gpu, uint16_t address);
void ocula_gpu_write(ocula_gpu_t* gpu, memory_t* mem, video_t* vid,
                     uint16_t address, uint8_t value);

/* Raster-sync register values, derived from the PAL frame cycle counter
 * (emulator_t.frame_cycles, 0..19967). Pure + headless-testable. */
uint8_t ocula_raster_lo(int frame_cycles);
uint8_t ocula_raster_status(int frame_cycles);

#endif /* OCULA_GPU_H */
