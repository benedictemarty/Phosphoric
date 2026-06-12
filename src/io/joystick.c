/**
 * @file joystick.c
 * @brief IJK joystick interface for Phosphoric
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.6.0-alpha
 *
 * Implements the IJK joystick interface — the most common ORIC joystick
 * adapter. Lives on the printer port = VIA Port A direct (enable on
 * PB4 low, stick select on pins 6-7, state on pins 0-5 active low,
 * presence on bit 5) — cf. oric_joystick_port_a_pins() and joystick.h.
 *
 * Supports SDL2 game controllers and keyboard-as-joystick mode
 * (arrow keys + Right Ctrl as fire).
 */

#include "io/joystick.h"
#include "utils/logging.h"
#include <string.h>

void oric_joystick_init(oric_joystick_t* joy) {
    memset(joy, 0, sizeof(*joy));
    joy->port_a_mask = 0xFF;  /* All released (active low) */
    joy->mode = ORIC_JOY_DISABLED;
#ifdef HAS_SDL2
    joy->controller = NULL;
    joy->joystick = NULL;
    joy->device_index = -1;
#endif
}

void oric_joystick_reset(oric_joystick_t* joy) {
    joy->port_a_mask = 0xFF;
}

void oric_joystick_set_mode(oric_joystick_t* joy, oric_joy_mode_t mode) {
    joy->mode = mode;
    joy->port_a_mask = 0xFF;
    log_info("Joystick mode: %s",
             mode == ORIC_JOY_DISABLED ? "disabled" :
             mode == ORIC_JOY_SDL_GAMEPAD ? "SDL gamepad" :
             "keyboard");
}

void oric_joystick_press(oric_joystick_t* joy, uint8_t button_mask) {
    joy->port_a_mask &= ~button_mask;  /* Clear bits = pressed (active low) */
}

void oric_joystick_release(oric_joystick_t* joy, uint8_t button_mask) {
    joy->port_a_mask |= button_mask;   /* Set bits = released (active low) */
}

void oric_joystick_release_all(oric_joystick_t* joy) {
    joy->port_a_mask = 0xFF;
}

uint8_t oric_joystick_read(const oric_joystick_t* joy) {
    if (joy->mode == ORIC_JOY_DISABLED) {
        return 0xFF;  /* No joystick: all bits high */
    }
    return joy->port_a_mask;
}

uint8_t oric_joystick_port_a_pins(const oric_joystick_t* joy,
                                  uint8_t via_ora, uint8_t via_ddra,
                                  uint8_t via_orb, uint8_t via_ddrb) {
    uint8_t joysel, out;

    if (joy->mode == ORIC_JOY_DISABLED) return 0xFF;
    /* Enable: PB4 (printer strobe) output and driven low */
    if (!(via_ddrb & 0x10)) return 0xFF;
    if (via_orb & 0x10) return 0xFF;

    out = (uint8_t)~IJK_PRESENCE;       /* presence line pulled low */
    /* Stick select = Port A pins as driven by the program (output
     * bits from ORA, floating input lines read high) */
    joysel = (uint8_t)((via_ora & via_ddra) | ~via_ddra);
    if ((joysel & 0xC0) == 0xC0) return out;   /* both selects = none */
    if (joysel & 0x40) {
        out &= joy->port_a_mask;               /* stick A */
    }
    /* bit 7 = stick B: not wired (single emulated joystick) */
    return out;
}

/* ═══════════════════════════════════════════════════════════════ */
/*  SDL2 support                                                   */
/* ═══════════════════════════════════════════════════════════════ */

#ifdef HAS_SDL2

bool oric_joystick_open_sdl(oric_joystick_t* joy, int device_index) {
    /* Try game controller first (better button mapping) */
    if (SDL_IsGameController(device_index)) {
        joy->controller = SDL_GameControllerOpen(device_index);
        if (joy->controller) {
            joy->device_index = device_index;
            const char* name = SDL_GameControllerName(joy->controller);
            log_info("Joystick: opened game controller '%s' (index %d)",
                     name ? name : "Unknown", device_index);
            return true;
        }
    }

    /* Fallback to basic joystick */
    joy->joystick = SDL_JoystickOpen(device_index);
    if (joy->joystick) {
        joy->device_index = device_index;
        const char* name = SDL_JoystickName(joy->joystick);
        log_info("Joystick: opened joystick '%s' (index %d)",
                 name ? name : "Unknown", device_index);
        return true;
    }

    log_error("Joystick: failed to open device %d", device_index);
    return false;
}

void oric_joystick_close_sdl(oric_joystick_t* joy) {
    if (joy->controller) {
        SDL_GameControllerClose(joy->controller);
        joy->controller = NULL;
    }
    if (joy->joystick) {
        SDL_JoystickClose(joy->joystick);
        joy->joystick = NULL;
    }
    joy->device_index = -1;
}

/**
 * @brief Map SDL2 game controller button to IJK mask
 */
static uint8_t gamepad_button_to_ijk(SDL_GameControllerButton button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    return IJK_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return IJK_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return IJK_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return IJK_RIGHT;
        case SDL_CONTROLLER_BUTTON_A:          return IJK_FIRE;
        case SDL_CONTROLLER_BUTTON_B:          return IJK_FIRE;
        case SDL_CONTROLLER_BUTTON_X:          return IJK_FIRE;
        default: return 0;
    }
}

/** Axis dead zone threshold (out of 32767) */
#define JOY_AXIS_DEADZONE 8000

bool oric_joystick_handle_sdl_event(oric_joystick_t* joy, const SDL_Event* event) {
    if (joy->mode == ORIC_JOY_DISABLED) {
        return false;
    }

    /* Keyboard-as-joystick mode: arrow keys + Right Ctrl = fire */
    if (joy->mode == ORIC_JOY_KEYBOARD) {
        if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
            uint8_t mask = 0;
            switch (event->key.keysym.sym) {
                case SDLK_UP:    mask = IJK_UP;    break;
                case SDLK_DOWN:  mask = IJK_DOWN;  break;
                case SDLK_LEFT:  mask = IJK_LEFT;  break;
                case SDLK_RIGHT: mask = IJK_RIGHT; break;
                case SDLK_RCTRL:
                case SDLK_RALT:  mask = IJK_FIRE;  break;
                default: return false;
            }
            if (event->type == SDL_KEYDOWN) {
                oric_joystick_press(joy, mask);
            } else {
                oric_joystick_release(joy, mask);
            }
            return true;
        }
        return false;
    }

    /* SDL gamepad mode */
    switch (event->type) {
        case SDL_CONTROLLERBUTTONDOWN: {
            uint8_t mask = gamepad_button_to_ijk(
                (SDL_GameControllerButton)event->cbutton.button);
            if (mask) {
                oric_joystick_press(joy, mask);
                return true;
            }
            break;
        }
        case SDL_CONTROLLERBUTTONUP: {
            uint8_t mask = gamepad_button_to_ijk(
                (SDL_GameControllerButton)event->cbutton.button);
            if (mask) {
                oric_joystick_release(joy, mask);
                return true;
            }
            break;
        }
        case SDL_CONTROLLERAXISMOTION: {
            /* Left stick X axis → Left/Right */
            if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                oric_joystick_release(joy, IJK_LEFT | IJK_RIGHT);
                if (event->caxis.value < -JOY_AXIS_DEADZONE) {
                    oric_joystick_press(joy, IJK_LEFT);
                } else if (event->caxis.value > JOY_AXIS_DEADZONE) {
                    oric_joystick_press(joy, IJK_RIGHT);
                }
                return true;
            }
            /* Left stick Y axis → Up/Down */
            if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                oric_joystick_release(joy, IJK_UP | IJK_DOWN);
                if (event->caxis.value < -JOY_AXIS_DEADZONE) {
                    oric_joystick_press(joy, IJK_UP);
                } else if (event->caxis.value > JOY_AXIS_DEADZONE) {
                    oric_joystick_press(joy, IJK_DOWN);
                }
                return true;
            }
            break;
        }
        case SDL_JOYHATMOTION: {
            /* D-pad hat for basic joysticks */
            oric_joystick_release(joy, IJK_UP | IJK_DOWN | IJK_LEFT | IJK_RIGHT);
            if (event->jhat.value & SDL_HAT_UP)    oric_joystick_press(joy, IJK_UP);
            if (event->jhat.value & SDL_HAT_DOWN)  oric_joystick_press(joy, IJK_DOWN);
            if (event->jhat.value & SDL_HAT_LEFT)  oric_joystick_press(joy, IJK_LEFT);
            if (event->jhat.value & SDL_HAT_RIGHT) oric_joystick_press(joy, IJK_RIGHT);
            return true;
        }
        case SDL_JOYBUTTONDOWN:
            oric_joystick_press(joy, IJK_FIRE);
            return true;
        case SDL_JOYBUTTONUP:
            oric_joystick_release(joy, IJK_FIRE);
            return true;
        default:
            break;
    }

    return false;
}

#endif /* HAS_SDL2 */
