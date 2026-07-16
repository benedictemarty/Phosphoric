/**
 * @file video_internal.h
 * @brief Interface interne partagée entre video.c et ocula_video.c.
 *
 * En-tête NON public (pas installé) : expose les quelques helpers OCULA
 * (palette/bordure/registres) extraits vers ocula_video.c mais encore appelés
 * par video.c (palette_latch, video_render_scanline). Voir docs/ocula/CODE-MAP.md.
 */
#ifndef VIDEO_INTERNAL_H
#define VIDEO_INTERNAL_H

#include "video/video.h"

/* Expansion RGB332 → RGB888 (palette + bordure redéfinissables OCULA). */
void rgb332_to_rgb888(uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b);

/* Bloc gaté $BFE0-$BFFF vivant : profil OCULA + déverrouillé + magic 'O','C'. */
bool ocula_block_armed(const video_t* vid, const uint8_t* memory);

/* Fichier de registres write-only OCULA armé (source palette/bordure). */
bool ocula_regs_active(const video_t* vid);

/* Latch de la couleur de bordure pour la scanline y (RGB888), gating OCULA. */
void border_latch(video_t* vid, const uint8_t* memory, int y);

#endif /* VIDEO_INTERNAL_H */
