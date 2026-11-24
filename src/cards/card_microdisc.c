/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_microdisc.c
 * @brief Microdisc floppy disk controller (WD1793) as a module: menu, setup
 *        (ROM, floppies, write protection, --disk-web, --disk-create), bus,
 *        tick, teardown (card_module.h).
 * @author bmarty <bmarty@mailo.com>
 *
 * Its options (--disk-rom and the floppy options) stay core options, shared
 * with the Jasmin and LOCI; its state (emu->microdisc) stays in the machine
 * (FDC/MDC/DSK save states, LOCI, debugger, menu).
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
#include "io/microdisc.h"
#include "storage/disk_http.h"

static const card_desc_t k_desc = {
        "microdisc", "Microdisc",
        "Contrôleur de disquettes Oric (WD1793), 4 lecteurs 3\". Les disquettes "
        "s'insèrent ensuite dans la section Disquettes du menu.",
        "disque", "--disk-rom", 0, -1, 0x0310, 9, false,   /* $0310-$0313, $0314, $0318 */
        { { "rom", "ROM du contrôleur", CARD_P_FILE, "--disk-rom", "roms/microdis.rom",
            "Micrologiciel du Microdisc (microdis.rom) : démarrage du DOS et accès disque." } },
        1
    };
static const card_desc_t* const k_descs[] = { &k_desc };

/* ── IRQF_DISK interrupt (shared by the disk controllers and LOCI) ─────── */

static void irq_set(emulator_t* emu) { cpu_irq_set(&emu->cpu, IRQF_DISK); }
static void irq_clr(emulator_t* emu) { cpu_irq_clear(&emu->cpu, IRQF_DISK); }

/* ── Bus ───────────────────────────────────────────────────────────────── */

/* Microdisc WD1793: $0310-$031F (the ACIA, registered earlier, already owns
 * $031C-$031F if present → no internal test here). */
static bool microdisc_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->card_on[CARD_IDX_microdisc] && addr >= 0x0310 && addr <= 0x031F;
}
static uint8_t microdisc_dev_read(emulator_t* emu, uint16_t addr) {
    return microdisc_read(&emu->microdisc, addr);
}
static bool microdisc_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    if (fdc_trace_enabled()) {
        fprintf(stderr, "[FDC] PC=%04X cyc=%llu write $%04X = %02X\n",
                emu->cpu.PC, (unsigned long long)emu->cpu.cycles, addr, value);
    }
    microdisc_write(&emu->microdisc, addr, value);
    /* Sync overlay flags to memory system */
    emu->memory.basic_rom_disabled = emu->microdisc.romdis;
    emu->memory.overlay_active = emu->microdisc.diskrom;
    return true;
}


/* Sections FDC/MDC/DSK/BAD written by savestate.c (historical). */
static const io_device_t k_bus = {
    .name = "microdisc", .claims = microdisc_dev_claims,
    .read = microdisc_dev_read, .write = microdisc_dev_write,
    /* No tick here: the core advances the FDC (io_bus.c), state in the machine. */
};

/* ── Setup and teardown ────────────────────────────────────────────────── */

static int setup(emulator_t* emu, const void* p, const struct cli_opts_s* core) {
    (void)p;
    /* Load disks with Microdisc controller. A Microdisc ROM on its own is
     * enough to bring the controller up (a real Microdisc is present even with
     * no disk in the drives) — this enables hot-swapping a .dsk in later via
     * the OSD or the --control `load-disk` command. */
    bool any_disk = !emu->card_on[CARD_IDX_jasmin] &&
                    ((core->disk_create_file != NULL) || (core->disk_rom_file != NULL));
    for (int i = 0; !emu->card_on[CARD_IDX_jasmin] && i < MICRODISC_MAX_DRIVES; i++) {
        if (core->disk_files[i]) { any_disk = true; break; }
    }

    if (any_disk) {
        /* Initialize Microdisc controller */
        microdisc_init(&emu->microdisc);
        emu->microdisc.cpu_irq_set = irq_set;
        emu->microdisc.cpu_irq_clr = irq_clr;
        emu->microdisc.cpu_userdata = emu;
        emu->card_on[CARD_IDX_microdisc] = true;

        /* Write-protect tab: set explicitly, or inferred
         * from the file itself — a .dsk that is read-only on the host behaves
         * like a floppy whose tab is open. */
        {
            bool wp = core->disk_write_protect;
            if (!wp && core->disk_files[0] && access(core->disk_files[0], W_OK) != 0)
                wp = true;
            if (wp) {
                /* Tab set on all 4 drives: same effect as the former global
                 * WD1793 flag (every floppy protected). */
                for (uint8_t d = 0; d < MICRODISC_MAX_DRIVES; d++)
                    microdisc_set_write_protect(&emu->microdisc, d, true);
                log_info("Disque protégé en écriture (statut WD1793 bit 6)%s",
                         core->disk_write_protect ? "" : " — fichier en lecture seule");
            }
        }

        /* WD1793 timing profile: mechanical (real) by default, --fdc-timing
         * fast restores the legacy short delays (instant-feel loading). */
        if (core->fdc_timing_arg) {
            if (strcmp(core->fdc_timing_arg, "fast") == 0) {
                emu->microdisc.fdc.timing_mode = FDC_TIMING_FAST;
            } else if (strcmp(core->fdc_timing_arg, "real") == 0) {
                emu->microdisc.fdc.timing_mode = FDC_TIMING_REAL;
            } else {
                log_error("Invalid --fdc-timing '%s' (use real or fast)", core->fdc_timing_arg);
                                return 1;
            }
        }

        /* Load Microdisc ROM if specified */
        if (core->disk_rom_file) {
            log_info("Loading Microdisc ROM: %s", core->disk_rom_file);
            if (!microdisc_load_rom(&emu->microdisc, core->disk_rom_file)) {
                log_error("Failed to load Microdisc ROM: %s", core->disk_rom_file);
                                return 1;
            }
            /* Set overlay ROM in memory system */
            emu->memory.overlay_rom = emu->microdisc.diskrom_data;
            emu->memory.overlay_rom_size = emu->microdisc.diskrom_size;
            emu->memory.overlay_active = true;
            emu->memory.basic_rom_disabled = true;
            log_info("Microdisc ROM loaded (%u bytes), overlay active", emu->microdisc.diskrom_size);
        }

        /* Load disk images into drives A-D */
        for (int i = 0; i < MICRODISC_MAX_DRIVES; i++) {
            if (!core->disk_files[i]) continue;

            log_info("Loading disk drive %c: %s", 'A' + i, core->disk_files[i]);
            emu->disks[i] = sedoric_load(core->disk_files[i]);
            if (!emu->disks[i]) {
                log_error("Failed to load disk image: %s", core->disk_files[i]);
                                return 1;
            }

            /* Connect disk data to Microdisc drive slot */
            microdisc_set_disk(&emu->microdisc, (uint8_t)i,
                               emu->disks[i]->data, emu->disks[i]->size,
                               emu->disks[i]->tracks, emu->disks[i]->sectors);
            log_info("Drive %c: %u bytes, %d sides x %d tracks x %d sectors",
                     'A' + i, emu->disks[i]->size, emu->disks[i]->sides,
                     emu->disks[i]->tracks, emu->disks[i]->sectors);
        }

        /* --disk-web URL: mounts as drive A a disk whose sectors are
         * served by an HTTP server (loci-webdisk project, architecture B). The
         * remote MFM_DISK header (256 bytes) is read first for the geometry, an
         * EMPTY flat image is allocated, and the FDC fetches each 6400-byte MFM track
         * on demand (faithful to the real LOCI dsk_web / ATDISKRD path). */
        if (core->disk_web_url && !emu->disks[0]) {
            uint8_t hdr[MFM_DISK_HEADER_SIZE];
            long hn = disk_http_get(core->disk_web_url, 0, MFM_DISK_HEADER_SIZE,
                                    hdr, sizeof(hdr));
            if (hn < 16 || memcmp(hdr, "MFM_DISK", 8) != 0) {
                log_error("--disk-web: en-tête MFM_DISK illisible depuis %s", core->disk_web_url);
                                return 1;
            }
            uint32_t sides  = hdr[8]  | ((uint32_t)hdr[9]  << 8) |
                              ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
            uint32_t tracks = hdr[12] | ((uint32_t)hdr[13] << 8) |
                              ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
            if (sides < 1) sides = 1;
            if (sides > MFM_MAX_SIDES)  sides  = MFM_MAX_SIDES;
            if (tracks > MFM_MAX_TRACKS) tracks = MFM_MAX_TRACKS;
            uint8_t spt = MFM_MAX_SECTORS;   /* 17 */

            emu->disks[0] = (sedoric_disk_t*)calloc(1, sizeof(sedoric_disk_t));
            uint32_t flat = sides * tracks * spt * SEDORIC_SECTOR_SIZE;
            if (!emu->disks[0] || !(emu->disks[0]->data = (uint8_t*)calloc(1, flat))) {
                log_error("--disk-web: allocation image à plat impossible");
                                return 1;
            }
            emu->disks[0]->size    = flat;
            emu->disks[0]->tracks  = (uint8_t)tracks;
            emu->disks[0]->sectors = spt;
            emu->disks[0]->sides   = (uint8_t)sides;
            emu->disks[0]->is_mfm  = false;   /* no local write-back */

            microdisc_set_disk(&emu->microdisc, 0, emu->disks[0]->data, emu->disks[0]->size,
                               emu->disks[0]->tracks, emu->disks[0]->sectors);
            fdc_set_web(&emu->microdisc.fdc, core->disk_web_url);   /* after set_disk */
            log_info("--disk-web: lecteur A servi par %s (%u faces x %u pistes x %u s., "
                     "pistes chargées à la demande)", core->disk_web_url, sides, tracks, spt);
        }

        /* --disk-create: mounts a blank Sedoric floppy as drive A and
         * writes it immediately to FILE. INIT/format inside; the exit
         * write-back (armed with this option) persists the changes. */
        if (core->disk_create_file && !emu->disks[0]) {
            /* Double-sided 42 tracks: the geometry that Sedoric's INIT B formats
             * (a single-sided blank was undersized, Sprint 66). */
            emu->disks[0] = sedoric_create_blank(SEDORIC_TRACKS, 2);
            if (!emu->disks[0]) {
                log_error("disk-create: allocation de la disquette vierge impossible");
                                return 1;
            }
            if (!sedoric_save(emu->disks[0], core->disk_create_file))
                log_error("disk-create: écriture impossible vers %s", core->disk_create_file);
            else
                log_info("disk-create: disquette vierge -> %s (%u octets), lecteur A",
                         core->disk_create_file, emu->disks[0]->size);
            microdisc_set_disk(&emu->microdisc, 0, emu->disks[0]->data, emu->disks[0]->size,
                               emu->disks[0]->tracks, emu->disks[0]->sectors);
            emu->disk_paths[0] = core->disk_create_file;
            emu->disk_path = core->disk_create_file;
        } else if (core->disk_create_file && emu->disks[0]) {
            log_warning("disk-create ignoré : le lecteur A est déjà occupé par -d");
        }
    }

    return 0;
}

static void teardown(emulator_t* emu) {
    if (emu->card_on[CARD_IDX_microdisc]) {
        microdisc_cleanup(&emu->microdisc);
    }
}

const card_module_t card_microdisc = {
    .descs = k_descs, .ndescs = 1, .desc_before = "jasmin",
    .stage = CARD_STAGE_DISKS, .setup = setup, .teardown = teardown,
    .bus = &k_bus, .bus_before = "jasmin",
};
