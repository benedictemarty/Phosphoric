/**
 * @file loci_core.c
 * @brief LOCI core — init/lifecycle, action button, HID, errno/xstack/
 *        API return helpers, system / RTC / RNG ops, xram DMA, dispatch,
 *        bus interface ($03A0-$03BF).
 *
 * Sprint 34c R4 : mechanical split of loci.c (was monolithic 2380 LOC).
 * Helpers shared with loci_fs.c / loci_bus.c / loci_boot.c are declared
 * in include/io/loci_internal.h.
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif

#include "emulator.h"
#include "io/loci.h"
#include "io/loci_internal.h"
#include "io/loci_sdimg.h"
#include "utils/logging.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>

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
        case LOCI_OP_ADJ_SCAN:        return "ADJ_SCAN";
        default:                      return "?";
    }
}

/* ─── clock helpers ────────────────────────────────────────────── */

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ─── lifecycle ────────────────────────────────────────────────── */

static void seed_initial_stub(loci_t* loci) {
    loci->regs[LOCI_REG_INJECT0 + 0] = 0xB8;
    loci->regs[LOCI_REG_INJECT0 + 1] = 0x50;
    loci->regs[LOCI_REG_INJECT0 + 2] = 0x00;
    loci->regs[LOCI_REG_INJECT0 + 3] = 0xA9;
    loci->regs[LOCI_REG_INJECT0 + 4] = 0x00;
    loci->regs[LOCI_REG_INJECT0 + 5] = 0xA2;
    loci->regs[LOCI_REG_INJECT0 + 6] = 0x00;
    loci->regs[LOCI_REG_INJECT0 + 7] = 0x60;
    loci->regs[LOCI_REG_INJECT0 + 8] = 0x00;
    loci->regs[LOCI_REG_INJECT0 + 9] = 0x00;
}

bool loci_init(loci_t* loci) {
    if (!loci) return false;
    memset(loci, 0, sizeof(*loci));
    loci->xstack_ptr = LOCI_XSTACK_SIZE;
    loci->clock_start_us = now_us();
    loci->rng_state = loci->clock_start_us ^ 0xA5A5A5A5A5A5A5A5ULL;
    loci->kbd_xram = 0xFFFF;
    loci->mou_xram = 0xFFFF;
    loci->pad_xram = 0xFFFF;
    /* MIA bus-timing model: firmware boot defaults (tmap/tula=10, rest 0) and a
     * fully-open reliable window (any tior works → legacy behaviour). */
    loci->mia_tmap = 10;
    loci->mia_tula = 10;
    loci->mia_tior = 0;
    loci->mia_tiow = 0;
    loci->mia_tiod = 0;
    loci->mia_tadr = 0;
    loci->mia_tior_lo = 0;
    loci->mia_tior_hi = 31;
    seed_initial_stub(loci);
    fdc_init(&loci->dsk_fdc);
    loci->dsk_fdc.set_drq = loci_fdc_set_drq;
    loci->dsk_fdc.clr_drq = loci_fdc_clr_drq;
    loci->dsk_fdc.drq_userdata = loci;
    loci->dsk_fdc.set_intrq = loci_fdc_set_intrq;
    loci->dsk_fdc.clr_intrq = loci_fdc_clr_intrq;
    loci->dsk_fdc.intrq_userdata = loci;
    loci->dsk_drq   = 0x80;
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

void loci_cleanup(loci_t* loci) {
    if (!loci) return;
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
    if (loci->tap_fp) {
        fclose((FILE*)loci->tap_fp);
        loci->tap_fp = NULL;
    }
    for (int i = 0; i < 4; i++) {
        loci_dsk_flush(loci, (uint8_t)i);
        if (loci->dsk_fp[i]) {
            fclose((FILE*)loci->dsk_fp[i]);
            loci->dsk_fp[i] = NULL;
        }
        if (loci->dsk_image[i]) {
            free(loci->dsk_image[i]);
            loci->dsk_image[i] = NULL;
            loci->dsk_image_size[i] = 0;
        }
        loci->dsk_host_path[i][0] = '\0';
        loci->dsk_is_mfm[i] = false;
    }
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

static const uint8_t LOCI_ACTION_TRAP[6] = {
    0xB8, 0x50, 0xFE, 0x6C, 0xFA, 0xFF
};

void loci_action_button_short(loci_t* loci) {
    if (!loci || !loci->enabled) return;
    if (loci->action_active) return;
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

void set_errno(loci_t* loci, uint16_t e) {
    loci->regs[LOCI_REG_API_ERRNO_LO] = (uint8_t)(e & 0xFF);
    loci->regs[LOCI_REG_API_ERRNO_HI] = (uint8_t)(e >> 8);
}

void xstack_sync(loci_t* loci) {
    if (loci->xstack_ptr >= LOCI_XSTACK_SIZE) {
        loci->regs[LOCI_REG_API_STACK] = 0;
    } else {
        loci->regs[LOCI_REG_API_STACK] = loci->xstack[loci->xstack_ptr];
    }
}

void xstack_zero(loci_t* loci) {
    loci->xstack_ptr = LOCI_XSTACK_SIZE;
    xstack_sync(loci);
}

bool xstack_push_n(loci_t* loci, const void* data, size_t n) {
    if (n > loci->xstack_ptr) return false;
    loci->xstack_ptr -= (uint16_t)n;
    memcpy(&loci->xstack[loci->xstack_ptr], data, n);
    xstack_sync(loci);
    return true;
}

bool xstack_push_u32(loci_t* loci, uint32_t v) {
    return xstack_push_n(loci, &v, 4);
}

bool xstack_push_i32(loci_t* loci, int32_t v) {
    return xstack_push_n(loci, &v, 4);
}

/* ─── API return helpers (firmware-mirroring spin-window patches) */

void api_install_blocked_stub(loci_t* loci) {
    loci->regs[LOCI_REG_INJECT0 + 0] = 0xB8;
    loci->regs[LOCI_REG_INJECT0 + 1] = 0x50;
    loci->regs[LOCI_REG_INJECT0 + 2] = 0xFE;
    loci->regs[LOCI_REG_INJECT0 + 3] = 0xA9;
}

void api_install_released_stub(loci_t* loci) {
    loci->regs[LOCI_REG_INJECT0 + 0] = 0xB8;
    loci->regs[LOCI_REG_INJECT0 + 1] = 0x50;
    loci->regs[LOCI_REG_INJECT0 + 2] = 0x00;
    loci->regs[LOCI_REG_INJECT0 + 3] = 0xA9;
}

static void api_set_ax(loci_t* loci, uint16_t val) {
    loci->regs[LOCI_REG_API_A]    = (uint8_t)(val & 0xFF);
    loci->regs[LOCI_REG_INJECT0 + 5] = 0xA2;
    loci->regs[LOCI_REG_API_X]    = (uint8_t)((val >> 8) & 0xFF);
    loci->regs[LOCI_REG_INJECT0 + 7] = 0x60;
}

static void api_set_axsreg(loci_t* loci, uint32_t val) {
    api_set_ax(loci, (uint16_t)val);
    loci->regs[LOCI_REG_API_SREG]    = (uint8_t)((val >> 16) & 0xFF);
    loci->regs[LOCI_REG_API_SREG_HI] = (uint8_t)((val >> 24) & 0xFF);
}

void set_busy(loci_t* loci, bool busy) {
    if (busy) loci->regs[LOCI_REG_BUSY] |=  0x80;
    else      loci->regs[LOCI_REG_BUSY] &= ~0x80;
}

void api_return_ax(loci_t* loci, uint16_t val) {
    api_set_ax(loci, val);
    api_install_released_stub(loci);
    set_busy(loci, false);
}

void api_return_axsreg(loci_t* loci, uint32_t val) {
    api_set_axsreg(loci, val);
    api_install_released_stub(loci);
    set_busy(loci, false);
}

void api_return_errno(loci_t* loci, uint16_t e) {
    xstack_zero(loci);
    set_errno(loci, e);
    api_return_axsreg(loci, 0xFFFFFFFFu);
}

/* ─── System / RTC / RNG handlers ──────────────────────────────── */

void op_pix_xreg(loci_t* loci) {
    if (LOCI_XSTACK_SIZE - loci->xstack_ptr < 5) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    uint8_t device  = loci->xstack[LOCI_XSTACK_SIZE - 1];
    uint8_t channel = loci->xstack[LOCI_XSTACK_SIZE - 2];
    uint8_t addr    = loci->xstack[LOCI_XSTACK_SIZE - 3];

    if (device != 0) {
        xstack_zero(loci);
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }

    uint16_t word = (uint16_t)loci->xstack[LOCI_XSTACK_SIZE - 5] |
                    ((uint16_t)loci->xstack[LOCI_XSTACK_SIZE - 4] << 8);

    uint16_t route = (uint16_t)channel * 256 + addr;
    switch (route) {
        case 0: loci->kbd_xram = word; break;
        case 1: loci->mou_xram = word; break;
        case 2: loci->pad_xram = word; break;
        default: break;
    }

    xstack_zero(loci);
    api_return_ax(loci, 0);
}

/* ─── HID bridge (Sprint 34ag) ─────────────────────────────────── */

#define LOCI_HID_KEY_A             0x04
#define LOCI_HID_KEY_CONTROL_LEFT  0xE0

void loci_kbd_set_report(loci_t* loci, uint8_t modifier,
                          const uint8_t keycodes[6]) {
    if (!loci || !loci->enabled) return;
    if (loci->kbd_xram == 0xFFFF) return;
    if ((uint32_t)loci->kbd_xram + 32 > LOCI_XRAM_SIZE) return;

    uint8_t keys[32] = {0};
    bool any_key = false;
    for (int i = 0; i < 6; i++) {
        uint8_t kc = keycodes[i];
        if (kc >= LOCI_HID_KEY_A) {
            any_key = true;
            keys[kc >> 3] |= (uint8_t)(1u << (kc & 7));
        }
    }
    keys[LOCI_HID_KEY_CONTROL_LEFT >> 3] = modifier;
    if (!any_key && !modifier) keys[0] |= 0x01;
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

    uint8_t* m = &loci->xram[loci->mou_xram];
    m[0]  = buttons;
    m[1] = (uint8_t)(m[1] + (uint8_t)dx);
    m[2] = (uint8_t)(m[2] + (uint8_t)dy);
    m[3] = (uint8_t)(m[3] + (uint8_t)wheel);
    m[4] = (uint8_t)(m[4] + (uint8_t)pan);
}

void op_rng_lrand(loci_t* loci) {
    uint32_t v = (uint32_t)rand();
    v ^= (uint32_t)(rand() << 16);
    v &= 0x7FFFFFFFu;
    api_return_axsreg(loci, v);
}

void op_clock(loci_t* loci) {
    uint64_t now = now_us();
    uint64_t up  = (now >= loci->clock_start_us)
                       ? (now - loci->clock_start_us)
                       : 0;
    api_return_axsreg(loci, (uint32_t)(up / 10000ULL));
}

#define CLK_ID_REALTIME 0

void op_clk_getres(loci_t* loci) {
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

void op_clk_gettime(loci_t* loci) {
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

void op_clk_settime(loci_t* loci) {
    uint8_t clock_id = loci->regs[LOCI_REG_API_A];
    if (clock_id != CLK_ID_REALTIME) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
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

void refresh_rw0(loci_t* loci) {
    loci->regs[LOCI_REG_RW0] = loci->xram[get_addr0(loci)];
}
void refresh_rw1(loci_t* loci) {
    loci->regs[LOCI_REG_RW1] = loci->xram[get_addr1(loci)];
}

/* ─── dispatch ─────────────────────────────────────────────────── */

static void dispatch_op(loci_t* loci, uint8_t op) {
    loci->op_count[op]++;
    loci->active_op = op;
    switch (op) {
        case LOCI_OP_PIX_XREG:    op_pix_xreg(loci);     break;
        case LOCI_OP_CPU_PHI2:    op_cpu_phi2(loci);     break;
        case LOCI_OP_OEM_CODEPAGE:op_oem_codepage(loci); break;
        case LOCI_OP_STDIN_OPT:   op_stdin_opt(loci);    break;
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
        case LOCI_OP_MAP_TUNE_TMAP:   op_map_tune_tmap(loci);  break;
        case LOCI_OP_MAP_TUNE_TIOR:   op_map_tune_tior(loci);  break;
        case LOCI_OP_MAP_TUNE_TIOW:   op_map_tune_tiow(loci);  break;
        case LOCI_OP_MAP_TUNE_TIOD:   op_map_tune_tiod(loci);  break;
        case LOCI_OP_MAP_TUNE_TADR:   op_map_tune_tadr(loci);  break;
        case LOCI_OP_ADJ_SCAN:        op_adj_scan(loci);       break;
        default:
            log_debug("LOCI op $%02X (%s) — stubbed, returns ENOSYS",
                      op, op_name(op));
            api_return_errno(loci, LOCI_ENOSYS);
            break;
    }
    loci->active_op = 0;
}

/* ─── bus interface ────────────────────────────────────────────── */

/* ─── console UART $03A0-$03A2 (Sprint 36f) ───────────────────── */

static bool cons_rx_pending(const loci_t* loci) {
    return loci->cons_rx_head != loci->cons_rx_tail;
}

static uint8_t cons_rx_pop(loci_t* loci) {
    if (!cons_rx_pending(loci)) return 0;
    uint8_t v = loci->cons_rx[loci->cons_rx_tail];
    loci->cons_rx_tail = (uint8_t)((loci->cons_rx_tail + 1) % sizeof(loci->cons_rx));
    return v;
}

void loci_cons_inject(loci_t* loci, uint8_t ch) {
    if (!loci) return;
    uint8_t next = (uint8_t)((loci->cons_rx_head + 1) % sizeof(loci->cons_rx));
    if (next == loci->cons_rx_tail) return;   /* ring full — drop */
    loci->cons_rx[loci->cons_rx_head] = ch;
    loci->cons_rx_head = next;
}

static void cons_tx_byte(loci_t* loci, uint8_t ch) {
    if (ch == '\n' || loci->cons_tx_len >= sizeof(loci->cons_tx_line) - 1) {
        loci->cons_tx_line[loci->cons_tx_len] = '\0';
        if (loci->cons_tx_len > 0 || ch == '\n') {
            log_info("LOCI console: %s", loci->cons_tx_line);
        }
        loci->cons_tx_len = 0;
        if (ch != '\n' && ch != '\r') {
            loci->cons_tx_line[loci->cons_tx_len++] = (char)ch;
        }
        return;
    }
    if (ch != '\r') {
        loci->cons_tx_line[loci->cons_tx_len++] = (char)ch;
    }
}

uint8_t loci_read(loci_t* loci, uint16_t address) {
    if (!loci || !loci->enabled) return 0xFF;
    if (!loci_addr_in_mia(address)) return 0xFF;
    uint8_t off = (uint8_t)(address - LOCI_MIA_BASE);

    if (off == LOCI_REG_CONS_FLAGS) {
        /* Firmware (mia.c): bit 7 = TX writable, bit 6 = RX data ready.
         * Our TX sink never stalls, so bit 7 is always set. Reading also
         * preloads $03A2 with the head of the RX queue. */
        uint8_t flags = 0x80;
        if (cons_rx_pending(loci)) {
            flags |= 0x40;
            loci->regs[LOCI_REG_CONS_CHAR] = loci->cons_rx[loci->cons_rx_tail];
        }
        loci->regs[LOCI_REG_CONS_FLAGS] = flags;
        return flags;
    }
    if (off == LOCI_REG_CONS_CHAR) {
        /* Read consumes one RX byte (0 when the queue is empty). */
        uint8_t v = cons_rx_pop(loci);
        loci->regs[LOCI_REG_CONS_CHAR] =
            cons_rx_pending(loci) ? loci->cons_rx[loci->cons_rx_tail] : 0;
        return v;
    }

    if (off == LOCI_REG_RW0) {
        uint8_t v = loci->regs[LOCI_REG_RW0];
        int8_t step = (int8_t)loci->regs[LOCI_REG_STEP0];
        set_addr0(loci, (uint16_t)(get_addr0(loci) + step));
        refresh_rw0(loci);
        return v;
    }
    if (off == LOCI_REG_RW1) {
        uint8_t v = loci->regs[LOCI_REG_RW1];
        int8_t step = (int8_t)loci->regs[LOCI_REG_STEP1];
        set_addr1(loci, (uint16_t)(get_addr1(loci) + step));
        refresh_rw1(loci);
        return v;
    }

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

    if (off == LOCI_REG_CONS_TX) {
        /* Console UART TX — accumulate and flush to the log on '\n'. */
        cons_tx_byte(loci, value);
        loci->regs[LOCI_REG_CONS_TX] = value;
        return;
    }
    if (off == LOCI_REG_ULA_SNOOP) {
        /* Firmware reloads the ULA-attribute pattern matcher and clears
         * the register; we have no PIO sniffer, so just clear. */
        loci->regs[LOCI_REG_ULA_SNOOP] = 0;
        return;
    }

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

    if (off == LOCI_REG_API_STACK) {
        if (loci->xstack_ptr > 0) {
            loci->xstack[--loci->xstack_ptr] = value;
        }
        xstack_sync(loci);
        return;
    }

    loci->regs[off] = value;

    if (off == LOCI_REG_API_OP) {
        if (value == 0x00) {
            xstack_zero(loci);
            api_return_ax(loci, 0);
            return;
        }
        if (value == 0xFF) {
            api_install_blocked_stub(loci);
            log_debug("LOCI: exit() called — 6502 will spin at $03B0");
            return;
        }
        if (value == LOCI_OP_RESET_SENTINEL) {
            return;
        }
        api_install_blocked_stub(loci);
        dispatch_op(loci, value);
    }
}

void loci_task(loci_t* loci) {
    (void)loci;
}
