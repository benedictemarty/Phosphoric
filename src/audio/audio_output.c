/**
 * @file audio_output.c
 * @brief Audio output backend (SDL2 or headless)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.5.0-alpha
 */

#include "audio/audio.h"
#include "network/cast_server.h"
#include <stdlib.h>
#include <string.h>

/* Cast server reference for audio forwarding */
static cast_server_t* cast_server_ref = NULL;

void audio_set_cast_server(cast_server_t* server) {
    cast_server_ref = server;
}

#ifdef HAS_SDL2
#include <SDL2/SDL.h>

static SDL_AudioDeviceID audio_device;
static ay3891x_t* psg_ref;

/* ── AVI audio tap (GUI) ─────────────────────────────────────────────
 * In GUI the SDL audio callback owns the PSG generator (ay_generate drains the
 * event queue). To also mux the sound into a --video AVI, the callback copies
 * the PCM it just produced into this ring FIFO, and the main loop drains it
 * once per video frame (avi_recorder_add_audio). Access from the two threads is
 * serialised by SDL_LockAudioDevice : the drain locks the device (which blocks
 * the callback), and the push runs inside the callback itself — so ring state
 * is never touched concurrently. Stereo, interleaved 16-bit LE. */
enum { TAP_CAP = 8192 };          /* sample-frames of backlog (~0.18 s @44.1k) */
static int16_t* tap_buf;          /* TAP_CAP * 2 int16, NULL = tap disabled */
static int      tap_tail;         /* read cursor, in sample-frames */
static int      tap_count;        /* valid sample-frames available */

/* Called only from inside audio_callback (audio thread) → no extra lock. */
static void tap_push(const int16_t* samples, int nframes) {
    if (!tap_buf) return;
    for (int i = 0; i < nframes; i++) {
        int w = (tap_tail + tap_count) % TAP_CAP;
        tap_buf[2 * w]     = samples[2 * i];
        tap_buf[2 * w + 1] = samples[2 * i + 1];
        if (tap_count == TAP_CAP)
            tap_tail = (tap_tail + 1) % TAP_CAP;  /* overflow: drop oldest */
        else
            tap_count++;
    }
}

static void audio_callback(void* userdata, uint8_t* stream, int len) {
    (void)userdata;
    int16_t* buf = (int16_t*)stream;
    int num_samples = len / (2 * sizeof(int16_t)); /* stereo */
    if (psg_ref) ay_generate(psg_ref, buf, num_samples);
    else memset(stream, 0, len);

    /* Copy the freshly generated PCM into the AVI tap (no-op if disabled). */
    tap_push(buf, num_samples);

    /* Forward audio to cast server if connected */
    if (cast_server_ref) {
        cast_server_push_audio(cast_server_ref, buf, num_samples);
    }
}

bool audio_avi_tap_enable(void) {
    if (tap_buf) return true;                    /* already enabled */
    int16_t* b = (int16_t*)calloc((size_t)TAP_CAP * 2, sizeof(int16_t));
    if (!b) return false;
    /* Lock so the callback can't observe a half-initialised ring. */
    if (audio_device) SDL_LockAudioDevice(audio_device);
    tap_tail = 0;
    tap_count = 0;
    tap_buf = b;
    if (audio_device) SDL_UnlockAudioDevice(audio_device);
    return true;
}

void audio_avi_tap_disable(void) {
    if (!tap_buf) return;
    int16_t* b = tap_buf;
    if (audio_device) SDL_LockAudioDevice(audio_device);
    tap_buf = NULL;
    tap_count = 0;
    tap_tail = 0;
    if (audio_device) SDL_UnlockAudioDevice(audio_device);
    free(b);
}

int audio_avi_tap_drain(int16_t* out, int max_frames) {
    if (!tap_buf || !out || max_frames <= 0) return 0;
    int n = 0;
    if (audio_device) SDL_LockAudioDevice(audio_device);
    n = (tap_count < max_frames) ? tap_count : max_frames;
    for (int i = 0; i < n; i++) {
        int r = (tap_tail + i) % TAP_CAP;
        out[2 * i]     = tap_buf[2 * r];
        out[2 * i + 1] = tap_buf[2 * r + 1];
    }
    tap_tail = (tap_tail + n) % TAP_CAP;
    tap_count -= n;
    if (audio_device) SDL_UnlockAudioDevice(audio_device);
    return n;
}

bool audio_init(ay3891x_t* psg) {
    psg_ref = psg;
    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = AUDIO_BUFFER_SIZE;
    want.callback = audio_callback;

    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_device == 0) return false;
    SDL_PauseAudioDevice(audio_device, 0);
    return true;
}

void audio_cleanup(void) {
    if (audio_device) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
}

void audio_pause(bool pause) {
    if (audio_device) SDL_PauseAudioDevice(audio_device, pause ? 1 : 0);
}

#else

bool audio_init(ay3891x_t* psg) { (void)psg; return true; }
void audio_cleanup(void) {}
void audio_pause(bool pause) { (void)pause; }

/* Headless has no SDL audio thread : the AVI audio is generated inline in the
 * main loop (ay_generate), so the tap is a no-op here. */
bool audio_avi_tap_enable(void) { return false; }
void audio_avi_tap_disable(void) {}
int  audio_avi_tap_drain(int16_t* out, int max_frames) { (void)out; (void)max_frames; return 0; }

#endif
