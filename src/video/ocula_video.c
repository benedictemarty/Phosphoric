/**
 * @file ocula_video.c
 * @brief OCULA — palette/bordure redéfinissables + compositing overscan.
 *
 * Extrait de video.c (branche feature/ocula-isolation, cf docs/ocula/CODE-MAP.md).
 * Cluster OCULA sans dépendance aux helpers de rendu de video.c (set_pixel,
 * get_rgb, charset…) : décodage RGB332, gating du bloc/registres, latch bordure
 * par scanline, et composition du framebuffer dans sa bande overscan.
 *
 * Les renderers de scanlines OCULA (80 colonnes, ext-HIRES) restent dans
 * video.c pour l'instant : trop couplés aux helpers statiques (à extraire
 * ensuite via un video_internal.h élargi).
 */
#include "video/video.h"
#include "video_internal.h"
#include <string.h>

/* Expansion d'un octet RGB332 (RRRGGGBB) en RGB888. Partagé par la palette
 * redéfinissable et le registre de bordure — tous deux dans le bloc gaté
 * $BFE0-$BFFF, donc décodés à l'identique. */
void rgb332_to_rgb888(uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = (uint8_t)((((v >> 5) & 0x07) * 255) / 7);
    *g = (uint8_t)((((v >> 2) & 0x07) * 255) / 7);
    *b = (uint8_t)(((v & 0x03) * 255) / 3);
}

/* Vrai quand le bloc gaté $BFE0-$BFFF est vivant : profil OCULA, déverrouillé,
 * et magic 'O','C' armé à OCULA_PAL_MAGIC. Opt-in (sprint 45) : sinon le bloc
 * reste du stockage brut (certains jeux y gardent des données — cf. Dbug, forum
 * t=2709), et ni la palette ni la bordure ne réagissent. */
bool ocula_block_armed(const video_t* vid, const uint8_t* memory) {
    return vid->ula_profile == ULA_PROFILE_OCULA && vid->ocula_unlocked &&
           memory[OCULA_PAL_MAGIC] == 'O' && memory[OCULA_PAL_MAGIC + 1] == 'C';
}

/* Vrai quand le fichier de registres write-only OCULA est la source active de
 * la palette + bordure (sprint 66) : un registre a été écrit depuis le
 * déverrouillage. Lu en direct via le pointeur câblé depuis memory_t ; NULL
 * dans le chemin de test unitaire nu. Armé, il prime sur le bloc in-band
 * $BFE0-$BFFF (transition vers le schéma de registres de sodiumlb). */
bool ocula_regs_active(const video_t* vid) {
    return vid->ula_profile == ULA_PROFILE_OCULA &&   /* jamais sur une ULA stock */
           vid->ocula_regs_armed && *vid->ocula_regs_armed &&
           vid->ocula_reg_pal && vid->ocula_reg_border;
}

/* Latch de la couleur de bordure pour la scanline y depuis OCULA_BORDER_REG
 * ($BFEA), RGB332, sous le même gating que la palette. Noir ($00 / désarmé /
 * ULA stock) reproduit la bordure Oric standard. Relu à chaque scanline → une
 * valeur réécrite entre lignes donne des barres raster de bordure (sprint 64,
 * cf. Dbug forum t=2709). Le framebuffer n'a pas encore de bande overscan — ceci
 * ne remplit que le modèle par-ligne exposé par video_get_border_rgb(). */
void border_latch(video_t* vid, const uint8_t* memory, int y) {
    if (y < 0 || y >= OCULA_MAX_H) return;
    if (ocula_regs_active(vid)) {
        rgb332_to_rgb888(*vid->ocula_reg_border,
                         &vid->ocula_border[y][0], &vid->ocula_border[y][1],
                         &vid->ocula_border[y][2]);
    } else if (ocula_block_armed(vid, memory)) {
        rgb332_to_rgb888(memory[OCULA_BORDER_REG],
                         &vid->ocula_border[y][0], &vid->ocula_border[y][1],
                         &vid->ocula_border[y][2]);
    } else {
        vid->ocula_border[y][0] = 0;
        vid->ocula_border[y][1] = 0;
        vid->ocula_border[y][2] = 0;
    }
}

/* Couleur de bordure OCULA latchée pour la scanline y (0-223), RGB888. */
void video_get_border_rgb(const video_t* vid, int y,
                          uint8_t* r, uint8_t* g, uint8_t* b) {
    if (y < 0 || y >= OCULA_MAX_H) { *r = *g = *b = 0; return; }
    *r = vid->ocula_border[y][0];
    *g = vid->ocula_border[y][1];
    *b = vid->ocula_border[y][2];
}

int video_bordered_w(const video_t* vid) {
    return vid->native_w + 2 * OCULA_BORDER_W;
}

int video_bordered_h(const video_t* vid) {
    return vid->native_h + 2 * OCULA_BORDER_H;
}

/* Compose le framebuffer actif dans sa bande overscan OCULA (RGB888). */
void video_compose_bordered(const video_t* vid, uint8_t* out, int* w, int* h) {
    int aw = vid->native_w, ah = vid->native_h;
    int tw = aw + 2 * OCULA_BORDER_W;
    int th = ah + 2 * OCULA_BORDER_H;

    for (int ty = 0; ty < th; ty++) {
        /* Ligne active sous cette rangée de sortie (négative / au-delà dans les
         * bandes haut & bas, où l'on clampe à la première/dernière ligne). */
        int ay = ty - OCULA_BORDER_H;
        int cy = ay < 0 ? 0 : (ay >= ah ? ah - 1 : ay);
        const uint8_t* bc = vid->ocula_border[cy];

        uint8_t* row = out + (size_t)ty * tw * 3;
        for (int tx = 0; tx < tw; tx++) {
            row[tx * 3 + 0] = bc[0];
            row[tx * 3 + 1] = bc[1];
            row[tx * 3 + 2] = bc[2];
        }
        /* Superpose la scanline active dans sa bordure gauche/droite. */
        if (ay >= 0 && ay < ah) {
            const uint8_t* src = vid->framebuffer + (size_t)ay * aw * 3;
            memcpy(row + (size_t)OCULA_BORDER_W * 3, src, (size_t)aw * 3);
        }
    }
    if (w) *w = tw;
    if (h) *h = th;
}
