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

/* open() flags — matching firmware constants (std.c). */
#define LOCI_O_RDWR    0x03
#define LOCI_O_CREAT   0x10
#define LOCI_O_TRUNC   0x20
#define LOCI_O_APPEND  0x40
#define LOCI_O_EXCL    0x80

/* File descriptor table — POSIX-mapped subset of LOCI's std_fil[]. */
#define LOCI_FD_MAX     16
#define LOCI_FD_OFFSET  3      /* fd 0/1/2 reserved (stdin/stdout/stderr) */

/* Path sandbox root — LOCI paths resolve relative to this directory.
 * Override at runtime via loci_set_flash_root(). */
#define LOCI_FLASH_DEFAULT "."

/* xram — 64 KB SRAM exposed to the 6502 through DMA windows. */
#define LOCI_XRAM_SIZE  0x10000

/* Dir handles (POSIX DIR*) — separate fd space from file fds. */
#define LOCI_DIR_MAX     8
#define LOCI_DIR_OFFSET  32   /* matches firmware FD_OFFS_LFS */

/* dirent struct laid out on xstack by readdir (must match firmware exactly).
 * sizeof = 2 + 64 + 1 + 1 + 4 = 72 bytes. */
#define LOCI_DIR_NAME_LEN  64
#define LOCI_DIRENT_SIZE   72

/* FAT-style attribute bits used by readdir.d_attrib. */
#define LOCI_AM_DIR  0x10
#define LOCI_AM_SYS  0x04

/* DSK WD179x I/O registers (Sprint 34ae). */
#define LOCI_DSK_IO_CMD     0x0310   /* status (read) / command (write) */
#define LOCI_DSK_IO_TRACK   0x0311
#define LOCI_DSK_IO_SECT    0x0312
#define LOCI_DSK_IO_DATA    0x0313
#define LOCI_DSK_IO_CTRL    0x0314
#define LOCI_DSK_IO_DRQ     0x0318

/* WD1793 status bits we report (subset). */
#define LOCI_DSK_STAT_NOT_READY  0x80
#define LOCI_DSK_STAT_WPROT      0x40
#define LOCI_DSK_STAT_RECORD_T   0x20
#define LOCI_DSK_STAT_LOST       0x04
#define LOCI_DSK_STAT_DRQ        0x02
#define LOCI_DSK_STAT_BUSY       0x01

/* DSK CTRL ($0314) bits matching real Microdisc / LOCI layout. */
#define LOCI_DSK_CTRL_DRV_SEL_SHIFT  5
#define LOCI_DSK_CTRL_DRV_SEL_MASK   (0x03u << LOCI_DSK_CTRL_DRV_SEL_SHIFT)

/* TAP cassette I/O registers (Sprint 34af). */
#define LOCI_TAP_IO_CMD   0x0315
#define LOCI_TAP_IO_STAT  0x0316
#define LOCI_TAP_IO_DATA  0x0317

/* TAP STAT register bits. */
#define LOCI_TAP_STAT_NOT_READY 0x80
#define LOCI_TAP_STAT_WPROT     0x40
#define LOCI_TAP_STAT_BUSY      0x01

/* TAP CMD values. */
#define LOCI_TAP_CMD_PLAY      0x01
#define LOCI_TAP_CMD_REC       0x02
#define LOCI_TAP_CMD_REW       0x03
#define LOCI_TAP_CMD_READ_BIT  0x04
#define LOCI_TAP_CMD_FFW       0x05

/* TAP header layout (16 bytes, matches firmware tap_header_t). */
#define LOCI_TAP_HEADER_SIZE  16

/* MIA_BOOT (op 0xA0) bit flags — matches firmware sys/mia.h. */
#define LOCI_BOOT_FDC      0x01   /* Mount Microdisc device ROM at $A000 */
#define LOCI_BOOT_TAP      0x02   /* Load tape image */
#define LOCI_BOOT_B11      0x04   /* Use BASIC 1.1 (Atmos) instead of 1.0 */
#define LOCI_BOOT_TAP_BIT  0x08   /* TAP bit-level loading */
#define LOCI_BOOT_TAP_ALD  0x10   /* Tape autoload */
#define LOCI_BOOT_RESUME   0x40   /* Resume current ROM instead of swap */
#define LOCI_BOOT_FAST     0x80   /* Fast boot (skip leader) */

/* Mount slots — 4 disk drives + 1 tape + 1 ROM. */
#define LOCI_MNT_MAX    6
#define LOCI_MNT_TAP    4
#define LOCI_MNT_ROM    5

/* Additional MIA register offsets used for the DMA windows. */
#define LOCI_REG_ADDR0_LO 0x06   /* $03A6 */
#define LOCI_REG_ADDR0_HI 0x07   /* $03A7 */
#define LOCI_REG_ADDR1_LO 0x0A   /* $03AA */
#define LOCI_REG_ADDR1_HI 0x0B   /* $03AB */

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

    /* Host-fs file backend (Sprint 34aa).
     * fds[i] is the FILE* for fd = LOCI_FD_OFFSET + i (3..18). NULL = closed. */
    void* fds[LOCI_FD_MAX];

    /* Sandbox root directory — paths from the 6502 are resolved here.
     * NULL = use current working directory. */
    char flash_root[256];

    /* xram — 64 KB shared SRAM accessed by the 6502 via the DMA windows
     * at $03A4 (data window 0) and $03A8 (data window 1).
     * The corresponding addresses live in $03A6-A7 and $03AA-AB, with
     * signed step in $03A5/$03A9. */
    uint8_t xram[LOCI_XRAM_SIZE];

    /* Mount table : 4 disk drives + 1 tape + 1 ROM. Sprint 34ab tracks
     * paths only — actual disk/tape/ROM image plumbing lands in 34ad-34af. */
    bool mnt_mounted[LOCI_MNT_MAX];
    char mnt_paths[LOCI_MNT_MAX][256];

    /* Directory iterators (Sprint 34ac). dirs[i] is a host DIR* exposed
     * to the 6502 as dir_fd = LOCI_DIR_OFFSET + i. */
    void* dirs[LOCI_DIR_MAX];

    /* USB HID xram addresses (Sprint 34ag) — set by the 6502 via PIX_XREG
     * (device=0=MIA, channel=0, addr=0/1/2 for kbd/mou/pad). 0xFFFF = unset.
     * When set, the corresponding HID state bitmap is mirrored into
     * xram[kbd_xram..kbd_xram+31] (kbd is 32 bytes), etc. */
    uint16_t kbd_xram;
    uint16_t mou_xram;
    uint16_t pad_xram;

    /* MIA_BOOT settings latched on last 0xA0 call (Sprint 34ad). */
    uint8_t boot_settings;

    /* TAP cassette backend (Sprint 34af). Auto-opened by op_mount when
     * drive == LOCI_MNT_TAP; closed by op_umount. */
    void*    tap_fp;       /* FILE* host */
    uint32_t tap_size;     /* total bytes in mounted file */
    uint32_t tap_counter;  /* current read offset */
    uint8_t  tap_cmd;      /* last value written to $0315 (CMD) */
    uint8_t  tap_stat;     /* mirrored to $0316 (STAT) */

    /* DSK multi-drive backend (Sprint 34ae).
     * Each of slots 0-3 may have an open host file. The bus interface is
     * a minimal WD1793 stub: report idle/no-DRQ, accept commands silently. */
    void*    dsk_fp[4];        /* FILE* per drive (NULL = unmounted) */
    uint8_t  dsk_selected;     /* drive selected by last $0314 CTRL write (0..3) */
    uint8_t  dsk_status;       /* what $0310 read returns */
    uint8_t  dsk_track;        /* $0311 */
    uint8_t  dsk_sect;         /* $0312 */
    uint8_t  dsk_data;         /* $0313 */
    uint8_t  dsk_ctrl;         /* $0314 — last write */
    uint8_t  dsk_drq;          /* $0318 — current DRQ flag byte */

    /* ROM-swap callback (Sprint 34ad).
     * Set by the emulator via loci_set_rom_swap_callback(). Invoked by
     * op 0xA0 MIA_BOOT to load the requested ROM image into Oric memory
     * and reset the CPU. Returns true on success.
     *
     * Args: ctx is the registered opaque (typically emulator_t*),
     *       rom_path is a host-resolved path (already sandboxed),
     *       base_addr is the Oric memory address to load at ($C000
     *       for BASIC, $A000 for Microdisc). */
    bool (*rom_swap_cb)(void* ctx, const char* rom_path, uint16_t base_addr);
    void* rom_swap_ctx;
} loci_t;

bool    loci_init(loci_t* loci);
void    loci_reset(loci_t* loci);
void    loci_cleanup(loci_t* loci);

/* Configure the host directory used as sandbox root for LOCI file ops
 * (Sprint 34aa). Pass NULL or empty string to use CWD. */
void    loci_set_flash_root(loci_t* loci, const char* path);

/* USB HID bridge (Sprint 34ag).
 * Update the keyboard bitmap in xram from a HID-style boot report.
 * `modifier` mirrors the HID modifier byte (LCtrl=0x01, LShift=0x02, ...).
 * `keycodes` is a 6-slot array of HID usage codes (0 = empty slot).
 * Has no effect if the 6502 has not yet pointed kbd_xram at a region. */
void    loci_kbd_set_report(loci_t* loci, uint8_t modifier,
                            const uint8_t keycodes[6]);

/* Clear the keyboard bitmap (all keys released). */
void    loci_kbd_clear(loci_t* loci);

/* Register the ROM-swap callback used by op 0xA0 MIA_BOOT. */
void    loci_set_rom_swap_callback(
            loci_t* loci,
            bool (*cb)(void* ctx, const char* rom_path, uint16_t base_addr),
            void* ctx);

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

/* TAP cassette register range $0315-$0317 (Sprint 34af). */
static inline bool loci_addr_in_tap(uint16_t address) {
    return address >= LOCI_TAP_IO_CMD && address <= LOCI_TAP_IO_DATA;
}

/* DSK WD179x register range $0310-$0314 + $0318 (Sprint 34ae). */
static inline bool loci_addr_in_dsk(uint16_t address) {
    return (address >= LOCI_DSK_IO_CMD && address <= LOCI_DSK_IO_CTRL) ||
           (address == LOCI_DSK_IO_DRQ);
}

/* TAP register access (used by the main I/O router). */
uint8_t loci_tap_read(loci_t* loci, uint16_t address);
void    loci_tap_write(loci_t* loci, uint16_t address, uint8_t value);

/* DSK register access — stub WD1793 (Sprint 34ae). */
uint8_t loci_dsk_read(loci_t* loci, uint16_t address);
void    loci_dsk_write(loci_t* loci, uint16_t address, uint8_t value);

/* Run the pending API op (called by the main loop after every CPU step
 * when LOCI is enabled). For Sprint 34y all ops return ENOSYS synchronously,
 * so this is a no-op in practice — the dispatch happens inline in
 * loci_write() when API_OP is written. The hook is kept for future async
 * handlers. */
void    loci_task(loci_t* loci);

#endif /* IO_LOCI_H */
