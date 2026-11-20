#ifndef FG_AMIGA_SDL_MIXER_H
#define FG_AMIGA_SDL_MIXER_H
#include <SDL3/SDL.h>
struct MIX_Mixer;   /* audio backend state */
struct MIX_Audio;   /* 8-bit signed mono sample */
struct MIX_Track;   /* mixer voice handle */
typedef struct MIX_Mixer MIX_Mixer;
typedef struct MIX_Audio MIX_Audio;
typedef struct MIX_Track MIX_Track;
#endif
