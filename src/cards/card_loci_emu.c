/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_loci_emu.c
 * @brief Menu entry « LOCI (co-simulation) »: the cartridge served by the
 *        real firmware (--loci-emu), available only with the « emul »
 *        backend (make LOCI_EMU=1; filtered by cards.c).
 * @author bmarty <bmarty@mailo.com>
 *
 * Module separate from card_loci.c because its entry is not next to the LOCI
 * one in the menu; its setup is that of LOCI (card_loci.c).
 */
#include "card_module.h"

static const card_desc_t k_desc = {
        "loci_emu", "LOCI firmware",
        "LOCI co-simulé : le vrai firmware RP2040 tourne dans l'émulateur "
        "(développement du firmware).",
        "disque", "--loci-emu", 0, -1, 0x03A0, 32, false,
        { { "elf", "Firmware (ELF)", CARD_P_FILE, "--loci-emu", "",
            "Fichier loci-firmware.elf compilé pour RP2040." },
          { "flash", "Image flash", CARD_P_FILE, "--loci-emu-flash", "",
            "Mémoire flash persistante du firmware ; vide : <ELF>.flash ; « - » : "
            "volatile." },
          { "usb", "Image de clé USB", CARD_P_FILE, "--loci-usb-image", "",
            "Image FAT servie au firmware comme clé USB ; vide : aucune." } },
        3
    };
static const card_desc_t* const k_descs[] = { &k_desc };

const card_module_t card_loci_emu = {
    .descs = k_descs, .ndescs = 1, .desc_before = "ula_ng",
};
