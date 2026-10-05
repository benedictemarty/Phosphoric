/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_loci.c
 * @brief LOCI cartridge as a module: menu entry (one card, three modes:
 *        built-in, co-simulated firmware, LOCI-USB), bus access (MIA, TAP,
 *        DSK, co-simulation) and setup (HLE model, firmware co-simulation,
 *        real hardware) — card_module.h.
 * @author bmarty <bmarty@mailo.com>
 *
 * Its options (--loci*) remain core options (two are read by the ACIA and
 * Microdisc cards, set up before it); its state (emu->loci) and its tick stay
 * in the machine (tape, menu, media, debugger, LOCI itself: io_bus.c,
 * loci_glue.c, tape_patches.c…).
 */
#define _DEFAULT_SOURCE
#include "card_module.h"
#include "cards_list.h"
#include "emulator.h"
#include "cli/cli_opts.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "io/io_bus.h"        /* loci_emu_reflect_nirq */
#include "io/loci.h"
#include "io/loci_emu.h"
#include "io/loci_glue.h"
#include "io/loci_internal.h"   /* loci_dsk_open_web */
#include "io/bus_timing.h"
#include "storage/disk.h"
#include "utils/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

/* Mode-specific parameters: cards.c (k_loci_mode_params) says which mode; the
 * menu only shows those of the selected mode. « mode » must stay first. */
static const card_desc_t k_desc = {
        "loci", "LOCI",
        "Cartouche LOCI : menu de fichiers, émulation Microdisc et cassette depuis une "
        "carte SD ou une clé USB, ACIA en $0380. Modèle intégré, vrai firmware "
        "co-simulé ou LOCI-USB (Feather) branchée sur ce PC.",
        "disque", "--loci", -1, -1, 0x03A0, 32, false,
        { { "mode", "Mode", CARD_P_CHOICE, NULL,
            LOCI_MODE_HLE "|" LOCI_MODE_FW "|" LOCI_MODE_HW,
            "intégré : LOCI émulée par Phosphoric. firmware : le vrai firmware RP2040 "
            "tourne dans l'émulateur (développement). usb : une LOCI-USB (Feather "
            "RP2040) branchée sur ce PC fait tourner le firmware, Phosphoric joue "
            "l'Oric par l'USB. Seuls les modes "
            "présents dans ce binaire sont proposés." },
          { "menu", "Démarrer sur le menu LOCI", CARD_P_BOOL, NULL, "oui",
            "oui : l'Oric démarre sur le menu de la carte (ROM roms/loci/locirom) ; "
            "non : BASIC direct, LOCI reste disponible." },
          { "sd", "Image de carte SD", CARD_P_FILE, "--loci-sdimg", "",
            "Image FAT16/32 lue par LOCI : ses .dsk et .tap apparaissent dans son menu. "
            "Vide : aucune." },
          { "flash", "Dossier flash interne", CARD_P_DIR, "--loci-flash", "",
            "Dossier de l'hôte servant de mémoire flash interne (fichiers 0: du LOCI). "
            "Vide : aucun." },
          { "modem", "Modem Wi-Fi (picowifi)", CARD_P_CHOICE, NULL,
            LOCI_MODEM_NONE "|" LOCI_MODEM_SIM "|" LOCI_MODEM_REAL,
            "Modem de la carte, vu par l'Oric comme l'ACIA en $0380. simulé : picowifi "
            "émulé (commandes AT, Wi-Fi par le réseau de l'hôte) ; réel : le picowifi "
            "USB branché sur ce PC. Remplace la carte ACIA 6551." },
          { "port", "Port du picowifi réel", CARD_P_TEXT, NULL, "",
            "Port série du picowifi réel (ex. /dev/ttyACM0). Vide : détection automatique "
            "par son nom USB « PicoWifiModemUSB » (Linux)." },
          { "elf", "Firmware (ELF)", CARD_P_FILE, "--loci-emu", "",
            "Fichier loci-firmware.elf compilé pour RP2040 (obligatoire)." },
          { "fw_flash", "Image flash du firmware", CARD_P_FILE, "--loci-emu-flash", "",
            "Mémoire flash persistante du firmware ; vide : <ELF>.flash ; « - » : "
            "volatile." },
          { "fw_usb", "Image de clé USB", CARD_P_FILE, "--loci-usb-image", "",
            "Image FAT servie au firmware comme clé USB ; vide : aucune." },
          { "port_usb", "Port de la LOCI-USB", CARD_P_TEXT, "--loci-hw", "",
            "Port série de la LOCI-USB (Feather, ex. /dev/ttyACM0). Vide : détection "
            "automatique par son nom USB « LOCI-USB » (Linux). Bouton MENU : F8." } },
        10
    };
static const card_desc_t* const k_descs[] = { &k_desc };

/* ── Bus ───────────────────────────────────────────────────────────────── */

/* LOCI (sodiumlb): three disjoint sub-windows, dispatched internally.
 *  - MIA $03A0-$03BF (independent of the other devices);
 *  - TAP $0315-$0317: replaces the cassette interface, overlaps the Microdisc
 *    $0310-$031F → priority (LOCI is first in the table);
 *  - DSK $0310-$0314 + $0318-$0319: only when no real Microdisc is present
 *    (otherwise the Microdisc owns the range). */
/* Co-sim: window + registers of the $AF RAM expansion ($03C0-$03E4), served by the
 * firmware (io-page) — unknown to the internal model. */
static bool loci_emu_ramx_claims(uint16_t addr) {
    return loci_emu_active() && addr >= 0x03C0 && addr <= 0x03E4;
}
static bool loci_dev_claims(emulator_t* emu, uint16_t addr) {
    if (!emu->card_on[CARD_IDX_loci]) return false;
    if (loci_emu_io_page()) return addr >= 0x0310 && addr <= 0x03FF;   /* neo backend: /IO CONTROL */
    if (loci_addr_in_mia(addr) || loci_emu_ramx_claims(addr)) return true;
    if (loci_addr_in_tap(addr)) return true;
    if (!emu->card_on[CARD_IDX_microdisc] && loci_addr_in_dsk(addr)) return true;
    return false;
}
/* Faithful --loci-hw: read served after the 6502 latch → open bus. */
static uint8_t hw_lost(emulator_t* emu, uint8_t v) {
    return loci_emu_read_lost() ? memory_open_bus(&emu->memory) : v;
}
static uint8_t loci_dev_read(emulator_t* emu, uint16_t addr) {
    /* Co-sim backend (--loci-emu): the MIA $03xx window is served by the REAL
     * RP2040 firmware (emulator) instead of the behavioural backend (loci_core).
     * During the background boot (first launch of an ELF), we WAIT: otherwise the
     * internal model answered in place of the firmware (open("N:…") → FR_NO_FILE). */
    loci_emu_wait_boot();
    if (loci_emu_io_page()) {
        uint8_t v;
        bool drv = loci_emu_io_read(addr, &v);
        loci_emu_reflect_nirq(emu);
        return drv ? v : memory_open_bus(&emu->memory);
    }
    if (loci_emu_ramx_claims(addr)) return hw_lost(emu, loci_emu_api_read(addr));
    if (loci_addr_in_mia(addr)) return loci_emu_active() ? hw_lost(emu, loci_emu_api_read(addr))
                                                         : loci_read(&emu->loci, addr);
    if (loci_addr_in_tap(addr)) return loci_emu_active() ? hw_lost(emu, loci_emu_tap_read(addr))
                                                         : loci_tap_read(&emu->loci, addr);
    /* DSK (guaranteed by claims). In co-sim, the WD1793 is the firmware's (oric/dsk.c):
     * a .dsk mounted on A: in the REAL LOCI menu is finally read by the 6502. */
    if (loci_emu_active()) {
        uint8_t v = hw_lost(emu, loci_emu_dsk_read(addr));
        loci_emu_reflect_nirq(emu);   /* end of sector: the IRQ is raised on the LAST DATA read */
        return v;
    }
    return loci_dsk_read(&emu->loci, addr);
}
static bool loci_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    loci_emu_wait_boot();
    if (loci_emu_io_page()) { loci_emu_io_write(addr, value); loci_emu_reflect_nirq(emu); return true; }
    if (loci_emu_ramx_claims(addr))  loci_emu_api_write(addr, value);
    else if (loci_addr_in_mia(addr)) { if (loci_emu_active()) { loci_emu_api_write(addr, value);
                                                           loci_emu_reflect_nirq(emu); }
                                  else                   loci_write(&emu->loci, addr, value); }
    else if (loci_addr_in_tap(addr)) { if (loci_emu_active()) loci_emu_tap_write(addr, value);
                                       else                   loci_tap_write(&emu->loci, addr, value); }
    else if (loci_emu_active())    { loci_emu_dsk_write(addr, value);           /* DSK co-sim */
                                     loci_emu_reflect_nirq(emu); }
    else                             loci_dsk_write(&emu->loci, addr, value);  /* DSK */
    return true;
}

/* No .ost section (caveat of the file backend's OS handles); no tick
 * here: the core advances the FDC and the LOCI clock (io_bus.c). */
static const io_device_t k_bus = {
    .name = "loci", .claims = loci_dev_claims, .read = loci_dev_read,
    .write = loci_dev_write,
};

/* ── Setup: firmware co-simulation, real hardware, HLE model ───────────── */

static int setup(emulator_t* emu, const void* p, const struct cli_opts_s* core) {
    (void)p;
    /* --loci-emu: run the REAL RP2040 firmware in the emulator (smoke test:
     * boot + banner). Bus co-sim not wired yet -> the behavioural backend
     * stays active in parallel for the runtime. */
    if (core->loci_emu_path) {
        if (!loci_emu_select("emul")) {
            log_error("--loci-emu : firmware co-simulé absent de ce binaire (libemul "
                      "introuvable au build : make LOCI_EMU=1 LOCI_EMUL_DIR=…)");
            return 1;
        }
        if (core->loci_emu_usb_image) loci_emu_set_usb_image(core->loci_emu_usb_image);
        if (core->loci_emu_cdc_dev) loci_emu_set_cdc_device(core->loci_emu_cdc_dev);
        if (core->loci_emu_flash) loci_emu_set_flash_image(core->loci_emu_flash);
        loci_emu_start(core->loci_emu_path);
        emu->loci_external = true;
    }
    /* --loci-hw: a LOCI-USB (Feather) runs the LOCI firmware; the emulated 6502
     * sends it its accesses over USB (loci-usb protocol). The loci_hw.c backend
     * shares the loci_emu.h interface: same io_bus/memory path, but
     * each access is a real bus cycle. Present in the binary when the
     * loci-usb repository was present at build time (LOCI_HW, Makefile). */
    if (core->loci_hw_dev) {
        if (!loci_emu_select("hw")) {
            log_error("--loci-hw : LOCI-USB absente de ce binaire (dépôt "
                      "~/loci/loci-usb introuvable au build : make LOCI_HW=1 LOCI_USB_DIR=…)");
            return 1;
        }
        if (loci_emu_start(core->loci_hw_dev) != 0) return 1;
        emu->loci_external = true;
    }

    /* Enable LOCI peripheral (--loci) */
    if (core->loci_enabled) {
        loci_init(&emu->loci);
        emu->loci.enabled = true;
        emu->card_on[CARD_IDX_loci] = true;
        /* Route B: server base for the « W: Web disks » pseudo-device. */
        if (core->loci_web_base) {
            snprintf(emu->loci.web_base, sizeof(emu->loci.web_base), "%s", core->loci_web_base);
            log_info("LOCI: device web « W: Web disks » -> %s (menu: opendir/readdir GET /disks)",
                     core->loci_web_base);
        }
        if (core->loci_mia_win_lo >= 0) {
            loci_set_mia_window(&emu->loci, (uint8_t)core->loci_mia_win_lo, (uint8_t)core->loci_mia_win_hi);
            log_info("LOCI MIA reliable tior window: %d-%d (picowifi ACIA $0380 "
                     "corrupted outside it; tune via MAP_TUNE_TIOR / ADJ_SCAN)",
                     emu->loci.mia_tior_lo, emu->loci.mia_tior_hi);
        }
        if (core->loci_serve_cycles >= 0) {
            /* Sub-cycle PHI2 race model (bus_timing.h) — replaces the window. */
            loci_set_serve_timing(&emu->loci, (uint16_t)core->loci_serve_cycles,
                                  (uint16_t)core->loci_tdsr_ns);
            log_info("LOCI MIA phase model: serve=%d cycles, tDSR=%d ns — picowifi $0380 "
                     "propre ssi la donnée précède la fin du cycle moins tDSR",
                     emu->loci.mia_serve_cycles, emu->loci.mia_tdsr_ns);
        }
        if (core->loci_serve_jitter >= 0) {
            loci_set_serve_jitter(&emu->loci, (uint8_t)core->loci_serve_jitter, core->loci_jitter_seed);
            log_info("LOCI MIA serve jitter: +/-%d cycles (seed=%u) — ratés "
                     "occasionnels reproductibles pres du latch",
                     emu->loci.mia_serve_jitter, core->loci_jitter_seed);
        }
        /* ROM-swap callback used by op 0xA0 MIA_BOOT (Sprint 34ad). */
        loci_set_rom_swap_callback(&emu->loci, loci_rom_swap_cb, emu);
        /* Session-resume callback: menu "resume" → MIA_BOOT RESUME (Sprint 85). */
        loci_set_resume_callback(&emu->loci, loci_resume_session_cb, emu);
        /* Live ROM poke: ADJ_SCAN progress byte polled by the menu ROM. */
        loci_set_rom_poke_callback(&emu->loci, loci_rom_poke_hook, emu);

        /* Device list served by opendir("") (menu file browser: internal
         * storage first, then one line per mounted USB device — firmware
         * usb_set_status strings). */
        if (core->loci_sdimg_path) {
            struct stat st;
            char msc[64];
            double mb = (stat(core->loci_sdimg_path, &st) == 0)
                      ? (double)st.st_size / (1024.0 * 1024.0) : 0.0;
            if (mb >= 1024.0)
                snprintf(msc, sizeof(msc), "MSC %.1f GB PHOSPHOR SDIMG rev 1.0",
                         mb / 1024.0);
            else
                snprintf(msc, sizeof(msc), "MSC %.1f MB PHOSPHOR SDIMG rev 1.0", mb);
            loci_add_usb_device(&emu->loci, msc);
        }
        if (emu->serial_spec && strncmp(emu->serial_spec, "picowifi", 8) == 0) {
            /* firmware cdc.c: the picowifi enumerates as a CDC modem */
            loci_add_usb_device(&emu->loci, "CDC modem mounted");
        }
        /* Real USB keys: explicit --loci-usb DIRs, then media mounted on
         * the host (udisks: /media/$USER, /run/media/$USER). Their "N:"
         * paths are served from the host directory. NOTE: with
         * --loci-sdimg, file ops are owned by the SD image backend — the
         * keys still appear in the list but are not browsable. */
        for (int i = 0; i < core->loci_usb_count; i++)
            loci_attach_usb_dir(emu, core->loci_usb_args[i]);
        if (core->loci_usb_autoscan)
            loci_scan_host_usb(emu);
        loci_set_dsk_bus_callbacks(&emu->loci, loci_dsk_cpu_irq_set,
                                   loci_dsk_cpu_irq_clr,
                                   loci_dsk_sync_overlay, emu);
        /* Tape-mount callback used by op_mount on LOCI_MNT_TAP (Sprint 34ao). */
        loci_set_tape_mount_callback(&emu->loci, loci_tape_mount_cb, emu);
        /* Action-button hooks (Sprint 34ai). */
        loci_set_action_callbacks(&emu->loci,
            loci_action_install_irq_trap,
            loci_action_release_irq_trap,
            emu);
        /* Sprint 34am fix: the real LOCI hardware's Pi Pico firmware
         * pre-initialises the AY-3-8910 R7 (mixer) to enable Port A as
         * output for keyboard scanning. The LOCI ROM relies on that
         * state and never writes R7 itself. Without this seed, the
         * keyboard scan callback's R7-bit-6 check always rejects, and
         * no key reaches the LOCI TUI. Mirror the firmware setup so
         * the ROM's ReadKeyboard sees a working PSG. */
        emu->psg.registers[7] = 0x7F;
        log_info("LOCI: pre-seeded PSG R7=$7F (firmware AY init for keyboard)");
        if (core->loci_flash_root && core->loci_sdimg_path) {
            log_error("--loci-flash and --loci-sdimg are mutually exclusive");
            return 1;
        }
        if (core->loci_sdimg_path) {
            if (!loci_attach_sdimg(&emu->loci, core->loci_sdimg_path)) {
                log_error("Failed to attach LOCI SD image: %s", core->loci_sdimg_path);
                return 1;
            }
            log_info("LOCI MIA enabled at $%04X-$%04X (SD image: %s)",
                     LOCI_MIA_BASE, LOCI_MIA_END, core->loci_sdimg_path);
        } else if (core->loci_flash_root) {
            loci_set_flash_root(&emu->loci, core->loci_flash_root);
            log_info("LOCI MIA enabled at $%04X-$%04X (flash root: %s)",
                     LOCI_MIA_BASE, LOCI_MIA_END, core->loci_flash_root);
        } else {
            log_info("LOCI MIA enabled at $%04X-$%04X (flash root: CWD)",
                     LOCI_MIA_BASE, LOCI_MIA_END);
        }

        /* --loci-web URL: NATIVE LOCI mount of a disk served over HTTP as
         * drive A (loci-webdisk archi B). Twin of --disk-web (Microdisc),
         * but on the LOCI's own FDC. The 6400-byte MFM tracks are
         * fetched on demand. */
        if (core->loci_web_url) {
            if (!loci_dsk_open_web(&emu->loci, 0, core->loci_web_url)) {
                log_error("--loci-web: montage du disque web impossible (%s)", core->loci_web_url);
                return 1;
            }
        }
    }
    return 0;
}

const card_module_t card_loci = {
    .descs = k_descs, .ndescs = 1, .desc_before = "acia",
    .stage = CARD_STAGE_LOCI, .setup = setup,
    .bus = &k_bus, .bus_before = "acia",
};
