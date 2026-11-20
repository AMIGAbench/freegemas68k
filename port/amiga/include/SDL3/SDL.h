/* Minimal SDL3 replacement for the AmigaOS port of freegemas.
 * Only the types/constants the game code uses. The opaque structs are
 * defined by the Amiga platform layer (port/amiga/amiga_internal.h). */
#ifndef FG_AMIGA_SDL_H
#define FG_AMIGA_SDL_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint64_t Uint64;

typedef struct SDL_Color { Uint8 r, g, b, a; } SDL_Color;
typedef struct SDL_FRect { float x, y, w, h; } SDL_FRect;
typedef struct SDL_Rect  { int x, y, w, h; } SDL_Rect;

struct SDL_Window;    /* Amiga display (screen/window) */
struct SDL_Renderer;  /* software compositor */
struct SDL_Texture;   /* RGB565 + optional A8 */
struct SDL_Surface;   /* RGBA8888 scratch image */
struct SDL_Gamepad;   /* joystick port (lowlevel.library) */

typedef Uint32 SDL_Keycode;

/* Key codes (values as in SDL3) */
#define SDLK_RETURN    0x0000000du
#define SDLK_ESCAPE    0x0000001bu
#define SDLK_SPACE     0x00000020u
#define SDLK_H         0x00000068u
#define SDLK_F         0x00000066u
#define SDLK_RIGHT     0x4000004fu
#define SDLK_LEFT      0x40000050u
#define SDLK_DOWN      0x40000051u
#define SDLK_UP        0x40000052u
#define SDLK_KP_ENTER  0x40000058u

#define SDL_BUTTON_LEFT   1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT  3

typedef enum SDL_GamepadButton {
    SDL_GAMEPAD_BUTTON_INVALID = -1,
    SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_GUIDE,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT
} SDL_GamepadButton;

#ifdef __cplusplus
extern "C" {
#endif
Uint64 SDL_GetTicks(void);
void   SDL_Delay(Uint32 ms);
void   SDL_DestroyTexture(struct SDL_Texture *texture);
void   SDL_Log(const char *fmt, ...);
#ifdef __cplusplus
}
#endif

#endif
