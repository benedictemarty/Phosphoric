/**
 * @file ocula_gpu.c
 * @brief OCULA-GPU command window ($03E8-$03EF) — étape 5
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-13
 * @version 1.21.0-alpha
 *
 * Emulation note: commands execute instantly from the 6502's point of
 * view (the emulator is single-threaded), so posted commands complete
 * before the next instruction and GPU_STATUS is only ever observed
 * busy for WAIT_VBL. Real hardware may be slower: well-behaved software
 * must always poll GPU_STATUS, as the spec mandates.
 */

#include "io/ocula_gpu.h"
#include <string.h>

void ocula_gpu_init(ocula_gpu_t* gpu) {
    memset(gpu, 0, sizeof(ocula_gpu_t));
}

uint8_t ocula_gpu_read(const ocula_gpu_t* gpu, uint16_t address) {
    switch (address) {
        case OCULA_GPU_STATUS: return gpu->status;
        case OCULA_GPU_PTRL:   return (uint8_t)(gpu->arg_ptr & 0xFF);
        case OCULA_GPU_PTRH:   return (uint8_t)(gpu->arg_ptr >> 8);
        default:               return 0x00;  /* CMD write-only + reserved */
    }
}

/* PAL frame geometry — must mirror emulator.h (PAL_CYCLES_PER_LINE,
 * PAL_LINES_PER_FRAME, VSYNC_START_LINE). Kept local so this unit stays
 * decoupled from the emulator struct and headless-testable. The active text
 * area is ~40 of the 64 cycles per line; the rest is horizontal blanking. */
#define RASTER_CYCLES_PER_LINE 64
#define RASTER_LINES_PER_FRAME 312
#define RASTER_VBLANK_LINE     256
#define RASTER_HVISIBLE_CYCLES 40

static int raster_line(int frame_cycles) {
    if (frame_cycles < 0) frame_cycles = 0;
    int line = frame_cycles / RASTER_CYCLES_PER_LINE;
    /* Defensive wrap: callers pass 0..19967, but never overrun the table. */
    return line % RASTER_LINES_PER_FRAME;
}

uint8_t ocula_raster_lo(int frame_cycles) {
    return (uint8_t)(raster_line(frame_cycles) & 0xFF);
}

uint8_t ocula_raster_status(int frame_cycles) {
    if (frame_cycles < 0) frame_cycles = 0;
    int line = raster_line(frame_cycles);
    int col  = frame_cycles % RASTER_CYCLES_PER_LINE;
    uint8_t s = 0;
    if (line & 0x100)                 s |= OCULA_RASTER_LINE8;
    if (line >= RASTER_VBLANK_LINE)   s |= OCULA_RASTER_VBLANK;
    if (col  >= RASTER_HVISIBLE_CYCLES) s |= OCULA_RASTER_HBLANK;
    return s;
}

/* Guarded RAM access for GPU operations: addresses below $0400 (zero
 * page, stack, system variables, I/O page) are off limits — reading
 * the I/O page from the GPU could trigger VIA/FDC side effects. */
static uint8_t gpu_read8(memory_t* mem, uint16_t addr) {
    if (addr < 0x0400) return 0x00;
    return memory_read(mem, addr);
}

static void gpu_write8(memory_t* mem, uint16_t addr, uint8_t val) {
    if (addr < 0x0400) return;
    memory_write(mem, addr, val);  /* ROM-area writes are ignored upstream */
}

static void gpu_exec(ocula_gpu_t* gpu, memory_t* mem, video_t* vid,
                     uint8_t opcode) {
    bool blocking = (opcode & 0x80) != 0;
    uint8_t op = opcode & 0x7F;

    gpu->status = OCULA_GPU_ST_BUSY;

    if (gpu->arg_ptr < 0x0400 || gpu->arg_ptr > 0xFFF0) {
        gpu->status = OCULA_GPU_ERR_BADADDR;
        return;
    }

    /* Snapshot the 16-byte argument block (it is reusable immediately
     * after the command triggers, per spec). Goes through memory_read:
     * $A000-$BFFF arguments live in the active CPU bank (étape 4). */
    uint8_t a[16];
    for (int i = 0; i < 16; i++)
        a[i] = memory_read(mem, (uint16_t)(gpu->arg_ptr + i));

    switch (op) {
        case OCULA_GPU_OP_INFO: {
            gpu_write8(mem, gpu->arg_ptr,     OCULA_GPU_INFO_VERSION);
            gpu_write8(mem, gpu->arg_ptr + 1, OCULA_GPU_INFO_SPRITES);
            gpu_write8(mem, gpu->arg_ptr + 2, OCULA_GPU_INFO_OPMASK);
            for (int i = 3; i < 16; i++)
                gpu_write8(mem, (uint16_t)(gpu->arg_ptr + i), 0x00);
            gpu->status = OCULA_GPU_ST_READY;
            break;
        }
        case OCULA_GPU_OP_FILL: {
            uint16_t dst = (uint16_t)(a[0] | (a[1] << 8));
            uint8_t stride = a[2], w = a[3], h = a[4], val = a[5];
            if (dst < 0x0400) { gpu->status = OCULA_GPU_ERR_BADADDR; return; }
            for (int r = 0; r < h; r++)
                for (int c = 0; c < w; c++)
                    gpu_write8(mem, (uint16_t)(dst + r * stride + c), val);
            gpu->status = OCULA_GPU_ST_READY;
            break;
        }
        case OCULA_GPU_OP_COPY: {
            uint16_t src = (uint16_t)(a[0] | (a[1] << 8));
            uint8_t sstr = a[2];
            uint16_t dst = (uint16_t)(a[3] | (a[4] << 8));
            uint8_t dstr = a[5], w = a[6], h = a[7];
            if (src < 0x0400 || dst < 0x0400) {
                gpu->status = OCULA_GPU_ERR_BADADDR;
                return;
            }
            /* Full snapshot before writing: overlap-safe in any
             * direction (max 255x255 = ~64KB). */
            static uint8_t tmp[255 * 255];
            for (int r = 0; r < h; r++)
                for (int c = 0; c < w; c++)
                    tmp[r * w + c] = gpu_read8(mem, (uint16_t)(src + r * sstr + c));
            for (int r = 0; r < h; r++)
                for (int c = 0; c < w; c++)
                    gpu_write8(mem, (uint16_t)(dst + r * dstr + c), tmp[r * w + c]);
            gpu->status = OCULA_GPU_ST_READY;
            break;
        }
        case OCULA_GPU_OP_SCROLL: {
            vid->ocula_scroll_x = a[0];
            vid->ocula_scroll_y = a[1];
            vid->need_refresh = true;
            gpu->status = OCULA_GPU_ST_READY;
            break;
        }
        case OCULA_GPU_OP_WAIT_VBL: {
            if (!blocking) {
                gpu->status = OCULA_GPU_ERR_NOBLOCK;
                return;
            }
            /* Stays busy; the main loop stretches PHI0 and clears the
             * flag (and status) when the beam enters vertical blanking. */
            gpu->wait_vbl = true;
            break;
        }
        default:
            gpu->status = OCULA_GPU_ERR_BADOP;
            break;
    }
}

void ocula_gpu_write(ocula_gpu_t* gpu, memory_t* mem, video_t* vid,
                     uint16_t address, uint8_t value) {
    switch (address) {
        case OCULA_GPU_CMD:
            gpu_exec(gpu, mem, vid, value);
            break;
        case OCULA_GPU_PTRL:
            gpu->arg_ptr = (uint16_t)((gpu->arg_ptr & 0xFF00) | value);
            break;
        case OCULA_GPU_PTRH:
            gpu->arg_ptr = (uint16_t)((gpu->arg_ptr & 0x00FF) | (value << 8));
            break;
        default:
            break;  /* STATUS read-only, reserved ignored */
    }
}
