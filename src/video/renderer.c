/**
 * @file renderer.c
 * @brief SDL2 renderer (headless mode if SDL2 not available)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.4.0-alpha
 */

#include "video/video.h"

#ifdef HAS_SDL2
#include <SDL2/SDL.h>

static SDL_Window* window;
static SDL_Renderer* sdl_renderer;
static SDL_Texture* texture;
static bool fullscreen;
static int current_scale;
static int tex_w = ORIC_SCREEN_W;   /* Current texture resolution (border incl.) */
static int tex_h = ORIC_SCREEN_H;
static bool border_on = true;       /* Composite the OCULA overscan band (Sprint 65) */
/* Scratch buffer for the bordered composite (sized to the largest mode). */
static uint8_t composed[OCULA_BORDERED_MAX_W * OCULA_BORDERED_MAX_H * 3];

bool renderer_init(int scale, bool prefer_software) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) return false;
    tex_w = ORIC_SCREEN_W;
    tex_h = ORIC_SCREEN_H;
    window = SDL_CreateWindow("Phosphoric",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        ORIC_SCREEN_W * scale, ORIC_SCREEN_H * scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) return false;

    /* Renderer selection. Default is the accelerated driver, but on some
     * setups (llvmpipe/software X, certain NVIDIA/Xwayland combos) the
     * accelerated renderer is created successfully yet presents an all-black
     * window. So:
     *   - --render-software (prefer_software) or SDL_RENDER_DRIVER=software
     *     forces the software renderer up front;
     *   - otherwise we try accelerated and fall back to software if it can't
     *     be created at all.
     * No PRESENTVSYNC flag: SDL_Delay(20 - frame_elapsed) in main.c handles
     * 50 Hz pacing; PRESENTVSYNC could block indefinitely on some Wayland
     * compositors when the window is occluded/minimized. */
    const char* drv = SDL_GetHint(SDL_HINT_RENDER_DRIVER);
    if (drv && SDL_strcasecmp(drv, "software") == 0) prefer_software = true;

    if (prefer_software) {
        sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    } else {
        sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!sdl_renderer) {
            SDL_Log("Accelerated renderer unavailable (%s) — falling back to software",
                    SDL_GetError());
            sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        }
    }
    if (!sdl_renderer) sdl_renderer = SDL_CreateRenderer(window, -1, 0); /* last resort */
    if (!sdl_renderer) return false;
    texture = SDL_CreateTexture(sdl_renderer,
        SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        ORIC_SCREEN_W, ORIC_SCREEN_H);
    if (!texture) return false;
    /* No SDL_RenderSetLogicalSize: SDL_RenderCopy(NULL, NULL) stretches the
     * texture to fill the window exactly, so mode changes (e.g. 80-col OCULA)
     * never produce letterbox bars. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); /* nearest-neighbor */
    fullscreen = false;
    current_scale = scale;
    return true;
}

void renderer_cleanup(void) {
    if (texture) SDL_DestroyTexture(texture);
    if (sdl_renderer) SDL_DestroyRenderer(sdl_renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}

void renderer_present(video_t* vid) {
    /* Source pixels + dimensions: either the active framebuffer directly, or
     * the active image composited inside the OCULA overscan band (Sprint 65). */
    const uint8_t* pixels = vid->framebuffer;
    int src_w = vid->native_w, src_h = vid->native_h;
    if (border_on) {
        video_compose_bordered(vid, composed, &src_w, &src_h);
        pixels = composed;
    }

    /* OCULA extended modes (and toggling the border) change the source
     * resolution at runtime: recreate the streaming texture to follow. */
    if (src_w != tex_w || src_h != tex_h) {
        tex_w = src_w;
        tex_h = src_h;
        if (texture) SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(sdl_renderer,
            SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
            tex_w, tex_h);
        /* No SDL_RenderSetLogicalSize: SDL_RenderCopy fills the window. */
    }
    SDL_UpdateTexture(texture, NULL, pixels, src_w * 3);
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, texture, NULL, NULL);
    SDL_RenderPresent(sdl_renderer);
}

void renderer_set_border(bool on) { border_on = on; }
bool renderer_get_border(void) { return border_on; }

void renderer_toggle_fullscreen(void) {
    fullscreen = !fullscreen;
    SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void renderer_set_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    if (scale == current_scale) return;
    current_scale = scale;
    if (fullscreen) return; /* Don't resize in fullscreen */
    SDL_SetWindowSize(window, tex_w * scale, tex_h * scale);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

int renderer_get_scale(void) {
    return current_scale;
}

void renderer_cycle_scale(void) {
    int next = current_scale + 1;
    if (next > 4) next = 1;
    renderer_set_scale(next);
}

#else

bool renderer_init(int scale, bool prefer_software) { (void)scale; (void)prefer_software; return true; }
void renderer_cleanup(void) {}
void renderer_present(video_t* vid) { (void)vid; }
void renderer_set_border(bool on) { (void)on; }
bool renderer_get_border(void) { return false; }
void renderer_toggle_fullscreen(void) {}
void renderer_set_scale(int scale) { (void)scale; }
int renderer_get_scale(void) { return 1; }
void renderer_cycle_scale(void) {}

#endif
