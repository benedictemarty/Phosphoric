/**
 * @file export.h
 * @brief Video framebuffer export (PPM, BMP, ASCII)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-beta.2
 */

#ifndef VIDEO_EXPORT_H
#define VIDEO_EXPORT_H

#include <stdio.h>
#include "video/video.h"

/**
 * @brief Export framebuffer as PPM (P6 binary format)
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path (.ppm)
 * @return true on success
 */
bool video_export_ppm(const video_t* vid, const char* filename);

/**
 * @brief Export framebuffer as BMP (24-bit uncompressed)
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path (.bmp)
 * @return true on success
 */
bool video_export_bmp(const video_t* vid, const char* filename);

/**
 * @brief Export framebuffer as ANSI true-color text to a file
 * @param vid Video context with rendered framebuffer
 * @param fp Output FILE pointer (e.g. stdout)
 * @param scale_x Horizontal pixel grouping (e.g. 2 = half width)
 * @param scale_y Vertical pixel grouping (e.g. 2 = half height)
 * @return true on success
 */
bool video_export_ascii(const video_t* vid, FILE* fp, unsigned int scale_x, unsigned int scale_y);

/**
 * @brief Comme video_export_ascii() mais ouvre/ferme le fichier lui-même.
 * @param vid Video context with rendered framebuffer
 * @param filename Chemin du fichier de sortie (texte, échappements ANSI)
 * @param scale_x Regroupement horizontal de pixels (0 => défaut 2)
 * @param scale_y Regroupement vertical de pixels (0 => défaut 2)
 * @return true on success
 */
bool video_export_ascii_file(const video_t* vid, const char* filename,
                             unsigned int scale_x, unsigned int scale_y);

/**
 * @brief Exporte le CONTENU TEXTE réel de l'écran ($BB80, 40x28) en ASCII lisible.
 *
 * Lit directement le buffer texte ORIC en RAM (pas le framebuffer). Chaque octet
 * est décodé par `octet & 0x7F` (suppression du bit vidéo inverse) ; les codes de
 * contrôle/attributs (< 0x20) deviennent des espaces. Chaque ligne est écrite sur
 * 40 colonnes maximum, espaces de fin supprimés, terminée par '\n'.
 *
 * @note Approximation : suppose le jeu de caractères standard ORIC, pour lequel
 *       0x20-0x7F coïncide avec l'ASCII. Les charsets redéfinis ne sont pas résolus.
 * @note Lit toujours les 28 lignes de $BB80, indépendamment du mode (TEXT/HIRES).
 *
 * @param memory Pointeur sur la RAM 64KB (index $BB80 lu directement)
 * @param fp Fichier de sortie (ex. stdout)
 * @return true on success
 */
bool video_export_screen_text(const uint8_t* memory, FILE* fp);

/**
 * @brief Auto-detect format from filename extension and export
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path (.ppm or .bmp)
 * @return true on success
 */
bool video_export_auto(const video_t* vid, const char* filename);

/**
 * @brief Like video_export_auto(), but with the overscan border
 *        composited around the active area (larger image). Format auto-detected
 *        from the extension (.bmp → BMP, else PPM).
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path
 * @return true on success, false on error / allocation failure
 */
bool video_export_auto_bordered(const video_t* vid, const char* filename);

#endif /* VIDEO_EXPORT_H */
