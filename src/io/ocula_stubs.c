/**
 * @file ocula_stubs.c
 * @brief Stubs no-op des modules OCULA — compilé UNIQUEMENT dans le build
 *        `make OCULA=0` (à la place de ocula_io.c / ocula_gpu.c / ocula_video.c).
 *
 * Permet un binaire **sans OCULA** sans truffer les fichiers cœur de `#ifdef` :
 * les appels OCULA du cœur (video.c, main.c) se résolvent ici en no-op inertes.
 * Le profil `--ula ocula` devient donc silencieusement inactif (rendu HCS10017).
 * Voir docs/ocula/CODE-MAP.md.
 */
#include "video/video.h"
#include "../video/video_internal.h"
#include "io/ocula_io.h"
#include "io/ocula_gpu.h"

/* ── Vidéo (à la place de ocula_video.c) ─────────────────────────────────── */
void rgb332_to_rgb888(uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
    (void)v; *r = *g = *b = 0;
}
bool ocula_block_armed(const video_t* vid, const uint8_t* memory) {
    (void)vid; (void)memory; return false;
}
bool ocula_regs_active(const video_t* vid) { (void)vid; return false; }
void border_latch(video_t* vid, const uint8_t* memory, int y) {
    (void)vid; (void)memory; (void)y;   /* laisse ocula_border[] à 0 = bordure noire */
}
void render_80col_scanline(video_t* vid, const uint8_t* memory, int y) {
    (void)vid; (void)memory; (void)y;   /* jamais atteint : ocula_80col reste faux */
}
void render_exthires_scanline(video_t* vid, const uint8_t* memory, int y) {
    (void)vid; (void)memory; (void)y;
}

/* ── I/O page 3 (à la place de ocula_io.c) ───────────────────────────────── */
uint8_t ocula_io_read(memory_t* mem, uint16_t address) {
    (void)mem; (void)address; return 0xFF;   /* bus ouvert */
}
void ocula_io_write(memory_t* mem, uint16_t address, uint8_t value) {
    (void)mem; (void)address; (void)value;
}

/* ── GPU (à la place de ocula_gpu.c) ─────────────────────────────────────── */
void ocula_gpu_init(ocula_gpu_t* gpu) { (void)gpu; }
uint8_t ocula_gpu_read(const ocula_gpu_t* gpu, uint16_t address) {
    (void)gpu; (void)address; return 0xFF;
}
void ocula_gpu_write(ocula_gpu_t* gpu, memory_t* mem, video_t* vid,
                     uint16_t address, uint8_t value) {
    (void)gpu; (void)mem; (void)vid; (void)address; (void)value;
}
uint8_t ocula_raster_lo(int frame_cycles) { (void)frame_cycles; return 0; }
uint8_t ocula_raster_status(int frame_cycles) { (void)frame_cycles; return 0; }
