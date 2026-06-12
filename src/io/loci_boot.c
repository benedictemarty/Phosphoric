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

static void op_map_tune_noop(loci_t* loci, const char* name) {
    log_debug("LOCI %s (xstack_ptr=%u) accepted, no hardware to tune",
              name, loci->xstack_ptr);
    xstack_zero(loci);
    api_return_ax(loci, 0);
}

void op_adj_scan(loci_t* loci) {
    /* Firmware (adj.c) releases immediately then sweeps tior 0-31 with
     * progress in xram[$FFF0] (bit 7 = scan in progress). No hardware to
     * tune here: report an already-completed scan. */
    loci->xram[0xFFF0] = 0x00;
    xstack_zero(loci);
    api_return_ax(loci, 0);
}

void op_map_tune_tmap(loci_t* loci) { op_map_tune_noop(loci, "MAP_TUNE_TMAP"); }
void op_map_tune_tior(loci_t* loci) { op_map_tune_noop(loci, "MAP_TUNE_TIOR"); }
void op_map_tune_tiow(loci_t* loci) { op_map_tune_noop(loci, "MAP_TUNE_TIOW"); }
void op_map_tune_tiod(loci_t* loci) { op_map_tune_noop(loci, "MAP_TUNE_TIOD"); }
void op_map_tune_tadr(loci_t* loci) { op_map_tune_noop(loci, "MAP_TUNE_TADR"); }
