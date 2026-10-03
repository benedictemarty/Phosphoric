/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_jasmin.c
 * @brief Jasmin disk interface (WD177x) as a module: menu, setup, bus,
 *        tick (card_module.h).
 * @author bmarty <bmarty@mailo.com>
 *
 * Its --jasmin-rom option stays a core option (also read when loading
 * phosphoric.cfg and by the Microdisc for their mutual exclusion); its state
 * (emu->jasmin) stays in the machine (paged memory, save states).
 */
#define _DEFAULT_SOURCE   /* access() */
#include "card_module.h"
#include "cards_list.h"
#include "emulator.h"
#include "cli/cli_opts.h"
#include "cpu/cpu6502.h"
#include "storage/disk.h"
#include "storage/sedoric.h"
#include "utils/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>   /* offsetof */
#include <unistd.h>
#include "io/jasmin.h"

static const card_desc_t k_desc = {
        "jasmin", "Jasmin",
        "Contrôleur de disquettes Jasmin (WD177x), autre standard Oric (TDOS).",
        "disque", "--jasmin-rom", 0, -1, 0x03F4, 12, false,
        { { "rom", "ROM de démarrage", CARD_P_FILE, "--jasmin-rom", "roms/jasmin.rom",
            "ROM de démarrage Jasmin (2 Ko, jasmin.rom), servie en $F800." } },
        1
    };
static const card_desc_t* const k_descs[] = { &k_desc };

/* ── IRQF_DISK interrupt (shared by the disk controllers and LOCI) ─────── */

static void irq_set(emulator_t* emu) { cpu_irq_set(&emu->cpu, IRQF_DISK); }
static void irq_clr(emulator_t* emu) { cpu_irq_clear(&emu->cpu, IRQF_DISK); }

/* ── Bus ───────────────────────────────────────────────────────────────── */

/* Jasmin WD177x: $03F4-$03FF (mutually exclusive with DTL2000/Mageco, which
 * overlap $03F8-$03FF — guard at activation in main.c). */
static bool jasmin_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->card_on[CARD_IDX_jasmin] && addr >= JASMIN_BASE && addr <= JASMIN_END;
}
static uint8_t jasmin_dev_read(emulator_t* emu, uint16_t addr) {
    return jasmin_read(&emu->jasmin, addr);
}
static bool jasmin_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    if (fdc_trace_enabled()) {
        fprintf(stderr, "[FDC] PC=%04X cyc=%llu write $%04X = %02X\n",
                emu->cpu.PC, (unsigned long long)emu->cpu.cycles, addr, value);
    }
    jasmin_write(&emu->jasmin, addr, value);
    /* Sync Jasmin banking flags to the memory system. */
    emu->memory.jasmin_olay   = emu->jasmin.olay;
    emu->memory.jasmin_romdis = emu->jasmin.romdis;
    return true;
}

/* Savestate (section "JAS"): emitted only when the Jasmin is present. The
 * disk images go through the DSK section (savestate.c), read BEFORE it. */
static bool jasmin_dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->card_on[CARD_IDX_jasmin]) return false;
    return jasmin_save(&emu->jasmin, fp);
}
static void jasmin_dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    jasmin_load(&emu->jasmin, fp, size);
    /* Same latch-to-memory synchronisation as jasmin_dev_write. */
    emu->memory.jasmin_olay   = emu->jasmin.olay;
    emu->memory.jasmin_romdis = emu->jasmin.romdis;
}


static const io_device_t k_bus = {
    .name = "jasmin", .claims = jasmin_dev_claims, .read = jasmin_dev_read,
    .write = jasmin_dev_write,
    .save_tag = "JAS\0", .save = jasmin_dev_save, .load = jasmin_dev_load,
    /* No tick here: the core advances the FDC (io_bus.c), state in the machine. */
};

/* ── Setup ─────────────────────────────────────────────────────────────── */

static int setup(emulator_t* emu, const void* p, const struct cli_opts_s* core) {
    (void)p;
    /* Jasmin disk interface (--jasmin-rom): the 2nd Oric disk standard (WD177x
     * at $03F4-$03FF, 2 KB boot ROM at $F800). Alternative to the Microdisc and
     * mutually exclusive with it and with DTL2000/Mageco ($03F8-$03FF overlap). */
    if (core->jasmin_rom_file) {
        if (core->disk_rom_file) {
            log_error("--jasmin-rom and --disk-rom are mutually exclusive "
                      "(Jasmin vs Microdisc)");
                        return 1;
        }
        if (emu->card_on[CARD_IDX_dtl2000] || emu->card_on[CARD_IDX_mageco]) {
            log_error("--jasmin-rom conflicts with --dtl2000/--mageco "
                      "(both claim $03F8-$03FF)");
                        return 1;
        }
        /* Load the 2 KB Jasmin boot ROM. */
        FILE* jf = fopen(core->jasmin_rom_file, "rb");
        if (!jf) {
            log_error("Failed to open Jasmin ROM: %s", core->jasmin_rom_file);
                        return 1;
        }
        uint8_t jbuf[JASMIN_ROM_SIZE];
        size_t jrd = fread(jbuf, 1, JASMIN_ROM_SIZE, jf);
        fclose(jf);

        jasmin_init(&emu->jasmin);
        if (jrd != JASMIN_ROM_SIZE || !jasmin_load_rom(&emu->jasmin, jbuf, (uint32_t)jrd)) {
            log_error("Jasmin ROM must be exactly %d bytes (got %zu): %s",
                      JASMIN_ROM_SIZE, jrd, core->jasmin_rom_file);
                        return 1;
        }
        emu->jasmin.cpu_irq_set = irq_set;   /* IRQF_DISK (shared) */
        emu->jasmin.cpu_irq_clr = irq_clr;
        emu->jasmin.cpu_userdata = emu;
        emu->card_on[CARD_IDX_jasmin] = true;

        /* Wire the Jasmin banking into the memory system. At boot the Jasmin
         * ROM is NOT paged (romdis=olay=0 → BASIC ROM visible); the auto-boot
         * PC-trap pages it in ($3FB=1) at $EB78/$E905. */
        emu->memory.jasmin_active = true;
        emu->memory.jasmin_rom = emu->jasmin.rom;
        emu->memory.jasmin_olay = emu->jasmin.olay;
        emu->memory.jasmin_romdis = emu->jasmin.romdis;

        if (core->fdc_timing_arg && strcmp(core->fdc_timing_arg, "fast") == 0)
            emu->jasmin.fdc.timing_mode = FDC_TIMING_FAST;

        log_info("Jasmin disk interface enabled (WD177x $03F4-$03FF, ROM $F800)");

        /* Load disk images into drives A-D (same MFM_DISK container as the
         * Microdisc — sedoric_load). */
        for (int i = 0; i < JASMIN_MAX_DRIVES; i++) {
            if (!core->disk_files[i]) continue;
            log_info("Loading Jasmin disk drive %c: %s", 'A' + i, core->disk_files[i]);
            emu->disks[i] = sedoric_load(core->disk_files[i]);
            if (!emu->disks[i]) {
                log_error("Failed to load disk image: %s", core->disk_files[i]);
                                return 1;
            }
            emu->disk_paths[i] = core->disk_files[i];
            jasmin_set_disk(&emu->jasmin, (uint8_t)i,
                            emu->disks[i]->data, emu->disks[i]->size,
                            emu->disks[i]->tracks, emu->disks[i]->sectors);
            log_info("Drive %c: %u bytes, %d sides x %d tracks x %d sectors",
                     'A' + i, emu->disks[i]->size, emu->disks[i]->sides,
                     emu->disks[i]->tracks, emu->disks[i]->sectors);
        }
    }

    return 0;
}

const card_module_t card_jasmin = {
    .descs = k_descs, .ndescs = 1, .desc_before = "loci",
    .stage = CARD_STAGE_DISKS_EARLY, .setup = setup,
    .bus = &k_bus, .bus_before = "sp0256",
};
