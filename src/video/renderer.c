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
static int tex_w = ORIC_SCREEN_W;   /* Current texture/native resolution */
static int tex_h = ORIC_SCREEN_H;

bool renderer_init(int scale) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) return false;
    tex_w = ORIC_SCREEN_W;
    tex_h = ORIC_SCREEN_H;
    window = SDL_CreateWindow("Phosphoric",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        ORIC_SCREEN_W * scale, ORIC_SCREEN_H * scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) return false;
    /* No PRESENTVSYNC flag: SDL_Delay(20 - frame_elapsed) in main.c handles
     * 50 Hz pacing. PRESENTVSYNC was redundant and could block indefinitely
     * on some Wayland compositors when the window is occluded/minimized,
     * causing the WM to flag Phosphoric as « ne répond pas ». */
    sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
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
    /* OCULA extended modes change the native resolution at runtime:
     * recreate the streaming texture (and resize the window) to follow. */
    if (vid->native_w != tex_w || vid->native_h != tex_h) {
        tex_w = vid->native_w;
        tex_h = vid->native_h;
        if (texture) SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(sdl_renderer,
            SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
            tex_w, tex_h);
        /* No SDL_RenderSetLogicalSize: SDL_RenderCopy fills the window. */
    }
    SDL_UpdateTexture(texture, NULL, vid->framebuffer, vid->native_w * 3);
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, texture, NULL, NULL);
    SDL_RenderPresent(sdl_renderer);
}

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

bool renderer_init(int scale) { (void)scale; return true; }
void renderer_cleanup(void) {}
void renderer_present(video_t* vid) { (void)vid; }
void renderer_toggle_fullscreen(void) {}
void renderer_set_scale(int scale) { (void)scale; }
int renderer_get_scale(void) { return 1; }
void renderer_cycle_scale(void) {}

#endif
