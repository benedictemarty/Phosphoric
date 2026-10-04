/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_loci_emu.c
 * @brief Fiches du menu « LOCI firmware » (co-simulation, --loci-emu, backend
 *        « emul » : make LOCI_EMU=1) et « LOCI réelle » (la vraie cartouche
 *        par le pont USB loci-usb d'une Feather RP2040, --loci-hw, backend
 *        « hw » : make LOCI_HW=1) ; chacune n'apparaît qu'avec son backend
 *        (filtrées par cards.c).
 * @author bmarty <bmarty@mailo.com>
 *
 * Module à part de card_loci.c parce que ses fiches ne sont pas à côté de celle
 * de LOCI dans le menu ; leur mise en route est celle de LOCI (card_loci.c).
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
/* Port vide : cards.c le cherche par le produit USB « LOCI-USB… » (Linux). */
static const card_desc_t k_desc_hw = {
        "loci_hw", "LOCI réelle",
        "La vraie cartouche LOCI branchée par le pont USB loci-usb (Feather RP2040) : "
        "le 6502 émulé lit et écrit son bus, menu et ROM compris.",
        "disque", "--loci-hw", 0, -1, 0x03A0, 32, false,
        { { "port", "Port du pont USB", CARD_P_TEXT, "--loci-hw", "",
            "Port série de la Feather (ex. /dev/ttyACM0). Vide : détection automatique "
            "par son nom USB « LOCI-USB » (Linux). Bouton MENU : F8." } },
        1
    };
static const card_desc_t* const k_descs[] = { &k_desc, &k_desc_hw };

const card_module_t card_loci_emu = {
    .descs = k_descs, .ndescs = 2, .desc_before = "ula_ng",
};
