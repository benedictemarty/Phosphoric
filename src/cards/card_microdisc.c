/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_microdisc.c
 * @brief Contrôleur de disquettes Microdisc (WD1793) en module : menu, mise en
 *        route (ROM, disquettes, protection, --disk-web, --disk-create), bus,
 *        tick, fermeture (card_module.h).
 * @author bmarty <bmarty@mailo.com>
 *
 * Ses options (--disk-rom et les options de disquette) restent des options du
 * cœur, partagées avec le Jasmin et LOCI ; son état (emu->microdisc) reste dans la
 * machine (sauvegardes d'état FDC/MDC/DSK, LOCI, débogueur, menu).
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

/* ── Interruption IRQF_DISK (partagée par les contrôleurs disque et LOCI) ── */

static void irq_set(emulator_t* emu) { cpu_irq_set(&emu->cpu, IRQF_DISK); }
static void irq_clr(emulator_t* emu) { cpu_irq_clear(&emu->cpu, IRQF_DISK); }

/* ── Bus ───────────────────────────────────────────────────────────────── */

/* Microdisc WD1793 : $0310-$031F (l'ACIA, enregistrée avant, possède déjà
 * $031C-$031F si présente → pas de test interne ici). */
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


/* Sections FDC/MDC/DSK/BAD : écrites par savestate.c (historique). */
static const io_device_t k_bus = {
    .name = "microdisc", .claims = microdisc_dev_claims,
    .read = microdisc_dev_read, .write = microdisc_dev_write,
    /* Pas de tick ici : le cœur avance le FDC (io_bus.c), état dans la machine. */
};

/* ── Mise en route et fermeture ────────────────────────────────────────── */

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

        /* Languette de protection en écriture : posée explicitement, ou déduite
         * du fichier lui-même — un .dsk en lecture seule sur l'hôte se comporte
         * comme une disquette dont la languette est ouverte. */
        {
            bool wp = core->disk_write_protect;
            if (!wp && core->disk_files[0] && access(core->disk_files[0], W_OK) != 0)
                wp = true;
            if (wp) {
                /* Languette posée sur les 4 lecteurs : même effet que l'ancien
                 * drapeau global du WD1793 (toutes les disquettes protégées). */
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

        /* --disk-web URL : monte en lecteur A un disque dont les secteurs sont
         * servis par un serveur HTTP (projet loci-webdisk, architecture B). On
         * lit d'abord l'en-tête MFM_DISK distant (256 o) pour la géométrie, on
         * alloue une image à plat VIDE, et le FDC va chercher chaque piste MFM
         * de 6400 o à la demande (fidèle au chemin réel LOCI dsk_web / ATDISKRD). */
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
            emu->disks[0]->is_mfm  = false;   /* pas de write-back local */

            microdisc_set_disk(&emu->microdisc, 0, emu->disks[0]->data, emu->disks[0]->size,
                               emu->disks[0]->tracks, emu->disks[0]->sectors);
            fdc_set_web(&emu->microdisc.fdc, core->disk_web_url);   /* après set_disk */
            log_info("--disk-web: lecteur A servi par %s (%u faces x %u pistes x %u s., "
                     "pistes chargées à la demande)", core->disk_web_url, sides, tracks, spt);
        }

        /* --disk-create : monte une disquette Sedoric vierge en lecteur A et
         * l'écrit aussitôt sur FILE. INIT/format à l'intérieur ; le write-back
         * de sortie (armé avec cette option) persiste les changements. */
        if (core->disk_create_file && !emu->disks[0]) {
            /* Double face 42 pistes : géométrie que formate INIT B de Sedoric
             * (un blank simple face était sous-dimensionné, Sprint 66). */
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
