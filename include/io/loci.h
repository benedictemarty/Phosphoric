/**
 * @file loci.h
 * @brief LOCI (Lovely Oric Computer Interface) emulation — Sprint 34y skeleton
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 *
 * Emulates the LOCI bus peripheral by sodiumlb (2024). Phase 1 — Sprint 34y —
 * provides only the MIA register file at $03A0-$03BF and the API op dispatcher
 * with all 30+ ops stubbed to return ENOSYS. Subsequent sprints implement the
 * real handlers.
 *
 * Reference firmware: github.com/sodiumlb/loci-firmware
 */

#ifndef IO_LOCI_H
#define IO_LOCI_H

#include <stdint.h>
#include <stdbool.h>

/* MIA bus range — the API surface lives in this 32-byte window. */
#define LOCI_MIA_BASE   0x03A0
#define LOCI_MIA_END    0x03BF
#define LOCI_MIA_SIZE   32

/* Selected register offsets within the window (firmware-truthful names). */
#define LOCI_REG_CONS_FLAGS   0x00   /* $03A0 — bit 7 = data avail, bit 6 = VSYNC */
#define LOCI_REG_CONS_CHAR    0x02   /* $03A2 — console input character */
#define LOCI_REG_RW0          0x04   /* $03A4 — DMA window 0 data */
#define LOCI_REG_STEP0        0x05   /* $03A5 — DMA window 0 step (signed) */
#define LOCI_REG_RW1          0x08   /* $03A8 — DMA window 1 data */
#define LOCI_REG_STEP1        0x09   /* $03A9 — DMA window 1 step (signed) */
#define LOCI_REG_API_STACK    0x0C   /* $03AC — xstack pointer */
#define LOCI_REG_API_ERRNO_LO 0x0D   /* $03AD — errno low byte */
#define LOCI_REG_API_ERRNO_HI 0x0E   /* $03AE — errno high byte */
#define LOCI_REG_API_OP       0x0F   /* $03AF — API operation trigger */
#define LOCI_REG_INJECT0      0x10   /* $03B0 — 6502 injection slot byte 0 */
#define LOCI_REG_BUSY         0x12   /* $03B2 — bit 7 = busy */
#define LOCI_REG_API_A        0x14   /* $03B4 — return A */
#define LOCI_REG_API_X        0x16   /* $03B6 — return X */
#define LOCI_REG_API_SREG     0x18   /* $03B8 — return SREG (16-bit lo) */
#define LOCI_REG_API_SREG_HI  0x19   /* $03B9 — return SREG (high byte) */

/* API operations — written to $03AF by the 6502 to request a service. */
typedef enum {
    LOCI_OP_NONE             = 0x00,
    LOCI_OP_PIX_XREG         = 0x01,
    LOCI_OP_CPU_PHI2         = 0x02,
    LOCI_OP_OEM_CODEPAGE     = 0x03,
    LOCI_OP_RNG_LRAND        = 0x04,
    LOCI_OP_STDIN_OPT        = 0x05,
    LOCI_OP_CLOCK            = 0x0F,
    LOCI_OP_CLK_GETRES       = 0x10,
    LOCI_OP_CLK_GETTIME      = 0x11,
    LOCI_OP_CLK_SETTIME      = 0x12,
    LOCI_OP_OPEN             = 0x14,
    LOCI_OP_CLOSE            = 0x15,
    LOCI_OP_READ_XSTACK      = 0x16,
    LOCI_OP_READ_XRAM        = 0x17,
    LOCI_OP_WRITE_XSTACK     = 0x18,
    LOCI_OP_WRITE_XRAM       = 0x19,
    LOCI_OP_LSEEK            = 0x1A,
    LOCI_OP_UNLINK           = 0x1B,
    LOCI_OP_RENAME           = 0x1C,
    LOCI_OP_OPENDIR          = 0x80,
    LOCI_OP_CLOSEDIR         = 0x81,
    LOCI_OP_READDIR          = 0x82,
    LOCI_OP_MKDIR            = 0x83,
    LOCI_OP_GETCWD           = 0x88,
    LOCI_OP_MOUNT            = 0x90,
    LOCI_OP_UMOUNT           = 0x91,
    LOCI_OP_TAP_SEEK         = 0x92,
    LOCI_OP_TAP_TELL         = 0x93,
    LOCI_OP_TAP_READ_HEADER  = 0x94,
    LOCI_OP_UNAME            = 0x98,
    LOCI_OP_MIA_BOOT         = 0xA0,
    LOCI_OP_MAP_TUNE_TMAP    = 0xA1,
    LOCI_OP_MAP_TUNE_TIOR    = 0xA2,
    LOCI_OP_MAP_TUNE_TIOW    = 0xA3,
    LOCI_OP_MAP_TUNE_TIOD    = 0xA4,
    LOCI_OP_MAP_TUNE_TADR    = 0xA5,
    LOCI_OP_RESET_SENTINEL   = 0xFF
} loci_op_t;

/* errno values mirroring the firmware (sys/errno-style). */
#define LOCI_ENOENT  1
#define LOCI_ENOMEM  2
#define LOCI_EACCES  3
#define LOCI_ENODEV  4
#define LOCI_EMFILE  5
#define LOCI_EBUSY   6
#define LOCI_EINVAL  7
#define LOCI_ENOSPC  8
#define LOCI_EEXIST  9
#define LOCI_EAGAIN  10
#define LOCI_EIO     11
#define LOCI_EINTR   12
#define LOCI_ENOSYS  13
#define LOCI_ESPIPE  14
#define LOCI_ERANGE  15
#define LOCI_EBADF   16
#define LOCI_ENOEXEC 17

#define LOCI_XSTACK_SIZE 256

typedef struct loci_s {
    bool enabled;

    /* MIA register file ($03A0-$03BF mirror). Byte-addressed. */
    uint8_t regs[LOCI_MIA_SIZE];

    /* xstack — argument buffer accessed by the 6502 through API_STACK.
     * Grows downward; top of stack is at xstack_ptr (LOCI_XSTACK_SIZE = empty).
     * Firmware semantics:
     *   - 6502 writes $03AC : push (xstack_ptr-=1; xstack[ptr]=value)
     *   - 6502 reads  $03AC : pop  (value=xstack[ptr]; xstack_ptr+=1)
     *   - api_zxstack() resets ptr to top (cleared between ops). */
    uint8_t  xstack[LOCI_XSTACK_SIZE];
    uint16_t xstack_ptr;

    /* The op being processed. 0 = idle. */
    uint8_t active_op;

    /* Aggregated counters (debug / stats). */
    uint64_t op_count[256];

    /* Wallclock start — used by clk_api_clock to report uptime.
     * Stored as Unix epoch microseconds at the moment of loci_init(). */
    uint64_t clock_start_us;

    /* Simple LCG state for deterministic RNG when needed (currently we
     * use rand() for variety). Reserved for future deterministic mode. */
    uint64_t rng_state;
} loci_t;

bool    loci_init(loci_t* loci);
void    loci_reset(loci_t* loci);
void    loci_cleanup(loci_t* loci);

/* Bus interface — called from the memory I/O callback when an address
 * lies inside the MIA window. Out-of-range addresses must be filtered
 * by the caller. */
uint8_t loci_read(loci_t* loci, uint16_t address);
void    loci_write(loci_t* loci, uint16_t address, uint8_t value);

/* Convenience: returns true if the given bus address is part of the
 * MIA window (used by the main I/O router). */
static inline bool loci_addr_in_mia(uint16_t address) {
    return address >= LOCI_MIA_BASE && address <= LOCI_MIA_END;
}

/* Run the pending API op (called by the main loop after every CPU step
 * when LOCI is enabled). For Sprint 34y all ops return ENOSYS synchronously,
 * so this is a no-op in practice — the dispatch happens inline in
 * loci_write() when API_OP is written. The hook is kept for future async
 * handlers. */
void    loci_task(loci_t* loci);

#endif /* IO_LOCI_H */
