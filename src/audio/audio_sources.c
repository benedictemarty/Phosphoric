/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file audio_sources.c
 * @brief Sources audio des cartes d'extension en modules, mixées au PSG par le
 *        callback SDL (audio_output.c) et par la capture (main.c). Sans
 *        dépendance : les tests qui lient une carte n'ont pas besoin de SDL.
 * @author bmarty <bmarty@mailo.com>
 */
#include "audio/audio.h"

#define AUDIO_SOURCES_MAX 8
static struct { audio_source_fn fn; void* ctx; } s_sources[AUDIO_SOURCES_MAX];
static int s_nsources;

void audio_add_source(audio_source_fn fn, void* ctx) {
    for (int i = 0; i < s_nsources; i++)
        if (s_sources[i].fn == fn && s_sources[i].ctx == ctx) return;
    if (!fn || s_nsources >= AUDIO_SOURCES_MAX) return;
    s_sources[s_nsources].fn = fn;
    s_sources[s_nsources].ctx = ctx;
    s_nsources++;                      /* après l'entrée : le callback la voit entière */
}

void audio_mix_sources(int16_t* stereo, int n, int chunk_max) {
    int16_t mbuf[1024];
    if (chunk_max > (int)(sizeof(mbuf) / sizeof(mbuf[0]))) chunk_max = 1024;
    for (int s = 0; s < s_nsources; s++) {
        int done = 0;
        while (done < n) {
            int chunk = n - done;
            if (chunk > chunk_max) chunk = chunk_max;
            if (!s_sources[s].fn(s_sources[s].ctx, mbuf, chunk)) break;
            for (int i = 0; i < chunk; i++) {
                int idx = (done + i) * 2;
                stereo[idx]     = (int16_t)((stereo[idx]     + mbuf[i]) / 2);
                stereo[idx + 1] = (int16_t)((stereo[idx + 1] + mbuf[i]) / 2);
            }
            done += chunk;
        }
    }
}

