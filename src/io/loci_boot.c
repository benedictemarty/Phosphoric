/**
 * @file loci_boot.c
 * @brief LOCI MIA_BOOT runtime ROM swap + Sprint 34au tuning / config stubs
 *        (CPU_PHI2, OEM_CODEPAGE, STDIN_OPT, MAP_TUNE_*).
 *
 * Sprint 34c R4 : mechanical split of loci.c.
 */

#include "io/loci.h"
#include "io/loci_internal.h"
#include "utils/logging.h"

#include <string.h>
#include <stdio.h>

/* ─── MIA_BOOT — runtime ROM swap (Sprint 34ad) ──────────────── */

static const char* derive_basic_rom_path(loci_t* loci, uint8_t settings) {
    if (loci->mnt_mounted[LOCI_MNT_ROM]) {
        return loci->mnt_paths[LOCI_MNT_ROM];
    }
    return (settings & LOCI_BOOT_B11) ? "basic11b.rom" : "basic10.rom";
}

void op_mia_boot(loci_t* loci) {
    uint8_t settings = loci->regs[LOCI_REG_API_A];
    loci->boot_settings = settings;

    if (settings & LOCI_BOOT_RESUME) {
        /* Menu "resume" entry: the host swaps the pre-warm ROM back and
         * restores the session snapshot taken at the Action button press. */
        if (loci->resume_cb && !loci->resume_cb(loci->resume_ctx)) {
            api_return_errno(loci, LOCI_ENOENT);   /* no snapshot to resume */
            return;
        }
        api_return_ax(loci, 0);
        return;
    }

    if (!loci->rom_swap_cb) {
        api_return_ax(loci, 0);
        return;
    }

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

/* ─── Sprint 34au : tuning / config stubs ──────────────────────── */

void op_cpu_phi2(loci_t* loci) {
    /* Firmware returns the PHI2 clock in kHz in AX (cpu.c cpu_api_phi2),
     * not Hz — 1 MHz Oric bus = 1000. */
    log_debug("LOCI op_cpu_phi2: returning 1000 kHz");
    api_return_ax(loci, 1000);
}

void op_oem_codepage(loci_t* loci) {
    uint8_t cp = loci->regs[LOCI_REG_API_A];
    log_debug("LOCI op_oem_codepage: cp=%u accepted (no translation done)", cp);
    api_return_ax(loci, 0);
}

void op_stdin_opt(loci_t* loci) {
    uint8_t opt = loci->regs[LOCI_REG_API_A];
    log_debug("LOCI op_stdin_opt: opt=$%02X accepted", opt);
    api_return_ax(loci, 0);
}

/* The tune value is passed in register A (firmware map.c: `uint8_t delay = API_A`).
 * Firmware semantics (map.c + cfg.c): A <= 31 SETS the delay, any other
 * value is a pure QUERY ("other return current") — cfg_set_* rejects it
 * and nothing changes; the op ALWAYS returns the current value in AX
 * (api_return_ax(cfg_get_*())). The stored value is kept unmasked: the
 * firmware only masks at the PIO level (adj.c, e.g. tiod & 0x07), the
 * config keeps and returns what was set. We store it so the modelled MIA
 * I/O reliability (loci_mia_io_reliable) reacts to it — a tior outside
 * the configured window corrupts the picowifi ACIA. */
static void op_map_tune_store(loci_t* loci, const char* name, uint8_t* field) {
    uint8_t a = loci->regs[LOCI_REG_API_A];
    if (a <= 31) {
        *field = a;
        log_debug("LOCI %s = %u (modelled MIA timing)", name, a);
    }
    xstack_zero(loci);
    api_return_ax(loci, *field);
}

bool loci_mia_io_reliable(const loci_t* loci) {
    return loci->mia_tior >= loci->mia_tior_lo && loci->mia_tior <= loci->mia_tior_hi;
}

void loci_set_mia_window(loci_t* loci, uint8_t lo, uint8_t hi) {
    if (lo > 31) lo = 31;
    if (hi > 31) hi = 31;
    if (lo > hi) { uint8_t t = lo; lo = hi; hi = t; }
    loci->mia_tior_lo = lo;
    loci->mia_tior_hi = hi;
}

void op_adj_scan(loci_t* loci) {
    /* Firmware (adj.c): asynchronous sweep of tior 0-31 (~100 ms lead-in
     * then one step per 5 ms) with live progress in the menu ROM's
     * TIMINGS byte at $FFF0 = 0x80 | tior (bit 7 = scan in progress),
     * then the CONFIGURED tior is restored there (bit 7 clear). The menu
     * ROM polls that byte, so with a rom_poke hook we run the sweep over
     * emulated time (loci_adj_tick); without one (unit tests, headless
     * embedders) the completed-scan final state is reported at once. */
    if (loci->rom_poke_cb) {
        loci->adj_state = 1;
        loci->adj_tior = 0;
        loci->adj_acc = 0;
        loci->xram[0xFFF0] = 0x80;
        loci->rom_poke_cb(loci->rom_poke_ctx, 0xFFF0, 0x80);
    } else {
        loci->xram[0xFFF0] = loci->mia_tior & 0x1F;
    }
    xstack_zero(loci);
    api_return_ax(loci, 0);
}

void loci_adj_tick(loci_t* loci, unsigned int cycles) {
    if (!loci || loci->adj_state == 0) return;
    loci->adj_acc += cycles;
    if (loci->adj_state == 1) {
        if (loci->adj_acc < 100000) return;    /* 20 × 5 ms lead-in */
        loci->adj_acc = 0;
        loci->adj_state = 2;
        loci->adj_tior = 0;
        return;
    }
    while (loci->adj_acc >= 5000) {            /* one tior step per 5 ms */
        loci->adj_acc -= 5000;
        if (++loci->adj_tior > 31) {
            /* cleanup: restore the configured tior, scan-done flag clear */
            uint8_t v = loci->mia_tior & 0x1F;
            loci->xram[0xFFF0] = v;
            if (loci->rom_poke_cb)
                loci->rom_poke_cb(loci->rom_poke_ctx, 0xFFF0, v);
            loci->adj_state = 0;
            return;
        }
    }
    uint8_t v = (uint8_t)(0x80 | loci->adj_tior);
    loci->xram[0xFFF0] = v;
    if (loci->rom_poke_cb)
        loci->rom_poke_cb(loci->rom_poke_ctx, 0xFFF0, v);
}

void op_map_tune_tmap(loci_t* loci) { op_map_tune_store(loci, "MAP_TUNE_TMAP", &loci->mia_tmap); }
void op_map_tune_tior(loci_t* loci) { op_map_tune_store(loci, "MAP_TUNE_TIOR", &loci->mia_tior); }
void op_map_tune_tiow(loci_t* loci) { op_map_tune_store(loci, "MAP_TUNE_TIOW", &loci->mia_tiow); }
void op_map_tune_tiod(loci_t* loci) { op_map_tune_store(loci, "MAP_TUNE_TIOD", &loci->mia_tiod); }
void op_map_tune_tadr(loci_t* loci) { op_map_tune_store(loci, "MAP_TUNE_TADR", &loci->mia_tadr); }
