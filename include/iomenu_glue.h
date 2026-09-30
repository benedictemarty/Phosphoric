/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file iomenu_glue.h
 * @brief Liaison entre le menu des périphériques (F1) et l'émulateur.
 * @author bmarty <bmarty@mailo.com>
 *
 * - Opérations sur les médias, partagées par le menu F1 et le sélecteur F6 :
 *   insérer / éjecter une disquette ou une cassette.
 * - iomenu_refresh() : remplit l'état affiché depuis emulator_t.
 * - iomenu_apply() : exécute une action renvoyée par le menu.
 * - phosphoric.cfg : enregistrement (bouton du menu) et relecture au lancement.
 */
#ifndef IOMENU_GLUE_H
#define IOMENU_GLUE_H

#include <stdbool.h>
#include <stddef.h>
#include "emulator.h"
#include "cli/cli_opts.h"
#include "video/iomenu.h"

typedef enum {
    MEDIA_OK = 0,
    MEDIA_NO_IFACE,        /* pas d'interface disque (Microdisc / Jasmin) */
    MEDIA_BAD_DRIVE,       /* lecteur hors de l'interface */
    MEDIA_EMPTY,           /* rien à éjecter */
    MEDIA_LOAD_FAILED      /* fichier illisible / invalide */
} media_result_t;

/* Réécrit l'image du lecteur si elle a été modifiée et que --disk-writeback est actif. */
bool           media_disk_writeback(emulator_t* emu, int drv);
media_result_t media_disk_insert(emulator_t* emu, int drv, const char* path);
media_result_t media_disk_eject(emulator_t* emu, int drv);
media_result_t media_tape_insert(emulator_t* emu, const char* path);
media_result_t media_tape_eject(emulator_t* emu);

void iomenu_refresh(emulator_t* emu);
/* Exécute l'action ; renvoie true si le menu doit se fermer. */
bool iomenu_apply(emulator_t* emu, const iom_action_t* act);

/* Fichier de configuration par défaut (dossier courant). */
#define IOMENU_CONFIG_DEFAULT "phosphoric.cfg"

/* Enregistre les réglages gérés par le menu dans `path`, en gardant les autres
 * lignes du fichier (commentaires, clés inconnues). */
bool iomenu_config_save(emulator_t* emu, const char* path);
/* Relit `path` et complète `cfg` : une option déjà donnée en ligne de commande
 * est prioritaire. Renvoie le nombre de réglages appliqués, -1 si le fichier
 * n'existe pas. */
int  iomenu_config_load(const char* path, cli_opts_t* cfg);

/* Rend le menu (état courant de l'émulateur) dans un fichier PPM 640 × 640. */
bool iomenu_screenshot(emulator_t* emu, const char* path);

#endif /* IOMENU_GLUE_H */
