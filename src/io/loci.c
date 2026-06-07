/**
 * @file loci.c
 * @brief LOCI emulation — skeleton + dispatcher (Sprint 34y)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 *
 * Phase 1 implementation: MIA register file + API op dispatcher.
 * All API operations return LOCI_ENOSYS synchronously — subsequent sprints
 * implement the real handlers.
 *
 * Bus address layout (real LOCI hardware, extracted from sodiumlb/loci-firmware):
 *
 *   $03A0  CONS_FLAGS    bit 7 = console data avail, bit 6 = VSYNC ack
 *   $03A2  CONS_CHAR     console input character latch
 *   $03A4  RW0           DMA window 0 data byte
 *   $03A5  STEP0         DMA window 0 increment (signed)
 *   $03A8  RW1           DMA window 1 data byte
 *   $03A9  STEP1         DMA window 1 increment (signed)
 *   $03AC  API_STACK     xstack pointer (writes pop, reads push)
 *   $03AD  API_ERRNO_LO  16-bit errno
 *   $03AE  API_ERRNO_HI
 *   $03AF  API_OP        write triggers dispatch
 *   $03B0..$03B9         6502 injection slot (used by MIA for RAM bursts)
 *   $03B2.7              BUSY flag
 *   $03B4  API_A         return A
 *   $03B6  API_X         return X
 *   $03B8  API_SREG_LO   return SREG lo
 *   $03B9  API_SREG_HI   return SREG hi
 */

/* _XOPEN_SOURCE 500 exposes mkstemp() prototypes (Sprint 34ar). */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif

#include "io/loci.h"
#include "io/loci_sdimg.h"
#include "storage/sedoric.h"   /* sedoric_load — MFM_DISK header parser (34aw+) */
#include "utils/logging.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

/* ─── name lookup for diagnostics ───────────────────────────────── */

static const char* op_name(uint8_t op) {
    switch (op) {
        case LOCI_OP_PIX_XREG:        return "PIX_XREG";
        case LOCI_OP_CPU_PHI2:        return "CPU_PHI2";
        case LOCI_OP_OEM_CODEPAGE:    return "OEM_CODEPAGE";
        case LOCI_OP_RNG_LRAND:       return "RNG_LRAND";
        case LOCI_OP_STDIN_OPT:       return "STDIN_OPT";
        case LOCI_OP_CLOCK:           return "CLOCK";
        case LOCI_OP_CLK_GETRES:      return "CLK_GETRES";
        case LOCI_OP_CLK_GETTIME:     return "CLK_GETTIME";
        case LOCI_OP_CLK_SETTIME:     return "CLK_SETTIME";
        case LOCI_OP_OPEN:            return "OPEN";
        case LOCI_OP_CLOSE:           return "CLOSE";
        case LOCI_OP_READ_XSTACK:     return "READ_XSTACK";
        case LOCI_OP_READ_XRAM:       return "READ_XRAM";
        case LOCI_OP_WRITE_XSTACK:    return "WRITE_XSTACK";
        case LOCI_OP_WRITE_XRAM:      return "WRITE_XRAM";
        case LOCI_OP_LSEEK:           return "LSEEK";
        case LOCI_OP_UNLINK:          return "UNLINK";
        case LOCI_OP_RENAME:          return "RENAME";
        case LOCI_OP_OPENDIR:         return "OPENDIR";
        case LOCI_OP_CLOSEDIR:        return "CLOSEDIR";
        case LOCI_OP_READDIR:         return "READDIR";
        case LOCI_OP_MKDIR:           return "MKDIR";
        case LOCI_OP_GETCWD:          return "GETCWD";
        case LOCI_OP_MOUNT:           return "MOUNT";
        case LOCI_OP_UMOUNT:          return "UMOUNT";
        case LOCI_OP_TAP_SEEK:        return "TAP_SEEK";
        case LOCI_OP_TAP_TELL:        return "TAP_TELL";
        case LOCI_OP_TAP_READ_HEADER: return "TAP_READ_HEADER";
        case LOCI_OP_UNAME:           return "UNAME";
        case LOCI_OP_MIA_BOOT:        return "MIA_BOOT";
        case LOCI_OP_MAP_TUNE_TMAP:   return "MAP_TUNE_TMAP";
        case LOCI_OP_MAP_TUNE_TIOR:   return "MAP_TUNE_TIOR";
        case LOCI_OP_MAP_TUNE_TIOW:   return "MAP_TUNE_TIOW";
        case LOCI_OP_MAP_TUNE_TIOD:   return "MAP_TUNE_TIOD";
        case LOCI_OP_MAP_TUNE_TADR:   return "MAP_TUNE_TADR";
        default:                      return "?";
    }
}

/* Forward decls for helpers used across sections. */
static bool tap_open(loci_t* loci, const char* host_path);
static void tap_close(loci_t* loci);
static bool dsk_open(loci_t* loci, uint8_t drive, const char* host_path);
static void dsk_close(loci_t* loci, uint8_t drive);

/* ─── clock helpers ────────────────────────────────────────────── */

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ─── lifecycle ────────────────────────────────────────────────── */

/* Seed the MIA spin window ($03B0-$03B7) with a no-op RELEASED stub
 * that returns A=X=SREG=0. Matches the firmware's api_run() entry which
 * also calls api_return_errno(0) → released stub with A=X=$FF, ERRNO=0.
 * Phosphoric uses A=X=0 for the boot state since no op has run yet
 * and any pre-op `JSR $03B0` from probe code should return cleanly. */
static void seed_initial_stub(loci_t* loci) {
    /* CLV / BVC +0 / LDA #$00 / LDX #$00 / RTS — falls through and returns. */
    loci->regs[LOCI_REG_INJECT0 + 0] = 0xB8;
    loci->regs[LOCI_REG_INJECT0 + 1] = 0x50;
    loci->regs[LOCI_REG_INJECT0 + 2] = 0x00;
    loci->regs[LOCI_REG_INJECT0 + 3] = 0xA9;
    loci->regs[LOCI_REG_INJECT0 + 4] = 0x00;   /* A immediate */
    loci->regs[LOCI_REG_INJECT0 + 5] = 0xA2;
    loci->regs[LOCI_REG_INJECT0 + 6] = 0x00;   /* X immediate */
    loci->regs[LOCI_REG_INJECT0 + 7] = 0x60;
    loci->regs[LOCI_REG_INJECT0 + 8] = 0x00;   /* SREG lo */
    loci->regs[LOCI_REG_INJECT0 + 9] = 0x00;   /* SREG hi */
}

/* Forward decls — defined further down. */
static void loci_fdc_set_drq(void* userdata);
static void loci_fdc_clr_drq(void* userdata);
static void loci_fdc_set_intrq(void* userdata);
static void loci_fdc_clr_intrq(void* userdata);

bool loci_init(loci_t* loci) {
    if (!loci) return false;
    memset(loci, 0, sizeof(*loci));
    loci->xstack_ptr = LOCI_XSTACK_SIZE;   /* empty */
    loci->clock_start_us = now_us();
    loci->rng_state = loci->clock_start_us ^ 0xA5A5A5A5A5A5A5A5ULL;
    /* HID xram addresses unset until PIX_XREG configures them. */
    loci->kbd_xram = 0xFFFF;
    loci->mou_xram = 0xFFFF;
    loci->pad_xram = 0xFFFF;
    seed_initial_stub(loci);
    /* Sprint 34aw : init the WD1793 backing the 4 DSK drives. Default
     * geometry matches Oric DSK convention (41 tracks SS, 17 sectors). */
    fdc_init(&loci->dsk_fdc);
    /* DRQ + INTRQ callbacks update LOCI-visible bytes (active-low) — même
     * convention que Microdisc sur $0318 (DRQ) / $0314 (INTRQ). */
    loci->dsk_fdc.set_drq = loci_fdc_set_drq;
    loci->dsk_fdc.clr_drq = loci_fdc_clr_drq;
    loci->dsk_fdc.drq_userdata = loci;
    loci->dsk_fdc.set_intrq = loci_fdc_set_intrq;
    loci->dsk_fdc.clr_intrq = loci_fdc_clr_intrq;
    loci->dsk_fdc.intrq_userdata = loci;
    loci->dsk_drq   = 0x80;   /* clear at boot */
    loci->dsk_intrq = 0x80;
    for (int i = 0; i < 4; i++) {
        loci->dsk_tracks[i]  = 41;
        loci->dsk_sectors[i] = 17;
    }
    return true;
}

void loci_reset(loci_t* loci) {
    if (!loci || !loci->enabled) return;
    memset(loci->regs, 0, sizeof(loci->regs));
    memset(loci->xstack, 0, sizeof(loci->xstack));
    loci->xstack_ptr = LOCI_XSTACK_SIZE;
    loci->active_op = 0;
    loci->clock_start_us = now_us();
    seed_initial_stub(loci);
}

/* Forward decl — defined later. */
void loci_detach_sdimg(loci_t* loci);

void loci_cleanup(loci_t* loci) {
    if (!loci) return;
    /* Close any still-open files. Sprint 34ar : fd_kind is the
     * authoritative source for which backend owns each slot — no more
     * pointer-tag punning. Mixed POSIX/SDIMG cleanup works correctly. */
    for (int i = 0; i < LOCI_FD_MAX; i++) {
        if (loci->fds[i] && loci->fd_kind[i] == 1) {
            fclose((FILE*)loci->fds[i]);
        }
        loci->fds[i] = NULL;
        loci->fd_kind[i] = 0;
    }
    for (int i = 0; i < LOCI_DIR_MAX; i++) {
        if (loci->dirs[i] && loci->dir_kind[i] == 1) {
            closedir((DIR*)loci->dirs[i]);
        }
        loci->dirs[i] = NULL;
        loci->dir_kind[i] = 0;
    }
    /* Close mounted TAP (Sprint 34af). */
    if (loci->tap_fp) {
        fclose((FILE*)loci->tap_fp);
        loci->tap_fp = NULL;
    }
    /* Close mounted disks (Sprint 34ae) + free their image buffers (34aw). */
    for (int i = 0; i < 4; i++) {
        if (loci->dsk_fp[i]) {
            fclose((FILE*)loci->dsk_fp[i]);
            loci->dsk_fp[i] = NULL;
        }
        if (loci->dsk_image[i]) {
            free(loci->dsk_image[i]);
            loci->dsk_image[i] = NULL;
            loci->dsk_image_size[i] = 0;
        }
    }
    /* Detach SD image backend (Sprint 34ao). */
    loci_detach_sdimg(loci);
}

void loci_set_flash_root(loci_t* loci, const char* path) {
    if (!loci) return;
    if (path && *path) {
        strncpy(loci->flash_root, path, sizeof(loci->flash_root) - 1);
        loci->flash_root[sizeof(loci->flash_root) - 1] = '\0';
    } else {
        loci->flash_root[0] = '\0';
    }
}

bool loci_attach_sdimg(loci_t* loci, const char* path) {
    if (!loci || !path) return false;
    loci_detach_sdimg(loci);
    loci_sdimg_t* img = loci_sdimg_open(path);
    if (!img) {
        log_warning("LOCI: failed to open SD image '%s' (errno=%d)", path, errno);
        return false;
    }
    loci->sdimg = img;
    log_info("LOCI: SD image attached: %s (%s, %u bytes)",
             path, loci_sdimg_fs_label(img), loci_sdimg_total_size(img));
    return true;
}

void loci_detach_sdimg(loci_t* loci) {
    if (!loci || !loci->sdimg) return;
    loci_sdimg_close((loci_sdimg_t*)loci->sdimg);
    loci->sdimg = NULL;
}

void loci_set_tape_mount_callback(loci_t* loci,
        bool (*cb)(void*, const char*), void* ctx) {
    if (!loci) return;
    loci->tape_mount_cb = cb;
    loci->tape_mount_ctx = ctx;
}

void loci_set_dsk_bus_callbacks(loci_t* loci,
        void (*cpu_irq_set)(void*),
        void (*cpu_irq_clr)(void*),
        void (*sync_overlay)(void*, bool, bool),
        void* ctx) {
    if (!loci) return;
    loci->dsk_cpu_irq_set = cpu_irq_set;
    loci->dsk_cpu_irq_clr = cpu_irq_clr;
    loci->dsk_sync_overlay = sync_overlay;
    loci->dsk_bus_ctx = ctx;
}

void loci_set_rom_swap_callback(loci_t* loci,
        bool (*cb)(void*, const char*, uint16_t),
        void* ctx) {
    if (!loci) return;
    loci->rom_swap_cb = cb;
    loci->rom_swap_ctx = ctx;
}

void loci_set_action_callbacks(loci_t* loci,
        void (*install_cb)(void*),
        void (*release_cb)(void*),
        void* ctx) {
    if (!loci) return;
    loci->action_install_cb = install_cb;
    loci->action_release_cb = release_cb;
    loci->action_ctx = ctx;
}

/* ─── Action button (Sprint 34ai) ─────────────────────────────── */

/* The 6-byte IRQ trap installed at $03BA-$03BF (LOCI MIA register space):
 *   $03BA  B8        CLV         clear V flag
 *   $03BB  50 FE     BVC -2      spin while V=0 (loops back to BVC)
 *   $03BD  6C FA FF  JMP ($FFFA) indirect via NMI vector
 * When the user releases the button the host sets V=1, BVC falls through,
 * and JMP ($FFFA) jumps to the save-state handler in the LOCI ROM. */
static const uint8_t LOCI_ACTION_TRAP[6] = {
    0xB8, 0x50, 0xFE, 0x6C, 0xFA, 0xFF
};

void loci_action_button_short(loci_t* loci) {
    if (!loci || !loci->enabled) return;
    if (loci->action_active) return;  /* idempotent */

    /* Mirror the trap into the MIA register file so a 6502 instruction
     * fetch from $03BA-$03BF (routed through loci_read) returns the right
     * opcodes. Offsets 0x1A-0x1F within the 32-byte window. */
    for (int i = 0; i < 6; i++) {
        loci->regs[0x1A + i] = LOCI_ACTION_TRAP[i];
    }

    loci->action_active = true;
    if (loci->action_install_cb) {
        loci->action_install_cb(loci->action_ctx);
    }
}

void loci_action_button_release(loci_t* loci) {
    if (!loci || !loci->enabled) return;
    if (!loci->action_active) return;
    if (loci->action_release_cb) {
        loci->action_release_cb(loci->action_ctx);
    }
    loci->action_active = false;
}

/* ─── errno / BUSY / xstack helpers ────────────────────────────── */

static void set_errno(loci_t* loci, uint16_t e) {
    loci->regs[LOCI_REG_API_ERRNO_LO] = (uint8_t)(e & 0xFF);
    loci->regs[LOCI_REG_API_ERRNO_HI] = (uint8_t)(e >> 8);
}

/* set_busy moved below — definition deferred until after install_*_stub
 * helpers so the kept variant can document its overloaded semantics. */
static void set_busy(loci_t* loci, bool busy);

/* Mirror the top byte of xstack into $03AC so the 6502 sees it on read.
 * Called after every xstack mutation. */
static void xstack_sync(loci_t* loci) {
    if (loci->xstack_ptr >= LOCI_XSTACK_SIZE) {
        loci->regs[LOCI_REG_API_STACK] = 0;
    } else {
        loci->regs[LOCI_REG_API_STACK] = loci->xstack[loci->xstack_ptr];
    }
}

/* Reset the xstack to empty. */
static void xstack_zero(loci_t* loci) {
    loci->xstack_ptr = LOCI_XSTACK_SIZE;
    xstack_sync(loci);
}

/* Push N bytes onto the xstack. Returns false if not enough room. */
static bool xstack_push_n(loci_t* loci, const void* data, size_t n) {
    if (n > loci->xstack_ptr) return false;
    loci->xstack_ptr -= (uint16_t)n;
    memcpy(&loci->xstack[loci->xstack_ptr], data, n);
    xstack_sync(loci);
    return true;
}

static bool xstack_push_u32(loci_t* loci, uint32_t v) {
    return xstack_push_n(loci, &v, 4);
}

static bool xstack_push_i32(loci_t* loci, int32_t v) {
    return xstack_push_n(loci, &v, 4);
}

/* ─── API return helpers (mirror firmware semantics) ──────────────
 *
 * The MIA "spin" window at $03B0-$03B9 isn't a passive register file —
 * it's a chunk of self-modifying 6502 code that the host coprocessor
 * (Pi Pico firmware on real LOCI hardware) rewrites on every op
 * transition. The 6502 ABI is:
 *
 *     STA $03AF          ; trigger op
 *     JSR $03B0          ; CALL the spin window
 *     ; <on return, A = result_lo, X = result_hi,
 *     ;            $03B8/B9 hold the high 16 bits for AXSREG returns>
 *
 * Blocked stub (op queued, BUSY set) — installed when the 6502 writes
 * a non-trivial op to $03AF :
 *
 *     $03B0  B8           CLV
 *     $03B1  50 FE        BVC -2     (loops back to $03B0)
 *     $03B3  A9 --        LDA #A     (operand patched later)
 *
 * Released stub (op done, BUSY cleared) — installed when the handler
 * has set A/X/SREG. Byte $03B2 = $00 acts as BOTH "BUSY cleared" AND
 * "BVC operand = +0" so the CPU falls through into LDA/LDX/RTS :
 *
 *     $03B0  B8           CLV
 *     $03B1  50 00        BVC +0     (falls through)
 *     $03B3  A9 <A_lo>    LDA #A
 *     $03B5  A2 <X_hi>    LDX #X
 *     $03B7  60           RTS
 *     $03B8  <SREG_lo>
 *     $03B9  <SREG_hi>
 *
 * Phosphoric prior to this fix only wrote $03B4 ($03B6, $03B8, $03B9)
 * — never the BVC/LDA/LDX/RTS opcodes. So a real 6502 `JSR $03B0`
 * fetched whatever was in regs[0x10..] (initialised to zero =
 * `BRK BRK …`), and the LOCI ROM's first fastcall (`tap_tell` from
 * `update_tap_counter` at main.c:1159) hung the boot before reaching
 * `InitKeyboard()` and the TUI's `while(1)`. Hence "no spinner, no
 * key reaction" reported in CR 2026-06-06_LOCI_Session. */

/* Install the BLOCKED stub at $03B0-$03B3. The operand byte at $03B2
 * is $FE which has bit 7 set, matching the firmware's overloaded
 * "BUSY=1" semantics. */
static void api_install_blocked_stub(loci_t* loci) {
    loci->regs[LOCI_REG_INJECT0 + 0] = 0xB8;   /* CLV          */
    loci->regs[LOCI_REG_INJECT0 + 1] = 0x50;   /* BVC          */
    loci->regs[LOCI_REG_INJECT0 + 2] = 0xFE;   /* operand -2 + BUSY=1 */
    loci->regs[LOCI_REG_INJECT0 + 3] = 0xA9;   /* LDA # (operand patched on release) */
}

/* Install the RELEASED stub at $03B0-$03B3. Operand $00 = both
 * BVC +0 (fall-through) and BUSY=0. */
static void api_install_released_stub(loci_t* loci) {
    loci->regs[LOCI_REG_INJECT0 + 0] = 0xB8;   /* CLV       */
    loci->regs[LOCI_REG_INJECT0 + 1] = 0x50;   /* BVC       */
    loci->regs[LOCI_REG_INJECT0 + 2] = 0x00;   /* operand +0 + BUSY=0 */
    loci->regs[LOCI_REG_INJECT0 + 3] = 0xA9;   /* LDA #     */
}

/* api_set_ax matches firmware api.h:183-186 byte-for-byte: stores A at
 * $03B4, the LDX # opcode ($A2) at $03B5, X at $03B6, RTS ($60) at $03B7.
 * Result : a `JSR $03B0` that falls through the BVC will execute
 *   LDA #A ; LDX #X ; RTS
 * and return cleanly with A/X loaded. */
static void api_set_ax(loci_t* loci, uint16_t val) {
    loci->regs[LOCI_REG_API_A]    = (uint8_t)(val & 0xFF);     /* $03B4 */
    loci->regs[LOCI_REG_INJECT0 + 5] = 0xA2;                   /* $03B5 LDX # */
    loci->regs[LOCI_REG_API_X]    = (uint8_t)((val >> 8) & 0xFF); /* $03B6 */
    loci->regs[LOCI_REG_INJECT0 + 7] = 0x60;                   /* $03B7 RTS */
}

static void api_set_axsreg(loci_t* loci, uint32_t val) {
    api_set_ax(loci, (uint16_t)val);
    loci->regs[LOCI_REG_API_SREG]    = (uint8_t)((val >> 16) & 0xFF); /* $03B8 */
    loci->regs[LOCI_REG_API_SREG_HI] = (uint8_t)((val >> 24) & 0xFF); /* $03B9 */
}

/* Kept as an explicit name for sites that just want the BUSY bit
 * toggled (e.g. xstack pop after release). Reads/writes the same
 * BUSY bit the firmware overloads onto the BVC operand. */
static void set_busy(loci_t* loci, bool busy) {
    if (busy) loci->regs[LOCI_REG_BUSY] |=  0x80;
    else      loci->regs[LOCI_REG_BUSY] &= ~0x80;
}

static void api_return_ax(loci_t* loci, uint16_t val) {
    api_set_ax(loci, val);
    api_install_released_stub(loci);   /* writes $03B0-3 incl. BUSY=0 */
    set_busy(loci, false);             /* redundant but explicit */
}

static void api_return_axsreg(loci_t* loci, uint32_t val) {
    api_set_axsreg(loci, val);
    api_install_released_stub(loci);
    set_busy(loci, false);
}

static void api_return_errno(loci_t* loci, uint16_t e) {
    xstack_zero(loci);
    set_errno(loci, e);
    api_return_axsreg(loci, 0xFFFFFFFFu);
}

/* ─── API handlers (Sprint 34z: system / RTC / RNG) ──────────── */

/* 0x01 PIX_XREG — generic packet routed to a PIX device.
 *
 * xstack layout (firmware reads from the high end):
 *   xstack[XSTACK_SIZE-1] = device   (0 = MIA, the only one we serve)
 *   xstack[XSTACK_SIZE-2] = channel
 *   xstack[XSTACK_SIZE-3] = addr
 *   xstack[XSTACK_SIZE-4..] = one or more uint16 data words (popped in reverse)
 *
 * Sprint 34ag handles the MIA HID subset:
 *   channel*256 + addr  == 0  → kbd_xreg(word)   (channel 0 addr 0)
 *                       == 1  → mou_xreg(word)
 *                       == 2  → pad_xreg(word)
 * The `word` arg gives the xram address where the bitmap mirrors live
 * (0xFFFF disables the mirror — matches firmware sentinel). */
static void op_pix_xreg(loci_t* loci) {
    /* Need at least device + channel + addr + 2-byte word = 5 bytes. */
    if (LOCI_XSTACK_SIZE - loci->xstack_ptr < 5) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    uint8_t device  = loci->xstack[LOCI_XSTACK_SIZE - 1];
    uint8_t channel = loci->xstack[LOCI_XSTACK_SIZE - 2];
    uint8_t addr    = loci->xstack[LOCI_XSTACK_SIZE - 3];

    /* Only MIA device is wired in Sprint 34ag — VGA/others would route
     * to a video chip that doesn't exist here. */
    if (device != 0) {
        xstack_zero(loci);
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }

    /* Data uint16 lives at xstack[XSTACK_SIZE-5..XSTACK_SIZE-4]
     * (lo at index -5, hi at -4 per firmware push convention). */
    uint16_t word = (uint16_t)loci->xstack[LOCI_XSTACK_SIZE - 5] |
                    ((uint16_t)loci->xstack[LOCI_XSTACK_SIZE - 4] << 8);

    uint16_t route = (uint16_t)channel * 256 + addr;
    switch (route) {
        case 0: loci->kbd_xram = word; break;
        case 1: loci->mou_xram = word; break;
        case 2: loci->pad_xram = word; break;
        default:
            /* Unhandled channel/addr — accept silently per Sprint 34z
             * behaviour to avoid breaking ROMs that probe other channels. */
            break;
    }

    xstack_zero(loci);
    api_return_ax(loci, 0);
}

/* ─── HID bridge (Sprint 34ag) ─────────────────────────────────── */

/* HID usage codes we care about. */
#define LOCI_HID_KEY_A             0x04
#define LOCI_HID_KEY_CONTROL_LEFT  0xE0   /* >> 3 = 28 → modifier byte index */

void loci_kbd_set_report(loci_t* loci, uint8_t modifier,
                          const uint8_t keycodes[6]) {
    if (!loci || !loci->enabled) return;
    if (loci->kbd_xram == 0xFFFF) return;
    if ((uint32_t)loci->kbd_xram + 32 > LOCI_XRAM_SIZE) return;

    uint8_t keys[32] = {0};
    bool any_key = false;
    /* Phantom-state preservation: if a slot reports keycode 1 (ErrorRollOver),
     * keep previous keys. For now we ignore phantom (we never inject it). */
    for (int i = 0; i < 6; i++) {
        uint8_t kc = keycodes[i];
        if (kc >= LOCI_HID_KEY_A) {
            any_key = true;
            keys[kc >> 3] |= (uint8_t)(1u << (kc & 7));
        }
    }
    /* Modifier maps directly to byte 28 (HID_KEY_CONTROL_LEFT >> 3). */
    keys[LOCI_HID_KEY_CONTROL_LEFT >> 3] = modifier;
    /* Sentinel bit 0 of byte 0 marks "no key pressed". */
    if (!any_key && !modifier) keys[0] |= 0x01;
    /* NUMLOCK/CAPSLOCK bits (2,3) intentionally omitted — host keyboard
     * lock state isn't relevant on the Oric side. */

    memcpy(&loci->xram[loci->kbd_xram], keys, 32);
}

void loci_kbd_clear(loci_t* loci) {
    uint8_t empty[6] = {0};
    loci_kbd_set_report(loci, 0, empty);
}

void loci_mou_report(loci_t* loci, uint8_t buttons,
                     int8_t dx, int8_t dy,
                     int8_t wheel, int8_t pan) {
    if (!loci || !loci->enabled) return;
    if (loci->mou_xram == 0xFFFF) return;
    if ((uint32_t)loci->mou_xram + 5 > LOCI_XRAM_SIZE) return;

    /* Layout (firmware mou_xram_data): buttons, dx, dy, wheel, pan.
     * dx/dy/wheel/pan accumulate (firmware does mou_xram_data.x +=
     * report->x on each HID poll), so we add into the live byte. */
    uint8_t* m = &loci->xram[loci->mou_xram];
    m[0]  = buttons;
    m[1] = (uint8_t)(m[1] + (uint8_t)dx);
    m[2] = (uint8_t)(m[2] + (uint8_t)dy);
    m[3] = (uint8_t)(m[3] + (uint8_t)wheel);
    m[4] = (uint8_t)(m[4] + (uint8_t)pan);
}

/* 0x04 RNG_LRAND — returns a 31-bit positive random uint32 in AXSREG. */
static void op_rng_lrand(loci_t* loci) {
    /* Use rand() seeded once at init time for variety. The firmware uses
     * the Pi Pico's hardware RNG; deterministic-mode hook reserved for
     * later. */
    uint32_t v = (uint32_t)rand();
    v ^= (uint32_t)(rand() << 16);
    v &= 0x7FFFFFFFu;
    api_return_axsreg(loci, v);
}

/* 0x0F CLOCK — uptime in 10 ms units (firmware: us_64 / 10000). */
static void op_clock(loci_t* loci) {
    uint64_t now = now_us();
    uint64_t up  = (now >= loci->clock_start_us)
                       ? (now - loci->clock_start_us)
                       : 0;
    api_return_axsreg(loci, (uint32_t)(up / 10000ULL));
}

#define CLK_ID_REALTIME 0

/* 0x10 CLK_GETRES — push (uint32 sec=1, int32 nsec=0); return ax=0. */
static void op_clk_getres(loci_t* loci) {
    uint8_t clock_id = loci->regs[LOCI_REG_API_A];
    if (clock_id != CLK_ID_REALTIME) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    int32_t nsec = 0;
    uint32_t sec = 1;
    if (!xstack_push_i32(loci, nsec) || !xstack_push_u32(loci, sec)) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    api_return_ax(loci, 0);
}

/* 0x11 CLK_GETTIME — push (uint32 rawtime, int32 nsec=0); return ax=0. */
static void op_clk_gettime(loci_t* loci) {
    uint8_t clock_id = loci->regs[LOCI_REG_API_A];
    if (clock_id != CLK_ID_REALTIME) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    time_t t = time(NULL);
    int32_t nsec = 0;
    if (!xstack_push_i32(loci, nsec) ||
        !xstack_push_u32(loci, (uint32_t)t)) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    api_return_ax(loci, 0);
}

/* 0x12 CLK_SETTIME — pop (uint32 rawtime, int32 nsec); ack ax=0
 * (we don't actually retune the host clock — would need privileges). */
static void op_clk_settime(loci_t* loci) {
    uint8_t clock_id = loci->regs[LOCI_REG_API_A];
    if (clock_id != CLK_ID_REALTIME) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    /* Drain four bytes (rawtime u32) + four bytes (nsec i32) from xstack.
     * We don't actually use them. */
    if (loci->xstack_ptr + 8 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    loci->xstack_ptr += 8;
    xstack_sync(loci);
    api_return_ax(loci, 0);
}

/* ─── xram DMA window helpers (Sprint 34ab) ───────────────────── */

static uint16_t get_addr0(loci_t* loci) {
    return (uint16_t)loci->regs[LOCI_REG_ADDR0_LO] |
           ((uint16_t)loci->regs[LOCI_REG_ADDR0_HI] << 8);
}
static uint16_t get_addr1(loci_t* loci) {
    return (uint16_t)loci->regs[LOCI_REG_ADDR1_LO] |
           ((uint16_t)loci->regs[LOCI_REG_ADDR1_HI] << 8);
}
static void set_addr0(loci_t* loci, uint16_t a) {
    loci->regs[LOCI_REG_ADDR0_LO] = (uint8_t)(a & 0xFF);
    loci->regs[LOCI_REG_ADDR0_HI] = (uint8_t)(a >> 8);
}
static void set_addr1(loci_t* loci, uint16_t a) {
    loci->regs[LOCI_REG_ADDR1_LO] = (uint8_t)(a & 0xFF);
    loci->regs[LOCI_REG_ADDR1_HI] = (uint8_t)(a >> 8);
}

/* Refresh the RW0/RW1 register from the current addr. Called after any
 * change to addr0/addr1 (write to lo/hi byte) so the next 6502 read of
 * $03A4/$03A8 sees the byte at the new position. */
static void refresh_rw0(loci_t* loci) {
    loci->regs[LOCI_REG_RW0] = loci->xram[get_addr0(loci)];
}
static void refresh_rw1(loci_t* loci) {
    loci->regs[LOCI_REG_RW1] = loci->xram[get_addr1(loci)];
}

/* ─── File I/O (Sprint 34aa) ──────────────────────────────────── */

/* Map host errno → LOCI errno. */
static uint16_t map_errno(int e) {
    switch (e) {
        case ENOENT: return LOCI_ENOENT;
        case EACCES: return LOCI_EACCES;
        case EEXIST: return LOCI_EEXIST;
        case ENOMEM: return LOCI_ENOMEM;
        case EBADF:  return LOCI_EBADF;
        case EINVAL: return LOCI_EINVAL;
        case ENOSPC: return LOCI_ENOSPC;
        case EBUSY:  return LOCI_EBUSY;
        case EIO:    return LOCI_EIO;
        case ENOSYS: return LOCI_ENOSYS;
        default:     return LOCI_EIO;
    }
}

/* Resolve a guest path against the sandbox root. Rejects absolute and
 * up-traversing paths to keep guest 6502 code from escaping the root.
 * Strips a leading volume prefix like "0:" or "USB:" if present.
 *
 * Path-traversal rejection (sprint 34c, R2 from senior review post-34b5):
 * tokenize on '/' and reject only the EXACT component ".." or empty.
 * The earlier strstr(p, "..") rejected legitimate names like
 * "my..backup.dsk" while missing URL-encoded variants. */
static bool resolve_path(loci_t* loci, const char* in,
                         char* out, size_t outsize) {
    /* Strip volume prefix "X:" where X is alnum. */
    const char* p = in;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '/' || *p == '\\') p++;

    /* Walk components, rejecting "." (no-op trap) and ".." (escape). */
    const char* c = p;
    while (*c) {
        const char* end = c;
        while (*end && *end != '/' && *end != '\\') end++;
        size_t len = (size_t)(end - c);
        if (len == 0) return false;                  /* "//" */
        if (len == 2 && c[0] == '.' && c[1] == '.') return false;
        c = end;
        while (*c == '/' || *c == '\\') c++;
    }

    const char* root = loci->flash_root[0] ? loci->flash_root : ".";
    int n = snprintf(out, outsize, "%s/%s", root, p);
    return n > 0 && (size_t)n < outsize;
}

/* Map LOCI open flags → fopen mode string. */
static const char* fopen_mode_for(uint8_t flags) {
    int rw = flags & LOCI_O_RDWR;
    bool create = (flags & LOCI_O_CREAT) != 0;
    bool trunc  = (flags & LOCI_O_TRUNC) != 0;
    bool append = (flags & LOCI_O_APPEND) != 0;

    if (rw == 0) return "rb";              /* read only */
    if (append)  return rw == 1 ? "ab"  : "a+b";
    if (trunc)   return rw == 1 ? "wb"  : "w+b";
    if (create)  return rw == 1 ? "wb"  : "w+b";   /* create new for write */
    return rw == 1 ? "r+b" : "r+b";        /* open existing */
}

/* Pop a null-terminated string from the xstack top into out (path), then
 * zero the xstack as per firmware semantics. Returns true on success. */
static bool pop_zstring(loci_t* loci, char* out, size_t outsize) {
    if (loci->xstack_ptr >= LOCI_XSTACK_SIZE) return false;
    size_t n = 0;
    uint16_t p = loci->xstack_ptr;
    while (p < LOCI_XSTACK_SIZE && n + 1 < outsize) {
        uint8_t c = loci->xstack[p++];
        if (c == 0) break;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    xstack_zero(loci);
    return n > 0;
}

/* Allocate a free fd slot. Returns -1 if all are used. */
static int alloc_fd(loci_t* loci) {
    for (int i = 0; i < LOCI_FD_MAX; i++) {
        if (loci->fds[i] == NULL) return i;
    }
    return -1;
}

static FILE* fd_to_file(loci_t* loci, int fd) {
    int idx = fd - LOCI_FD_OFFSET;
    if (idx < 0 || idx >= LOCI_FD_MAX) return NULL;
    return (FILE*)loci->fds[idx];
}

/* ─── SDIMG backend dispatch (Sprint 34ao) ──────────────────────────
 * When loci->sdimg is non-NULL, the SD raw image backend handles all
 * file/dir ops instead of POSIX. Each handler below mirrors its POSIX
 * counterpart's contract (errno mapping, return value, xstack layout).
 * Writes are rejected with EACCES — read-only initial cut. */

static int sdimg_errno_to_loci(int neg_errno) {
    int e = neg_errno < 0 ? -neg_errno : neg_errno;
    switch (e) {
        case 0:        return 0;
        case ENOENT:   return LOCI_ENOENT;
        case EACCES:   return LOCI_EACCES;
        case EISDIR:   return LOCI_EACCES;
        case ENOTDIR:  return LOCI_EINVAL;
        case EBADF:    return LOCI_EBADF;
        case EINVAL:   return LOCI_EINVAL;
        case EMFILE:   return LOCI_EMFILE;
        case EIO:      return LOCI_EIO;
        default:       return LOCI_EIO;
    }
}

/* Extract a file from the SD image to a temp file, so that legacy
 * POSIX-FILE-based code (rom_swap_cb, tap_open, dsk_open) can keep
 * working unchanged when --loci-sdimg is active.
 *
 * Sprint 34ar : use mkstemp() instead of a predictable
 * /tmp/loci_extract_<basename> path. Avoids collision when two emulator
 * instances run concurrently and removes the classic symlink-race vector
 * (an attacker could pre-create a symlink at the predictable path to
 * redirect the write). Caller is responsible for unlinking the temp file
 * after the consumer is done with it. */
static bool sdimg_extract_to_temp(loci_t* loci, const char* sd_path,
                                  char* out_host_path, size_t out_sz) {
    /* Strip the LOCI volume prefix if present. */
    const char* p = sd_path;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '/' || *p == '\\') p++;

    int fd = loci_sdimg_fopen((loci_sdimg_t*)loci->sdimg, p);
    if (fd < 0) {
        log_info("LOCI SDIMG extract: not found in image: '%s' (errno=%d)", p, -fd);
        return false;
    }
    /* Compose template: /tmp/loci_<basename>_XXXXXX. mkstemp replaces
     * XXXXXX with 6 random chars and opens the file securely (O_CREAT |
     * O_EXCL | 0600). */
    const char* base = strrchr(p, '/');
    base = base ? base + 1 : p;
    int n = snprintf(out_host_path, out_sz, "/tmp/loci_%s_XXXXXX", base);
    if (n <= 0 || (size_t)n >= out_sz) {
        loci_sdimg_fclose((loci_sdimg_t*)loci->sdimg, fd);
        return false;
    }
    int tmpfd = mkstemp(out_host_path);
    if (tmpfd < 0) {
        loci_sdimg_fclose((loci_sdimg_t*)loci->sdimg, fd);
        return false;
    }
    FILE* fp = fdopen(tmpfd, "wb");
    if (!fp) {
        close(tmpfd);
        unlink(out_host_path);
        loci_sdimg_fclose((loci_sdimg_t*)loci->sdimg, fd);
        return false;
    }
    uint8_t buf[512];
    int br;
    while ((br = loci_sdimg_fread((loci_sdimg_t*)loci->sdimg, fd, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)br, fp);
    }
    fclose(fp);
    loci_sdimg_fclose((loci_sdimg_t*)loci->sdimg, fd);
    log_info("LOCI SDIMG extract: '%s' → '%s'", p, out_host_path);
    return true;
}

static void op_open_sdimg(loci_t* loci) {
    uint8_t flags = loci->regs[LOCI_REG_API_A];
    char path[260] = {0};
    pop_zstring(loci, path, sizeof(path));
    if (path[0] == 0) {
        api_return_errno(loci, LOCI_ENOENT);
        return;
    }
    /* Strip optional volume prefix "X:". */
    const char* p = path;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '/' || *p == '\\') p++;

    /* Sprint 34ap: route through write-aware open if write flags set. */
    int rw = flags & LOCI_O_RDWR;
    int mode = 0;
    if ((flags & (LOCI_O_CREAT | LOCI_O_TRUNC | LOCI_O_APPEND)) ||
        rw == 1 || rw == 3) {
        mode = (rw == 3) ? 2 : 1;
    }
    int slot = loci_sdimg_fopen_ex((loci_sdimg_t*)loci->sdimg, p, mode);
    if (slot < 0) {
        api_return_errno(loci, sdimg_errno_to_loci(slot));
        return;
    }
    /* Mark slot as SDIMG-owned via fd_kind; fds[] just holds a non-NULL
     * marker (loci_t* is convenient and never type-punned). */
    loci->fds[slot] = loci;
    loci->fd_kind[slot] = 2;
    api_return_ax(loci, (uint16_t)(slot + LOCI_FD_OFFSET));
}

static void op_close_sdimg(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    int slot = fd - LOCI_FD_OFFSET;
    if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    int r = loci_sdimg_fclose((loci_sdimg_t*)loci->sdimg, slot);
    loci->fds[slot] = NULL;
    loci->fd_kind[slot] = 0;
    if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }
    api_return_ax(loci, 0);
}

static void op_read_xstack_sdimg(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    if (loci->xstack_ptr + 2 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL); return;
    }
    uint16_t count = (uint16_t)loci->xstack[loci->xstack_ptr]
                   | ((uint16_t)loci->xstack[loci->xstack_ptr + 1] << 8);
    loci->xstack_ptr += 2;
    if (count == 0 || count > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL); return;
    }
    int slot = fd - LOCI_FD_OFFSET;
    if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
        api_return_errno(loci, LOCI_EBADF); return;
    }
    uint8_t buf[LOCI_XSTACK_SIZE];
    int br = loci_sdimg_fread((loci_sdimg_t*)loci->sdimg, slot, buf, count);
    if (br < 0) { api_return_errno(loci, sdimg_errno_to_loci(br)); return; }
    xstack_zero(loci);
    if (br > 0) {
        loci->xstack_ptr = (uint16_t)(LOCI_XSTACK_SIZE - br);
        memcpy(&loci->xstack[loci->xstack_ptr], buf, (size_t)br);
        xstack_sync(loci);
    }
    api_return_ax(loci, (uint16_t)br);
}

static void op_lseek_sdimg(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    if (loci->xstack_ptr + 5 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL); return;
    }
    int32_t offset;
    memcpy(&offset, &loci->xstack[loci->xstack_ptr], 4);
    loci->xstack_ptr += 4;
    uint8_t whence = loci->xstack[loci->xstack_ptr++];
    xstack_zero(loci);
    int slot = fd - LOCI_FD_OFFSET;
    if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
        api_return_errno(loci, LOCI_EBADF); return;
    }
    int32_t pos = loci_sdimg_lseek((loci_sdimg_t*)loci->sdimg, slot, offset, whence);
    if (pos < 0) { api_return_errno(loci, sdimg_errno_to_loci(pos)); return; }
    api_return_axsreg(loci, (uint32_t)pos);
}

static void op_opendir_sdimg(loci_t* loci) {
    char path[260] = {0};
    /* Accept empty path = root. pop_zstring returns false on n==0 but
     * still zeroes path; we don't error out. */
    pop_zstring(loci, path, sizeof(path));
    const char* p = path;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '/' || *p == '\\') p++;
    int slot = loci_sdimg_opendir((loci_sdimg_t*)loci->sdimg, p);
    if (slot < 0) { api_return_errno(loci, sdimg_errno_to_loci(slot)); return; }
    /* Tag dirs[] slot for collision avoidance. */
    loci->dirs[slot] = loci;
    loci->dir_kind[slot] = 2;
    api_return_ax(loci, (uint16_t)(slot + LOCI_DIR_OFFSET));
}

static void op_closedir_sdimg(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    int slot = fd - LOCI_DIR_OFFSET;
    if (slot < 0 || slot >= LOCI_DIR_MAX || !loci->dirs[slot]) {
        api_return_errno(loci, LOCI_EBADF); return;
    }
    int r = loci_sdimg_closedir((loci_sdimg_t*)loci->sdimg, slot);
    loci->dirs[slot] = NULL;
    loci->dir_kind[slot] = 0;
    if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }
    api_return_ax(loci, 0);
}

static void op_readdir_sdimg(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    int slot = fd - LOCI_DIR_OFFSET;
    if (slot < 0 || slot >= LOCI_DIR_MAX || !loci->dirs[slot]) {
        api_return_errno(loci, LOCI_EBADF); return;
    }
    char name[64] = {0};
    uint8_t attrib = 0;
    uint32_t size = 0;
    int r = loci_sdimg_readdir((loci_sdimg_t*)loci->sdimg, slot, name, &attrib, &size);
    if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }

    uint8_t dirent_buf[LOCI_DIRENT_SIZE] = {0};
    dirent_buf[0] = (uint8_t)(fd & 0xFF);
    dirent_buf[1] = (uint8_t)((fd >> 8) & 0xFF);
    if (r == 1) {
        size_t nl = strlen(name);
        if (nl > LOCI_DIR_NAME_LEN - 1) nl = LOCI_DIR_NAME_LEN - 1;
        memcpy(&dirent_buf[2], name, nl);
        /* Normalize attrib to match what POSIX op_readdir emits: only
         * the DIR bit (0x10) survives. Files end up with attrib=0,
         * which is what the LOCI ROM TUI expects to render them. */
        dirent_buf[66] = (attrib & LOCI_AM_DIR) ? LOCI_AM_DIR : 0;
        memcpy(&dirent_buf[68], &size, 4);
    }
    xstack_zero(loci);
    loci->xstack_ptr = (uint16_t)(LOCI_XSTACK_SIZE - LOCI_DIRENT_SIZE);
    memcpy(&loci->xstack[loci->xstack_ptr], dirent_buf, LOCI_DIRENT_SIZE);
    xstack_sync(loci);
    api_return_ax(loci, 0);
}

/* 0x14 OPEN: A=flags, xstack top = path (null-terminated). Returns fd or -1. */
static void op_open(loci_t* loci) {
    if (loci->sdimg) { op_open_sdimg(loci); return; }
    uint8_t flags = loci->regs[LOCI_REG_API_A];
    char path[260];
    if (!pop_zstring(loci, path, sizeof(path))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    char host_path[512];
    if (!resolve_path(loci, path, host_path, sizeof(host_path))) {
        api_return_errno(loci, LOCI_EACCES);
        return;
    }

    /* Optional EXCL: refuse if file exists. */
    if ((flags & LOCI_O_CREAT) && (flags & LOCI_O_EXCL)) {
        if (access(host_path, F_OK) == 0) {
            api_return_errno(loci, LOCI_EEXIST);
            return;
        }
    }

    FILE* fp = fopen(host_path, fopen_mode_for(flags));
    if (!fp) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    int slot = alloc_fd(loci);
    if (slot < 0) {
        fclose(fp);
        api_return_errno(loci, LOCI_EMFILE);
        return;
    }
    loci->fds[slot] = fp;
    loci->fd_kind[slot] = 1;
    api_return_ax(loci, (uint16_t)(slot + LOCI_FD_OFFSET));
}

/* 0x15 CLOSE: A=fd. */
static void op_close(loci_t* loci) {
    if (loci->sdimg) { op_close_sdimg(loci); return; }
    int fd = loci->regs[LOCI_REG_API_A];
    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    fclose(fp);
    loci->fds[fd - LOCI_FD_OFFSET] = NULL;
    loci->fd_kind[fd - LOCI_FD_OFFSET] = 0;
    api_return_ax(loci, 0);
}

/* 0x16 READ_XSTACK: A=fd, xstack top = uint16 count. Read into xstack. */
static void op_read_xstack(loci_t* loci) {
    if (loci->sdimg) { op_read_xstack_sdimg(loci); return; }
    int fd = loci->regs[LOCI_REG_API_A];
    /* Pop 2 bytes (count, LE) from xstack. */
    if (loci->xstack_ptr + 2 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    uint16_t count = (uint16_t)loci->xstack[loci->xstack_ptr]
                   | ((uint16_t)loci->xstack[loci->xstack_ptr + 1] << 8);
    loci->xstack_ptr += 2;
    if (count == 0 || count > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    /* Read into a temp buffer, then push onto xstack (descending). */
    uint8_t buf[LOCI_XSTACK_SIZE];
    size_t br = fread(buf, 1, count, fp);
    xstack_zero(loci);
    if (br > 0) {
        /* Push so that buf[0] ends up at xstack[XSTACK_SIZE-br]. */
        loci->xstack_ptr = (uint16_t)(LOCI_XSTACK_SIZE - br);
        memcpy(&loci->xstack[loci->xstack_ptr], buf, br);
        xstack_sync(loci);
    }
    api_return_ax(loci, (uint16_t)br);
}

/* 0x18 WRITE_XSTACK: A=fd, xstack contains bytes followed by uint16 count.
 * The firmware first pops the count, then the bytes are at xstack[ptr..ptr+count]. */
static void op_write_xstack(loci_t* loci) {
    if (loci->sdimg) {
        int fd = loci->regs[LOCI_REG_API_A];
        if (loci->xstack_ptr + 2 > LOCI_XSTACK_SIZE) {
            api_return_errno(loci, LOCI_EINVAL); return;
        }
        uint16_t count = (uint16_t)loci->xstack[loci->xstack_ptr]
                       | ((uint16_t)loci->xstack[loci->xstack_ptr + 1] << 8);
        loci->xstack_ptr += 2;
        if (count == 0) {
            xstack_zero(loci);
            api_return_ax(loci, 0);
            return;
        }
        if (loci->xstack_ptr + count > LOCI_XSTACK_SIZE) {
            api_return_errno(loci, LOCI_EINVAL); return;
        }
        int slot = fd - LOCI_FD_OFFSET;
        if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
            api_return_errno(loci, LOCI_EBADF); return;
        }
        int bw = loci_sdimg_fwrite((loci_sdimg_t*)loci->sdimg, slot,
                                   &loci->xstack[loci->xstack_ptr], count);
        xstack_zero(loci);
        if (bw < 0) { api_return_errno(loci, sdimg_errno_to_loci(bw)); return; }
        api_return_ax(loci, (uint16_t)bw);
        return;
    }
    int fd = loci->regs[LOCI_REG_API_A];
    if (loci->xstack_ptr + 2 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    uint16_t count = (uint16_t)loci->xstack[loci->xstack_ptr]
                   | ((uint16_t)loci->xstack[loci->xstack_ptr + 1] << 8);
    loci->xstack_ptr += 2;
    if (count == 0) {
        xstack_zero(loci);
        api_return_ax(loci, 0);
        return;
    }
    if (loci->xstack_ptr + count > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    size_t bw = fwrite(&loci->xstack[loci->xstack_ptr], 1, count, fp);
    xstack_zero(loci);
    api_return_ax(loci, (uint16_t)bw);
}

/* 0x1A LSEEK: A=fd, xstack contains int32 offset, uint8 whence.
 * Firmware pushes whence first then offset (offset on top). */
static void op_lseek(loci_t* loci) {
    if (loci->sdimg) { op_lseek_sdimg(loci); return; }
    int fd = loci->regs[LOCI_REG_API_A];
    if (loci->xstack_ptr + 5 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    int32_t offset;
    memcpy(&offset, &loci->xstack[loci->xstack_ptr], 4);
    loci->xstack_ptr += 4;
    uint8_t whence = loci->xstack[loci->xstack_ptr++];
    xstack_zero(loci);

    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    int hw;
    switch (whence) {
        case 0: hw = SEEK_SET; break;
        case 1: hw = SEEK_CUR; break;
        case 2: hw = SEEK_END; break;
        default:
            api_return_errno(loci, LOCI_EINVAL);
            return;
    }
    if (fseek(fp, offset, hw) != 0) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    long pos = ftell(fp);
    if (pos < 0) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    /* Return position in AXSREG (uint32). */
    api_return_axsreg(loci, (uint32_t)pos);
}

/* 0x17 READ_XRAM: A=fd, xstack contains uint16 count then uint16 xram_addr
 * (firmware: api_pop_uint16(&count) then api_pop_uint16_end(&xram_addr)).
 * Reads from file into xram[xram_addr..]. Returns bytes_read in AXSREG. */
static void op_read_xram(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    if (loci->xstack_ptr + 4 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    uint16_t count;
    uint16_t xram_addr;
    memcpy(&count, &loci->xstack[loci->xstack_ptr], 2);
    loci->xstack_ptr += 2;
    memcpy(&xram_addr, &loci->xstack[loci->xstack_ptr], 2);
    loci->xstack_ptr += 2;
    xstack_zero(loci);

    if ((uint32_t)xram_addr + count > LOCI_XRAM_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }

    if (loci->sdimg) {
        int slot = fd - LOCI_FD_OFFSET;
        if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
            api_return_errno(loci, LOCI_EBADF); return;
        }
        int br = loci_sdimg_fread((loci_sdimg_t*)loci->sdimg, slot,
                                  &loci->xram[xram_addr], count);
        if (br < 0) { api_return_errno(loci, sdimg_errno_to_loci(br)); return; }
        refresh_rw0(loci);
        refresh_rw1(loci);
        api_return_axsreg(loci, (uint32_t)br);
        return;
    }

    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    size_t br = fread(&loci->xram[xram_addr], 1, count, fp);
    /* Refresh windows in case they pointed into the touched region. */
    refresh_rw0(loci);
    refresh_rw1(loci);
    api_return_axsreg(loci, (uint32_t)br);
}

/* 0x19 WRITE_XRAM: A=fd, xstack count + xram_addr.
 * Writes xram[xram_addr..] to file. Returns bytes_written. */
static void op_write_xram(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    if (loci->xstack_ptr + 4 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    uint16_t count;
    uint16_t xram_addr;
    memcpy(&count, &loci->xstack[loci->xstack_ptr], 2);
    loci->xstack_ptr += 2;
    memcpy(&xram_addr, &loci->xstack[loci->xstack_ptr], 2);
    loci->xstack_ptr += 2;
    xstack_zero(loci);

    if ((uint32_t)xram_addr + count > LOCI_XRAM_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }

    if (loci->sdimg) {
        int slot = fd - LOCI_FD_OFFSET;
        if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
            api_return_errno(loci, LOCI_EBADF); return;
        }
        int bw = loci_sdimg_fwrite((loci_sdimg_t*)loci->sdimg, slot,
                                   &loci->xram[xram_addr], count);
        if (bw < 0) { api_return_errno(loci, sdimg_errno_to_loci(bw)); return; }
        api_return_axsreg(loci, (uint32_t)bw);
        return;
    }

    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    size_t bw = fwrite(&loci->xram[xram_addr], 1, count, fp);
    api_return_axsreg(loci, (uint32_t)bw);
}

/* 0x88 GETCWD: A=drive (0-5, or 255 = derive boot). Pushes path string
 * onto xstack and returns its length in A. */
static void op_getcwd(loci_t* loci) {
    uint8_t drive = loci->regs[LOCI_REG_API_A];
    if (drive == 255) {
        /* Priority : ROM > DSK 0 > TAP — matches firmware logic. */
        if      (loci->mnt_mounted[LOCI_MNT_ROM]) drive = LOCI_MNT_ROM;
        else if (loci->mnt_mounted[0])            drive = 0;
        else if (loci->mnt_mounted[LOCI_MNT_TAP]) drive = LOCI_MNT_TAP;
        else { api_return_errno(loci, LOCI_ENOENT); return; }
    }
    if (drive >= LOCI_MNT_MAX || !loci->mnt_mounted[drive]) {
        api_return_errno(loci, LOCI_ENOENT);
        return;
    }
    const char* path = loci->mnt_paths[drive];
    /* Firmware reports the directory part — strip trailing filename if any.
     * Find the rightmost '/'; length = position + 1 (include the slash). */
    const char* slash = strrchr(path, '/');
    size_t len = slash ? (size_t)(slash - path + 1) : strlen(path);
    if (len > 255) len = 255;

    /* Push terminator then bytes in reverse so reading forward from
     * xstack_ptr yields the path. */
    xstack_zero(loci);
    uint8_t zero = 0;
    if (!xstack_push_n(loci, &zero, 1)) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    for (size_t i = len; i > 0; i--) {
        uint8_t c = (uint8_t)path[i - 1];
        if (!xstack_push_n(loci, &c, 1)) {
            api_return_errno(loci, LOCI_EINVAL);
            return;
        }
    }
    api_return_ax(loci, (uint16_t)len);
}

/* 0x90 MOUNT: A=drive (0-5), xstack=path. Stores path in slot. */
static void op_mount(loci_t* loci) {
    uint8_t drive = loci->regs[LOCI_REG_API_A];
    char path[256];
    if (!pop_zstring(loci, path, sizeof(path))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    if (drive >= LOCI_MNT_MAX) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    /* Verify the file exists. When sdimg is active, extract to /tmp so
     * the existing tap_open/dsk_open POSIX paths can use the result. */
    char host_path[512];
    if (loci->sdimg) {
        if (!sdimg_extract_to_temp(loci, path, host_path, sizeof(host_path))) {
            api_return_errno(loci, LOCI_ENOENT);
            return;
        }
    } else {
        if (!resolve_path(loci, path, host_path, sizeof(host_path))) {
            api_return_errno(loci, LOCI_EACCES);
            return;
        }
        if (access(host_path, F_OK) != 0) {
            api_return_errno(loci, LOCI_ENOENT);
            return;
        }
    }
    /* Sprint 34af: when the tape slot is targeted, auto-open the file
     * so the TAP API ops (0x92-94) can read from it. */
    if (drive == LOCI_MNT_TAP) {
        if (!tap_open(loci, host_path)) {
            api_return_errno(loci, LOCI_EIO);
            return;
        }
        /* Sprint 34ao: also notify the host so CLOAD ROM patches can
         * find the tape data (LOCI's tap_fp is for $0315-$0317 only). */
        if (loci->tape_mount_cb) {
            loci->tape_mount_cb(loci->tape_mount_ctx, host_path);
        }
    }
    /* Sprint 34ae: disk drives 0-3 → open the DSK image so the bus
     * registers can later stream sectors. */
    if (drive < 4) {
        if (!dsk_open(loci, drive, host_path)) {
            api_return_errno(loci, LOCI_EIO);
            return;
        }
    }
    loci->mnt_mounted[drive] = true;
    strncpy(loci->mnt_paths[drive], path, sizeof(loci->mnt_paths[drive]) - 1);
    loci->mnt_paths[drive][sizeof(loci->mnt_paths[drive]) - 1] = '\0';
    api_return_ax(loci, 0);
}

/* 0x91 UMOUNT: A=drive. Clears slot. */
static void op_umount(loci_t* loci) {
    uint8_t drive = loci->regs[LOCI_REG_API_A];
    if (drive < LOCI_MNT_MAX) {
        if (drive == LOCI_MNT_TAP) tap_close(loci);
        if (drive < 4)             dsk_close(loci, drive);
        loci->mnt_mounted[drive] = false;
        loci->mnt_paths[drive][0] = '\0';
    }
    api_return_ax(loci, 0);
}

/* 0x1B UNLINK: xstack top = path. */
static void op_unlink(loci_t* loci) {
    if (loci->sdimg) {
        char path[260] = {0};
        pop_zstring(loci, path, sizeof(path));
        if (path[0] == 0) { api_return_errno(loci, LOCI_EINVAL); return; }
        const char* p = path;
        if (p[0] && p[1] == ':') p += 2;
        while (*p == '/' || *p == '\\') p++;
        int r = loci_sdimg_unlink((loci_sdimg_t*)loci->sdimg, p);
        if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }
        api_return_ax(loci, 0);
        return;
    }
    char path[260];
    if (!pop_zstring(loci, path, sizeof(path))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    char host_path[512];
    if (!resolve_path(loci, path, host_path, sizeof(host_path))) {
        api_return_errno(loci, LOCI_EACCES);
        return;
    }
    if (unlink(host_path) != 0) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    api_return_ax(loci, 0);
}

/* 0x1C RENAME: xstack contains new path (top) then old path (below). */
static void op_rename(loci_t* loci) {
    if (loci->sdimg) {
        char new_path[260] = {0};
        char old_path[260] = {0};
        pop_zstring(loci, new_path, sizeof(new_path));
        pop_zstring(loci, old_path, sizeof(old_path));
        if (old_path[0] == 0 || new_path[0] == 0) {
            api_return_errno(loci, LOCI_EINVAL); return;
        }
        const char* p_old = old_path;
        const char* p_new = new_path;
        if (p_old[0] && p_old[1] == ':') p_old += 2;
        while (*p_old == '/' || *p_old == '\\') p_old++;
        if (p_new[0] && p_new[1] == ':') p_new += 2;
        while (*p_new == '/' || *p_new == '\\') p_new++;
        int r = loci_sdimg_rename((loci_sdimg_t*)loci->sdimg, p_old, p_new);
        if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }
        api_return_ax(loci, 0);
        return;
    }
    char new_path[260];
    if (!pop_zstring(loci, new_path, sizeof(new_path))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    char old_path[260];
    if (!pop_zstring(loci, old_path, sizeof(old_path))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    char host_new[512], host_old[512];
    if (!resolve_path(loci, new_path, host_new, sizeof(host_new)) ||
        !resolve_path(loci, old_path, host_old, sizeof(host_old))) {
        api_return_errno(loci, LOCI_EACCES);
        return;
    }
    if (rename(host_old, host_new) != 0) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    api_return_ax(loci, 0);
}

/* ─── Dir API + uname (Sprint 34ac) ───────────────────────────── */

static int alloc_dir(loci_t* loci) {
    for (int i = 0; i < LOCI_DIR_MAX; i++) {
        if (loci->dirs[i] == NULL) return i;
    }
    return -1;
}

static DIR* dir_fd_to_handle(loci_t* loci, int fd) {
    int idx = fd - LOCI_DIR_OFFSET;
    if (idx < 0 || idx >= LOCI_DIR_MAX) return NULL;
    return (DIR*)loci->dirs[idx];
}

/* 0x80 OPENDIR: xstack=path. Returns dir_fd >= LOCI_DIR_OFFSET. */
static void op_opendir(loci_t* loci) {
    if (loci->sdimg) { op_opendir_sdimg(loci); return; }
    char path[260];
    if (!pop_zstring(loci, path, sizeof(path))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    char host_path[512];
    if (!resolve_path(loci, path, host_path, sizeof(host_path))) {
        api_return_errno(loci, LOCI_EACCES);
        return;
    }
    DIR* d = opendir(host_path);
    if (!d) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    int slot = alloc_dir(loci);
    if (slot < 0) {
        closedir(d);
        api_return_errno(loci, LOCI_EMFILE);
        return;
    }
    loci->dirs[slot] = d;
    loci->dir_kind[slot] = 1;
    /* Track resolved path so op_readdir stat()s via the correct base
     * (sprint 34c R1, fix from senior review post-34b5). Truncation here
     * would only re-introduce the prior buggy behavior for the rare case
     * of host_path > 255 chars — log it. */
    size_t pn = strlen(host_path);
    if (pn >= sizeof(loci->dirs_path[slot])) pn = sizeof(loci->dirs_path[slot]) - 1;
    memcpy(loci->dirs_path[slot], host_path, pn);
    loci->dirs_path[slot][pn] = '\0';
    api_return_ax(loci, (uint16_t)(slot + LOCI_DIR_OFFSET));
}

/* 0x81 CLOSEDIR: A=dir_fd. */
static void op_closedir(loci_t* loci) {
    if (loci->sdimg) { op_closedir_sdimg(loci); return; }
    int fd = loci->regs[LOCI_REG_API_A];
    DIR* d = dir_fd_to_handle(loci, fd);
    if (!d) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    closedir(d);
    loci->dirs[fd - LOCI_DIR_OFFSET] = NULL;
    loci->dir_kind[fd - LOCI_DIR_OFFSET] = 0;
    loci->dirs_path[fd - LOCI_DIR_OFFSET][0] = '\0';
    api_return_ax(loci, 0);
}

/* 0x82 READDIR: A=dir_fd. Pushes a dirent struct (72 bytes) onto xstack.
 * Skips "." and "..". On end-of-dir, returns empty name. */
static void op_readdir(loci_t* loci) {
    if (loci->sdimg) { op_readdir_sdimg(loci); return; }
    int fd = loci->regs[LOCI_REG_API_A];
    DIR* d = dir_fd_to_handle(loci, fd);
    if (!d) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }

    struct dirent* de = NULL;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".")  == 0) continue;
        if (strcmp(de->d_name, "..") == 0) continue;
        break;
    }

    /* Build dirent payload (72 bytes) directly in the xstack tail. */
    uint8_t dirent_buf[LOCI_DIRENT_SIZE] = {0};
    /* d_fd: int16 at offset 0 */
    dirent_buf[0] = (uint8_t)(fd & 0xFF);
    dirent_buf[1] = (uint8_t)((fd >> 8) & 0xFF);

    if (de) {
        /* d_name: 64 bytes at offset 2 */
        size_t nl = strlen(de->d_name);
        if (nl > LOCI_DIR_NAME_LEN - 1) nl = LOCI_DIR_NAME_LEN - 1;
        memcpy(&dirent_buf[2], de->d_name, nl);
        dirent_buf[2 + nl] = '\0';

        /* d_attrib: at offset 66. Check via stat. Use the dir's resolved
         * path (sprint 34c R1) so entries inside a sub-directory get
         * stat'd via "<root>/<sub>/<entry>" and not "<root>/<entry>". */
        uint8_t attrib = 0;
        struct stat st;
        char fullpath[768];
        const char* base = loci->dirs_path[fd - LOCI_DIR_OFFSET];
        if (!base[0]) base = loci->flash_root[0] ? loci->flash_root : ".";
        snprintf(fullpath, sizeof(fullpath), "%s/%s", base, de->d_name);
        if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            attrib = LOCI_AM_DIR;
        }
        dirent_buf[66] = attrib;
        dirent_buf[67] = 0;   /* reserved */

        /* d_size: uint32 at offset 68 */
        uint32_t sz = 0;
        if (stat(fullpath, &st) == 0) sz = (uint32_t)st.st_size;
        memcpy(&dirent_buf[68], &sz, 4);
    }
    /* (else: d_name stays empty, d_size = 0 — signals end-of-dir to caller.) */

    xstack_zero(loci);
    /* Push the 72 bytes so they fill xstack[XSTACK_SIZE - 72 .. XSTACK_SIZE - 1]. */
    loci->xstack_ptr = (uint16_t)(LOCI_XSTACK_SIZE - LOCI_DIRENT_SIZE);
    memcpy(&loci->xstack[loci->xstack_ptr], dirent_buf, LOCI_DIRENT_SIZE);
    xstack_sync(loci);
    api_return_ax(loci, 0);
}

/* 0x83 MKDIR: xstack=path. */
static void op_mkdir(loci_t* loci) {
    if (loci->sdimg) {
        char path[260] = {0};
        pop_zstring(loci, path, sizeof(path));
        if (path[0] == 0) { api_return_errno(loci, LOCI_EINVAL); return; }
        const char* p = path;
        if (p[0] && p[1] == ':') p += 2;
        while (*p == '/' || *p == '\\') p++;
        int r = loci_sdimg_mkdir((loci_sdimg_t*)loci->sdimg, p);
        if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }
        api_return_ax(loci, 0);
        return;
    }
    char path[260];
    if (!pop_zstring(loci, path, sizeof(path))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    char host_path[512];
    if (!resolve_path(loci, path, host_path, sizeof(host_path))) {
        api_return_errno(loci, LOCI_EACCES);
        return;
    }
    if (mkdir(host_path, 0755) != 0) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    api_return_ax(loci, 0);
}

/* 0x98 UNAME: pushes 5 fixed-size strings onto xstack (in firmware push
 * order: machine, version, release, nodename, sysname — sysname on top).
 * Returns the final xstack_ptr in A. */
static void op_uname(loci_t* loci) {
    static const char machine[25]  = "Phosphoric Emulator     ";
    static const char version[9]   = "0.1     ";   /* build version */
    static const char release[9]   = "1.16.27 ";
    static const char nodename[9]  = "oric    ";
    static const char sysname[17]  = "Phosphoric LOCI ";

    xstack_zero(loci);
    if (!xstack_push_n(loci, machine,  sizeof(machine))  ||
        !xstack_push_n(loci, version,  sizeof(version))  ||
        !xstack_push_n(loci, release,  sizeof(release))  ||
        !xstack_push_n(loci, nodename, sizeof(nodename)) ||
        !xstack_push_n(loci, sysname,  sizeof(sysname))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    api_return_ax(loci, loci->xstack_ptr);
}

/* ─── TAP cassette (Sprint 34af) ──────────────────────────────── */

/* Open the host TAP file and seed the read counter. */
static bool tap_open(loci_t* loci, const char* host_path) {
    if (loci->tap_fp) {
        fclose((FILE*)loci->tap_fp);
        loci->tap_fp = NULL;
    }
    FILE* fp = fopen(host_path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return false; }
    loci->tap_fp = fp;
    loci->tap_size = (uint32_t)sz;
    loci->tap_counter = 0;
    loci->tap_stat = 0;          /* ready, not busy, not protected */
    return true;
}

static void tap_close(loci_t* loci) {
    if (loci->tap_fp) {
        fclose((FILE*)loci->tap_fp);
        loci->tap_fp = NULL;
    }
    loci->tap_size = 0;
    loci->tap_counter = 0;
    loci->tap_stat = LOCI_TAP_STAT_NOT_READY;
}

/* Bus interface for $0315-$0317. Sprint 34af leaves the bit-level
 * stream unimplemented (CMD/STAT just latch and report idle) — the
 * LOCI ROM uses these for cassette PLAY mode which needs cycle-accurate
 * VIA interaction we don't emulate yet. */
uint8_t loci_tap_read(loci_t* loci, uint16_t address) {
    if (!loci || !loci->enabled) return 0xFF;
    switch (address) {
        case LOCI_TAP_IO_CMD:  return loci->tap_cmd;
        case LOCI_TAP_IO_STAT:
            return loci->tap_fp ? loci->tap_stat
                                : (uint8_t)(LOCI_TAP_STAT_NOT_READY);
        case LOCI_TAP_IO_DATA:
            /* Streamed bits not implemented — return 0 to keep the 6502
             * out of a tight wait loop. */
            return 0;
    }
    return 0xFF;
}

void loci_tap_write(loci_t* loci, uint16_t address, uint8_t value) {
    if (!loci || !loci->enabled) return;
    if (address == LOCI_TAP_IO_CMD) {
        loci->tap_cmd = value;
        /* Rewind = back to start. */
        if (value == LOCI_TAP_CMD_REW && loci->tap_fp) {
            fseek((FILE*)loci->tap_fp, 0, SEEK_SET);
            loci->tap_counter = 0;
        }
    }
    /* DATA writes (record) and STAT writes are ignored in 34af. */
}

/* 0x92 TAP_SEEK: pos in AXSREG (clamped to size-1), returns final pos. */
static void op_tap_seek(loci_t* loci) {
    if (!loci->tap_fp) {
        api_return_errno(loci, LOCI_ENODEV);
        return;
    }
    uint32_t pos = (uint32_t)loci->regs[LOCI_REG_API_A]
                 | ((uint32_t)loci->regs[LOCI_REG_API_X]    <<  8)
                 | ((uint32_t)loci->regs[LOCI_REG_API_SREG] << 16)
                 | ((uint32_t)loci->regs[LOCI_REG_API_SREG_HI] << 24);
    if (loci->tap_size > 0 && pos >= loci->tap_size) {
        pos = loci->tap_size - 1;
    }
    if (fseek((FILE*)loci->tap_fp, (long)pos, SEEK_SET) != 0) {
        api_return_errno(loci, LOCI_EIO);
        return;
    }
    loci->tap_counter = pos;
    api_return_axsreg(loci, pos);
}

/* 0x93 TAP_TELL: returns counter in AXSREG. */
static void op_tap_tell(loci_t* loci) {
    api_return_axsreg(loci, loci->tap_counter);
}

/* 0x94 TAP_READ_HEADER: scan forward for sync mark 16 16 16 24, then
 * read the next 16-byte header. Pushes header on xstack, returns the
 * position of the sync mark in AXSREG. ENODEV/ENOENT on errors. */
static void op_tap_read_header(loci_t* loci) {
    if (!loci->tap_fp) {
        api_return_errno(loci, LOCI_ENODEV);
        return;
    }
    if (loci->tap_size < LOCI_TAP_HEADER_SIZE + 4) {
        api_return_errno(loci, LOCI_ENOENT);
        return;
    }

    /* Resume scan at tap_counter. */
    FILE* fp = (FILE*)loci->tap_fp;
    fseek(fp, (long)loci->tap_counter, SEEK_SET);

    /* Stream-scan one byte at a time keeping a 4-byte sliding window. */
    uint8_t w[4] = {0};
    int filled = 0;
    long sync_end = -1;
    long pos = (long)loci->tap_counter;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        w[0] = w[1]; w[1] = w[2]; w[2] = w[3]; w[3] = (uint8_t)c;
        pos++;
        if (filled < 4) { filled++; continue; }
        if (w[0] == 0x16 && w[1] == 0x16 && w[2] == 0x16 && w[3] == 0x24) {
            sync_end = pos;   /* points just past the 0x24 */
            break;
        }
    }
    if (sync_end < 0) {
        api_return_errno(loci, LOCI_ENOENT);
        return;
    }

    /* Read the 16-byte header that immediately follows. */
    uint8_t header[LOCI_TAP_HEADER_SIZE] = {0};
    size_t hr = fread(header, 1, LOCI_TAP_HEADER_SIZE, fp);
    if (hr != LOCI_TAP_HEADER_SIZE) {
        api_return_errno(loci, LOCI_ENOENT);
        return;
    }
    /* Sanitise the filename slice (last 16 bytes — actually offsets 9..24
     * in firmware; we mirror its safety pass). */
    for (int i = 9; i < LOCI_TAP_HEADER_SIZE; i++) {
        uint8_t ch = header[i];
        if (ch != 0 && (ch < 32 || ch >= 128)) header[i] = '?';
    }
    /* Advance counter past the header for subsequent calls. */
    loci->tap_counter = (uint32_t)(sync_end + LOCI_TAP_HEADER_SIZE);

    /* Push header on xstack (firmware: sync_pos - 4 = start of sync mark). */
    xstack_zero(loci);
    loci->xstack_ptr = (uint16_t)(LOCI_XSTACK_SIZE - LOCI_TAP_HEADER_SIZE);
    memcpy(&loci->xstack[loci->xstack_ptr], header, LOCI_TAP_HEADER_SIZE);
    xstack_sync(loci);
    api_return_axsreg(loci, (uint32_t)(sync_end - 4));
}

/* DRQ / INTRQ callbacks bridging fdc_t → loci.dsk_*. */
static void loci_fdc_set_drq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (l) l->dsk_drq = 0x00;
}
static void loci_fdc_clr_drq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (l) l->dsk_drq = 0x80;
}
static void loci_fdc_set_intrq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (!l) return;
    l->dsk_intrq = 0x00;
    /* 34ay : si INTENA déjà actif quand le FDC fire INTRQ, assert CPU
     * IRQ aussi — pas uniquement sur CTRL write. Sinon le Microdisc
     * ROM polls infiniment STATUS sans recevoir d'IRQ. */
    if (l->dsk_intena && l->dsk_cpu_irq_set) {
        l->dsk_cpu_irq_set(l->dsk_bus_ctx);
    }
}
static void loci_fdc_clr_intrq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (!l) return;
    l->dsk_intrq = 0x80;
    /* Clear CPU IRQ aussi (cohérent level-triggered). */
    if (l->dsk_cpu_irq_clr) {
        l->dsk_cpu_irq_clr(l->dsk_bus_ctx);
    }
}

/* ─── DSK WD1793 cycle-accurate (Sprint 34aw) ──────────────────────
 *
 * Backed by the shared fdc_t module in src/storage/disk.c (same WD1793
 * core that drives the real Microdisc card). Each of the 4 LOCI virtual
 * drives keeps its DSK image in RAM (loaded at mount time). On a CTRL
 * write that changes the active drive, we re-point the FDC at the new
 * drive's buffer via fdc_set_disk(). The 6502 then reads/writes through
 * the standard $0310-$0313 register window and the FDC handles the
 * Restore / Seek / Step / ReadSector / WriteSector / Force commands. */

static void loci_apply_dsk_selection(loci_t* loci) {
    uint8_t drv = loci->dsk_selected;
    if (drv >= 4) return;
    fdc_set_disk(&loci->dsk_fdc,
                 loci->dsk_image[drv],
                 loci->dsk_image_size[drv]);
    loci->dsk_fdc.tracks            = loci->dsk_tracks[drv];
    loci->dsk_fdc.sectors_per_track = loci->dsk_sectors[drv];
}

static bool dsk_open(loci_t* loci, uint8_t drive, const char* host_path) {
    if (drive >= 4) return false;
    /* Close previous handle / free previous buffer for this drive. */
    if (loci->dsk_fp[drive]) {
        fclose((FILE*)loci->dsk_fp[drive]);
        loci->dsk_fp[drive] = NULL;
    }
    if (loci->dsk_image[drive]) {
        free(loci->dsk_image[drive]);
        loci->dsk_image[drive] = NULL;
        loci->dsk_image_size[drive] = 0;
    }
    /* Sprint 34aw+ : utilise sedoric_load qui gère le format MFM_DISK
     * (header 256 octets + tracks MFM) — convertit en sector array plat
     * que le fdc_t comprend directement. */
    sedoric_disk_t* sd = sedoric_load(host_path);
    if (sd) {
        loci->dsk_image[drive]      = sd->data;   /* steal ownership */
        loci->dsk_image_size[drive] = sd->size;
        loci->dsk_tracks[drive]     = sd->tracks;
        loci->dsk_sectors[drive]    = sd->sectors;
        free(sd);
        loci->dsk_fp[drive] = fopen(host_path, "r+b");
        if (!loci->dsk_fp[drive]) loci->dsk_fp[drive] = fopen(host_path, "rb");
    } else {
        /* Fallback raw image. */
        FILE* fp = fopen(host_path, "rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0 || sz > (long)(1 * 1024 * 1024)) { fclose(fp); return false; }
        uint8_t* buf = (uint8_t*)malloc((size_t)sz);
        if (!buf) { fclose(fp); return false; }
        if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
            free(buf); fclose(fp); return false;
        }
        loci->dsk_fp[drive]         = fp;
        loci->dsk_image[drive]      = buf;
        loci->dsk_image_size[drive] = (uint32_t)sz;
    }
    log_info("LOCI dsk_open drive %d: %s (%u bytes, %d tracks, %d sectors)",
             drive, host_path, loci->dsk_image_size[drive],
             loci->dsk_tracks[drive], loci->dsk_sectors[drive]);
    if (loci->dsk_selected == drive) {
        loci_apply_dsk_selection(loci);
    }
    return true;
}

static void dsk_close(loci_t* loci, uint8_t drive) {
    if (drive >= 4) return;
    if (loci->dsk_fp[drive]) {
        fclose((FILE*)loci->dsk_fp[drive]);
        loci->dsk_fp[drive] = NULL;
    }
    if (loci->dsk_image[drive]) {
        free(loci->dsk_image[drive]);
        loci->dsk_image[drive] = NULL;
        loci->dsk_image_size[drive] = 0;
    }
    if (loci->dsk_selected == drive) {
        fdc_set_disk(&loci->dsk_fdc, NULL, 0);
    }
}

uint8_t loci_dsk_read(loci_t* loci, uint16_t address) {
    if (!loci || !loci->enabled) return 0xFF;
    switch (address) {
        case LOCI_DSK_IO_CMD: {
            /* STATUS = FDC status OR'd with NOT_READY when the active
             * drive has no image mounted (the FDC itself doesn't know
             * about LOCI mount state). */
            uint8_t s = fdc_read(&loci->dsk_fdc, 0);
            if (loci->dsk_selected < 4 && !loci->dsk_image[loci->dsk_selected]) {
                s |= LOCI_DSK_STAT_NOT_READY;
            }
            return s;
        }
        case LOCI_DSK_IO_TRACK: return fdc_read(&loci->dsk_fdc, 1);
        case LOCI_DSK_IO_SECT:  return fdc_read(&loci->dsk_fdc, 2);
        case LOCI_DSK_IO_DATA:  return fdc_read(&loci->dsk_fdc, 3);
        case LOCI_DSK_IO_CTRL:  return loci->dsk_intrq | 0x7F;  /* IRQ status read */
        case LOCI_DSK_IO_DRQ:   return loci->dsk_drq | 0x7F;
    }
    return 0xFF;
}

void loci_dsk_write(loci_t* loci, uint16_t address, uint8_t value) {
    if (!loci || !loci->enabled) return;
    switch (address) {
        case LOCI_DSK_IO_CMD:   fdc_write(&loci->dsk_fdc, 0, value); break;
        case LOCI_DSK_IO_TRACK: fdc_write(&loci->dsk_fdc, 1, value); break;
        case LOCI_DSK_IO_SECT:  fdc_write(&loci->dsk_fdc, 2, value); break;
        case LOCI_DSK_IO_DATA:  fdc_write(&loci->dsk_fdc, 3, value); break;
        case LOCI_DSK_IO_CTRL: {
            /* Sprint 34ax : sémantique Microdisc complète sur $0314.
             *   bit 0 = INTENA  (IRQ enable)
             *   bit 1 = ROMDIS  (0 = BASIC ROM disabled, romdis=true)
             *   bit 3 = DENSITY
             *   bit 4 = SIDE    (0 ou 1)
             *   bits 5-6 = DRIVE select (0..3)
             *   bit 7 = EPROM   (0 = overlay ON, 1 = overlay OFF)
             */
            loci->dsk_ctrl = value;
            loci->dsk_intena = (value & 0x01) != 0;
            uint8_t side = (value & 0x10) ? 1 : 0;
            bool diskrom = (value & 0x80) == 0;   /* bit 7 active-low : EPROM */
            /* 34az : ROMDIS dynamique (bit 1 active-low), miroir microdisc
             * natif. Sedoric re-map BASIC ROM en $C000-$DFFF via ROMDIS=1
             * pour appeler des routines BASIC tout en gardant ROM Microdisc
             * en $E000 via EPROM=0. Hardcoder basic_rom_disabled=true bloquait
             * Sedoric stage 2 (boucle $04F7 → JSR vers RAM noise). */
            bool romdis = (value & 0x02) == 0;
            uint8_t newdrv = (uint8_t)((value & LOCI_DSK_CTRL_DRV_SEL_MASK)
                                       >> LOCI_DSK_CTRL_DRV_SEL_SHIFT);
            loci->dsk_fdc.side = side;
            if (newdrv != loci->dsk_selected) {
                loci->dsk_selected = newdrv;
                loci_apply_dsk_selection(loci);
            }
            if (loci->dsk_sync_overlay) {
                loci->dsk_sync_overlay(loci->dsk_bus_ctx, romdis, diskrom);
            }
            /* INTENA + INTRQ active → assert CPU IRQ ; sinon clear. */
            if (loci->dsk_intena && loci->dsk_intrq == 0x00) {
                if (loci->dsk_cpu_irq_set) loci->dsk_cpu_irq_set(loci->dsk_bus_ctx);
            } else {
                if (loci->dsk_cpu_irq_clr) loci->dsk_cpu_irq_clr(loci->dsk_bus_ctx);
            }
            break;
        }
        case LOCI_DSK_IO_DRQ:   loci->dsk_drq = value; break;
    }
}

/* ─── MIA_BOOT — runtime ROM swap (Sprint 34ad) ──────────────── */

/* Resolve the BASIC ROM filename used by MIA_BOOT, given the settings:
 *   - if a ROM is mounted on slot 5 (LOCI_MNT_ROM): use that path verbatim
 *   - else if B11 bit set: "basic11b.rom"
 *   - else                : "basic10.rom"
 * The returned path goes through the sandbox resolver before host I/O. */
static const char* derive_basic_rom_path(loci_t* loci, uint8_t settings) {
    if (loci->mnt_mounted[LOCI_MNT_ROM]) {
        return loci->mnt_paths[LOCI_MNT_ROM];
    }
    return (settings & LOCI_BOOT_B11) ? "basic11b.rom" : "basic10.rom";
}

/* 0xA0 MIA_BOOT: A=settings flags. Triggers a ROM swap+CPU reset via
 * the registered callback. */
static void op_mia_boot(loci_t* loci) {
    uint8_t settings = loci->regs[LOCI_REG_API_A];
    loci->boot_settings = settings;

    /* RESUME: caller just wants to keep running with the current ROM. */
    if (settings & LOCI_BOOT_RESUME) {
        api_return_ax(loci, 0);
        return;
    }

    /* No callback wired = nothing we can do but acknowledge.
     * (Phosphoric in headless test mode hits this path.) */
    if (!loci->rom_swap_cb) {
        api_return_ax(loci, 0);
        return;
    }

    /* Resolve BASIC ROM path. When sdimg is active, extract from FAT
     * image into a temp file the rom_swap_cb can open with fopen(). */
    const char* guest_rom = derive_basic_rom_path(loci, settings);
    char host_rom[512];
    if (loci->sdimg) {
        if (!sdimg_extract_to_temp(loci, guest_rom, host_rom, sizeof(host_rom))) {
            api_return_errno(loci, LOCI_ENOENT);
            return;
        }
    } else if (!resolve_path(loci, guest_rom, host_rom, sizeof(host_rom))) {
        api_return_errno(loci, LOCI_EACCES);
        return;
    }

    if (!loci->rom_swap_cb(loci->rom_swap_ctx, host_rom, 0xC000)) {
        api_return_errno(loci, LOCI_EIO);
        return;
    }

    /* FDC bit: also load Microdisc device ROM at $A000. Best-effort —
     * absence is not fatal (some setups boot without disk). */
    if (settings & LOCI_BOOT_FDC) {
        const char* disc_path = "microdis.rom";
        char host_disc[512];
        bool ok = false;
        if (loci->sdimg) {
            ok = sdimg_extract_to_temp(loci, disc_path, host_disc, sizeof(host_disc));
        } else {
            ok = resolve_path(loci, disc_path, host_disc, sizeof(host_disc));
        }
        if (ok) {
            loci->rom_swap_cb(loci->rom_swap_ctx, host_disc, 0xA000);
        }
    }

    api_return_ax(loci, 0);
}

/* ─── Sprint 34au : 7 tuning / config stubs ──────────────────────
 *
 * Ces 7 ops étaient renvoyées avec ENOSYS jusqu'ici. Sur la vraie
 * carte LOCI, elles configurent le coprocesseur Pi Pico (fréquence
 * Phi2, codepage OEM, options stdin, lanes de mapping mémoire).
 * Côté émulateur, Phosphoric n'a pas d'équivalent hardware à régler,
 * mais on accepte les params et on renvoie succès — ainsi le firmware
 * LOCI peut continuer son flow d'init sans erreur.
 *
 * Side effects : log_debug pour observabilité, op_count incrémenté
 * comme pour toute op.
 */

/* 0x02 CPU_PHI2 — lecture / écriture de la fréquence Phi2 du 6502.
 * Le firmware utilise cet appel pour piloter l'horloge Pi Pico. On
 * retourne 1 MHz dans AXSREG (Hz) — la fréquence réelle de Phosphoric. */
static void op_cpu_phi2(loci_t* loci) {
    uint8_t requested = loci->regs[LOCI_REG_API_A];
    log_debug("LOCI op_cpu_phi2: requested divisor=%u, returning 1 MHz", requested);
    api_return_axsreg(loci, 1000000u);
}

/* 0x03 OEM_CODEPAGE — sélectionne la codepage pour les translations
 * de filenames (FAT short names). Phosphoric ne fait pas de
 * translation → on accepte tout, return 0 OK. */
static void op_oem_codepage(loci_t* loci) {
    uint8_t cp = loci->regs[LOCI_REG_API_A];
    log_debug("LOCI op_oem_codepage: cp=%u accepted (no translation done)", cp);
    api_return_ax(loci, 0);
}

/* 0x05 STDIN_OPT — options de la console (echo, line buffering, etc.).
 * Phosphoric n'a pas de console interactive LOCI → no-op success. */
static void op_stdin_opt(loci_t* loci) {
    uint8_t opt = loci->regs[LOCI_REG_API_A];
    log_debug("LOCI op_stdin_opt: opt=$%02X accepted", opt);
    api_return_ax(loci, 0);
}

/* Helper commun : MAP_TUNE_* prennent leurs params sur le xstack.
 * On vide le xstack et on renvoie succès — pas de hardware mapping à
 * tuner côté émulateur. */
static void op_map_tune_noop(loci_t* loci, const char* name) {
    log_debug("LOCI %s (xstack_ptr=%u) accepted, no hardware to tune",
              name, loci->xstack_ptr);
    xstack_zero(loci);
    api_return_ax(loci, 0);
}

/* 0xA1 MAP_TUNE_TMAP — sélectionne un mode de mapping mémoire. */
static void op_map_tune_tmap(loci_t* loci) { op_map_tune_noop(loci, "MAP_TUNE_TMAP"); }
/* 0xA2 MAP_TUNE_TIOR — règle les paramètres de lecture I/O. */
static void op_map_tune_tior(loci_t* loci) { op_map_tune_noop(loci, "MAP_TUNE_TIOR"); }
/* 0xA3 MAP_TUNE_TIOW — règle les paramètres d'écriture I/O. */
static void op_map_tune_tiow(loci_t* loci) { op_map_tune_noop(loci, "MAP_TUNE_TIOW"); }
/* 0xA4 MAP_TUNE_TIOD — règle les lanes de données I/O. */
static void op_map_tune_tiod(loci_t* loci) { op_map_tune_noop(loci, "MAP_TUNE_TIOD"); }
/* 0xA5 MAP_TUNE_TADR — règle les lanes d'adresses I/O. */
static void op_map_tune_tadr(loci_t* loci) { op_map_tune_noop(loci, "MAP_TUNE_TADR"); }

/* ─── dispatch ─────────────────────────────────────────────────── */

static void dispatch_op(loci_t* loci, uint8_t op) {
    loci->op_count[op]++;
    loci->active_op = op;
    switch (op) {
        case LOCI_OP_PIX_XREG:    op_pix_xreg(loci);     break;
        case LOCI_OP_CPU_PHI2:    op_cpu_phi2(loci);     break;  /* 34au */
        case LOCI_OP_OEM_CODEPAGE:op_oem_codepage(loci); break;  /* 34au */
        case LOCI_OP_STDIN_OPT:   op_stdin_opt(loci);    break;  /* 34au */
        case LOCI_OP_RNG_LRAND:   op_rng_lrand(loci);    break;
        case LOCI_OP_CLOCK:       op_clock(loci);        break;
        case LOCI_OP_CLK_GETRES:  op_clk_getres(loci);   break;
        case LOCI_OP_CLK_GETTIME: op_clk_gettime(loci);  break;
        case LOCI_OP_CLK_SETTIME: op_clk_settime(loci);  break;
        case LOCI_OP_OPEN:        op_open(loci);         break;
        case LOCI_OP_CLOSE:       op_close(loci);        break;
        case LOCI_OP_READ_XSTACK: op_read_xstack(loci);  break;
        case LOCI_OP_WRITE_XSTACK:op_write_xstack(loci); break;
        case LOCI_OP_LSEEK:       op_lseek(loci);        break;
        case LOCI_OP_UNLINK:      op_unlink(loci);       break;
        case LOCI_OP_RENAME:      op_rename(loci);       break;
        case LOCI_OP_READ_XRAM:   op_read_xram(loci);    break;
        case LOCI_OP_WRITE_XRAM:  op_write_xram(loci);   break;
        case LOCI_OP_MOUNT:       op_mount(loci);        break;
        case LOCI_OP_UMOUNT:      op_umount(loci);       break;
        case LOCI_OP_GETCWD:      op_getcwd(loci);       break;
        case LOCI_OP_OPENDIR:     op_opendir(loci);      break;
        case LOCI_OP_CLOSEDIR:    op_closedir(loci);     break;
        case LOCI_OP_READDIR:     op_readdir(loci);      break;
        case LOCI_OP_MKDIR:       op_mkdir(loci);        break;
        case LOCI_OP_UNAME:       op_uname(loci);        break;
        case LOCI_OP_MIA_BOOT:    op_mia_boot(loci);     break;
        case LOCI_OP_TAP_SEEK:    op_tap_seek(loci);     break;
        case LOCI_OP_TAP_TELL:    op_tap_tell(loci);     break;
        case LOCI_OP_TAP_READ_HEADER: op_tap_read_header(loci); break;
        case LOCI_OP_MAP_TUNE_TMAP:   op_map_tune_tmap(loci);  break;  /* 34au */
        case LOCI_OP_MAP_TUNE_TIOR:   op_map_tune_tior(loci);  break;  /* 34au */
        case LOCI_OP_MAP_TUNE_TIOW:   op_map_tune_tiow(loci);  break;  /* 34au */
        case LOCI_OP_MAP_TUNE_TIOD:   op_map_tune_tiod(loci);  break;  /* 34au */
        case LOCI_OP_MAP_TUNE_TADR:   op_map_tune_tadr(loci);  break;  /* 34au */
        default:
            log_debug("LOCI op $%02X (%s) — stubbed, returns ENOSYS",
                      op, op_name(op));
            /* api_return_errno posts the released stub via the
             * api_return_axsreg call chain. Crucial : without this
             * the 6502 JSR $03B0 reads the blocked stub forever
             * (the bug behind CR 2026-06-06 — InitKeyboard unreached). */
            api_return_errno(loci, LOCI_ENOSYS);
            break;
    }
    loci->active_op = 0;
}

/* ─── bus interface ────────────────────────────────────────────── */

uint8_t loci_read(loci_t* loci, uint16_t address) {
    if (!loci || !loci->enabled) return 0xFF;
    if (!loci_addr_in_mia(address)) return 0xFF;
    uint8_t off = (uint8_t)(address - LOCI_MIA_BASE);

    /* xram DMA window 0 — read RW0, then advance addr0 by step0 (signed). */
    if (off == LOCI_REG_RW0) {
        uint8_t v = loci->regs[LOCI_REG_RW0];
        int8_t step = (int8_t)loci->regs[LOCI_REG_STEP0];
        set_addr0(loci, (uint16_t)(get_addr0(loci) + step));
        refresh_rw0(loci);
        return v;
    }
    /* xram DMA window 1 — read RW1, advance addr1 by step1. */
    if (off == LOCI_REG_RW1) {
        uint8_t v = loci->regs[LOCI_REG_RW1];
        int8_t step = (int8_t)loci->regs[LOCI_REG_STEP1];
        set_addr1(loci, (uint16_t)(get_addr1(loci) + step));
        refresh_rw1(loci);
        return v;
    }

    /* API_STACK: a 6502 read pops the top byte. Firmware semantics:
     *   value = xstack[ptr]; ptr++ (clamped to LOCI_XSTACK_SIZE);
     *   regs[STACK] = next top byte (or 0 if empty).
     * This consumes whatever LOCI pushed (e.g. clock_gettime data). */
    if (off == LOCI_REG_API_STACK) {
        uint8_t v;
        if (loci->xstack_ptr >= LOCI_XSTACK_SIZE) {
            v = 0;
        } else {
            v = loci->xstack[loci->xstack_ptr];
            loci->xstack_ptr++;
        }
        xstack_sync(loci);
        return v;
    }

    return loci->regs[off];
}

void loci_write(loci_t* loci, uint16_t address, uint8_t value) {
    if (!loci || !loci->enabled) return;
    if (!loci_addr_in_mia(address)) return;
    uint8_t off = (uint8_t)(address - LOCI_MIA_BASE);

    /* xram DMA window writes : update xram[addr] then advance and
     * refresh RW. Address-set writes only refresh RW (no advance). */
    if (off == LOCI_REG_RW0) {
        loci->xram[get_addr0(loci)] = value;
        int8_t step = (int8_t)loci->regs[LOCI_REG_STEP0];
        set_addr0(loci, (uint16_t)(get_addr0(loci) + step));
        refresh_rw0(loci);
        return;
    }
    if (off == LOCI_REG_RW1) {
        loci->xram[get_addr1(loci)] = value;
        int8_t step = (int8_t)loci->regs[LOCI_REG_STEP1];
        set_addr1(loci, (uint16_t)(get_addr1(loci) + step));
        refresh_rw1(loci);
        return;
    }
    if (off == LOCI_REG_ADDR0_LO || off == LOCI_REG_ADDR0_HI) {
        loci->regs[off] = value;
        refresh_rw0(loci);
        return;
    }
    if (off == LOCI_REG_ADDR1_LO || off == LOCI_REG_ADDR1_HI) {
        loci->regs[off] = value;
        refresh_rw1(loci);
        return;
    }

    /* API_STACK: 6502 writes push onto xstack (grows downward). */
    if (off == LOCI_REG_API_STACK) {
        if (loci->xstack_ptr > 0) {
            loci->xstack[--loci->xstack_ptr] = value;
        }
        xstack_sync(loci);
        return;
    }

    loci->regs[off] = value;

    /* API_OP: writing here triggers a dispatch.
     *
     * Contract (mirrors firmware sys/mia.c:814-828) :
     *   - 0x00 zxstack()  : reset xstack + return ax=0 via released stub
     *   - 0xFF exit()     : install blocked stub permanently (6502 spins
     *                       at $03B0 — no released stub will ever be
     *                       posted; matches firmware which sets
     *                       stop_requested and never resolves the call)
     *   - other           : install blocked stub, then synchronous
     *                       handler; the handler MUST call api_return_*
     *                       which posts the released stub with A/X/SREG.
     *
     * Every path must end with either install_blocked (caller will
     * eventually un-spin) or install_released (handler done). Otherwise
     * the 6502 `JSR $03B0` fetches whatever zero-init garbage we have
     * in $03B0-9 and crashes. */
    if (off == LOCI_REG_API_OP) {
        if (value == 0x00) {
            /* zxstack: clear stack, ax=0, released stub. */
            xstack_zero(loci);
            api_return_ax(loci, 0);
            return;
        }
        if (value == 0xFF) {
            /* exit(): permanent block — 6502 spins forever at $03B0,
             * matching the firmware's stop_requested behaviour. */
            api_install_blocked_stub(loci);
            log_debug("LOCI: exit() called — 6502 will spin at $03B0");
            return;
        }
        /* Skip the sentinel marker (no-op, no dispatch). */
        if (value == LOCI_OP_RESET_SENTINEL) {
            return;
        }
        /* Normal op: blocked stub first, then handler resolves via
         * api_return_ax/axsreg/errno which install the released stub. */
        api_install_blocked_stub(loci);
        dispatch_op(loci, value);
    }
}

void loci_task(loci_t* loci) {
    /* Sprint 34y: dispatch is synchronous in loci_write. No async work. */
    (void)loci;
}
