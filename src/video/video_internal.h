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

/* ── Helpers de rendu partagés (étape 2) ─────────────────────────────────
 * Exposés (non-static) pour que les renderers OCULA (80 col, ext-HIRES) vivent
 * dans ocula_video.c. Restent l'API interne de video.c. */
void    set_pixel(video_t* vid, int x, int y, uint8_t r, uint8_t g, uint8_t b);
void    get_rgb(const video_t* vid, uint8_t oric_color, uint8_t* r, uint8_t* g, uint8_t* b);
uint8_t get_charset_byte(video_t* vid, const uint8_t* mem, int char_idx, int row);
bool    decode_attr(video_t* vid, uint8_t attr, uint8_t* ink, uint8_t* paper, bool* inverse);
void    render_attr_block(video_t* vid, int x, int y, uint8_t paper, int height, bool inverse);
int     effective_chline(video_t* vid, int chline, int row);
bool    blink_phase_on(video_t* vid);

/* Renderers de scanline OCULA (dans ocula_video.c). */
void render_80col_scanline(video_t* vid, const uint8_t* memory, int y);
void render_exthires_scanline(video_t* vid, const uint8_t* memory, int y);

#endif /* VIDEO_INTERNAL_H */
