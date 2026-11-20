/* Internal interfaces of the AmigaOS platform layer of freegemas. */
#ifndef FG_AMIGA_INTERNAL_H
#define FG_AMIGA_INTERNAL_H

#include <SDL3/SDL.h>
#include <stdint.h>
#include <vector>
#include <memory>
#include <string>

/* ------------------------------------------------------------------ config */
enum FgAudioMode { FG_AUDIO_AUTO, FG_AUDIO_AHI, FG_AUDIO_PAULA, FG_AUDIO_OFF };

struct FgConfig {
    int forceDisplay;    /* -1: from the options, 0: window, 1: fullscreen */
    int audioMode;       /* FgAudioMode */
    int ahiUnit;         /* ahi.device unit (0..3) */
    int frameStats;      /* STATS: frame timing in the log */
    int maxFrames;       /* > 0: quit after that many frames (test runs) */
    const char *shots;   /* "100,250": dump these frames to PROGDIR:shot_<n>.raw */
    const char *script;  /* "120=RETURN;300=CLICK/400/300": input for test runs */
    int seed;            /* != 0: fixed random seed (reproducible boards) */
    int present;         /* 0 auto, 1 friend bitmap, 2 direct (fullscreen), 3 ARGB */
    int fullRedraw;      /* test: compose every frame completely (reference for the dirty rects) */
    int statN;           /* STATS interval in frames */
    int noAmmx;          /* NOAMMX: C blending on the 68080 too */
    int noAsm;           /* NOASM: the C loops instead of the 68k kernels */
    int debugLayer;      /* DEBUGLAYER: log the static layer's work */
    int noPrescale;      /* NOPRESCALE: scaled sprites sampled every frame (A/B) */
    int log;             /* LOG=YES: write PROGDIR:freegemas.log */
};
extern FgConfig g_fgConfig;

/* PROGDIR:freegemas.log -- only with the tool type LOG=YES (or LOG, STATS in the Shell);
   main task only. No serial output. */
void fg_info(const char *fmt, ...);
#define FG_LOG(...) fg_info(__VA_ARGS__)
void fg_ammx_init();
void fg_icon_set_fullscreen(bool on);
void fg_kernels_init();
extern int g_kConv, g_kMix;

/* ------------------------------------------------------------------ timer */
bool     fg_timer_open();
void     fg_timer_close();
uint32_t fg_micros();          /* free running microsecond counter */
void     fg_delay_us(uint32_t us);

/* ------------------------------------------------------------------ textures */
enum { FG_TEX_OPAQUE = 0, FG_TEX_MASK = 1, FG_TEX_ALPHA = 2 };

struct SDL_Texture {
    int       w, h;
    uint16_t *rgb;       /* w*h RGB565 */
    uint8_t  *alpha;     /* w*h coverage (soft-alpha textures only) */
    uint8_t  *mask;      /* 1-bit mask, (w+7)/8 bytes per row (hard-mask textures, from the file) */
    uint8_t   kind;      /* FG_TEX_OPAQUE, FG_TEX_MASK (cookie-cut blit), FG_TEX_ALPHA */
    uint32_t  id;        /* unique, never reused: identifies the content */
    /* (made by fg_texture_finish) */
    int16_t   bx0, by0, bx1, by1;    /* bounding box of the visible pixels (alpha > 0) */
    uint16_t *runs;      /* per row: runs of (kind << 14 | length), kind 0 skip, 1 blend, 2 opaque */
    uint32_t *rowRun;    /* h+1 offsets into runs */
};

void fg_texture_finish(SDL_Texture *t);
void fg_tint_cache_free();

SDL_Texture *fg_texture_new(int w, int h, bool withAlpha);
SDL_Texture *fg_texture_load(const char *path);                 /* .gfx */
SDL_Texture *fg_texture_from_rgba(const uint8_t *rgba, int w, int h);

/* RGBA8888 scratch image (font rendering) */
struct SDL_Surface {
    int      w, h;
    uint8_t *rgba;
};
SDL_Surface *fg_surface_new(int w, int h);
void         fg_surface_free(SDL_Surface *s);

/* ------------------------------------------------------------------ compositor */
struct FgOp {
    const SDL_Texture *tex;
    uint32_t texId;
    int16_t  x, y, w, h;
    uint8_t  alpha;
    uint8_t  r, g, b;
    /* derived once per frame (fg_op_prepare): no divisions in the compositor */
    int16_t  bx0, by0, bx1, by1;         /* screen box of the visible pixels */
    uint32_t stepX, stepY;               /* 16.16 source steps (scaled ops) */
};
void fg_op_prepare(FgOp &op);
/* precomputed: fg drawn opaque over bg at (dx, dy) -- fades become one constant blend */
void fg_premix_register(const SDL_Texture *fg, const SDL_Texture *bg, int dx, int dy);
void fg_premix_free();
void fg_prescale_prewarm(const SDL_Texture *t, int maxSize);
void fg_prescale_free();

struct FgRect { int x0, y0, x1, y1; };   /* x1/y1 exclusive */

struct SDL_Renderer {
    int       w, h;
    uint16_t *back;                      /* w*h RGB565, fast RAM */
    uint8_t  *dirty;                     /* tile map */
    int       tilesX, tilesY;
    bool      fullDirty;
    std::vector<FgOp>   prev, sortedPrev, sortedCur;
    std::vector<FgRect> rects;
    std::vector<int>    idx;
    uint16_t *target;                    /* where draw_op() paints (back or layer) */
    /* the static layer: the composite of ops[0 .. layerN) of the current frame,
       the part below the first op that changes (background, logo, board...) */
    uint16_t *layer;
    size_t    layerN;                    /* 0 = not valid */
    size_t    lastS;                     /* first changed op index of the last frame */
    int       stableS;                   /* frames that index stayed the same */
    std::vector<uint8_t> changed, changedPrev;   /* per op position: changed this / last frame */
    /* a big image fading in the static layer (the menu logo): brought to the
       current alpha one horizontal band per frame */
    struct {
        bool     active, pending, suppress;
        uint32_t texId;
        int16_t  x, y, w, h;
        uint8_t  bandAlpha[3];
        int      next;
    } fade;
    std::vector<FgRect> extraDirty;
};

SDL_Renderer *fg_renderer_new(int w, int h);
void          fg_renderer_free(SDL_Renderer *r);
/* composes the frame; afterwards r->rects holds the changed areas */
void          fg_renderer_compose(SDL_Renderer *r, const std::vector<FgOp> &ops);

/* ------------------------------------------------------------------ display */
struct Screen;
struct Window;

struct SDL_Window {
    struct Screen *screen;       /* own screen (fullscreen) or the locked pubscreen */
    struct Window *win;
    bool      fullscreen;
    int       offX, offY;        /* game area inside the window's rastport */
    int       pixfmt;            /* CGX pixel format of the target */
    int       method;            /* how present() converts */
    struct BitMap *fb;           /* friend bitmap (screen format), the last frame */
    int       fbBpp;
    uint32_t  lutHi[256], lutLo[256];    /* RGB565 hi/lo byte -> target pixel */
    uint32_t  argbHi[256], argbLo[256];  /* RGB565 hi/lo byte -> ARGB32 (fallback) */
    void     *conv;              /* conversion buffer of the fallback */
    uint16_t *blankPointer;      /* chip RAM */
    int       gameW, gameH;
};

/* returns NULL and an error text on failure */
SDL_Window *fg_display_open(int w, int h, bool fullscreen, const char *title, const char **err);
void        fg_display_close(SDL_Window *d);
void        fg_display_present(SDL_Window *d, const uint16_t *back, int pitchPixels,
                               const std::vector<FgRect> &rects);
uint32_t    fg_display_signal(SDL_Window *d);
void        fg_display_debug(SDL_Window *d);

/* ------------------------------------------------------------------ input */
enum FgEventType { FG_EV_NONE, FG_EV_QUIT, FG_EV_KEYDOWN, FG_EV_KEYUP, FG_EV_MOTION,
                   FG_EV_BUTTONDOWN, FG_EV_BUTTONUP, FG_EV_PADDOWN };
struct FgEvent {
    int type;
    SDL_Keycode key;
    int button;
    int x, y;
};
bool fg_display_poll(SDL_Window *d, FgEvent *ev);

/* joystick port 1 via lowlevel.library (optional) */
void fg_pad_open();
void fg_pad_close();
int  fg_pad_poll(int *buttons, int max);   /* newly pressed SDL_GamepadButton values */

/* ------------------------------------------------------------------ audio */
bool fg_audio_open(int mode, int ahiUnit);
void fg_audio_close();

/* ------------------------------------------------------------------ assets (assets.cpp) */
std::shared_ptr<SDL_Texture>  fg_gfx_get(const std::string &path);     /* cached .gfx */
const std::vector<uint8_t>   *fg_file_get(const std::string &path);    /* cached raw file */
void fg_preload(const std::string &base, void (*progress)(void *ctx, int done, int total), void *ctx);
void fg_assets_free();
void fg_audio_free_cache();

/* simple message box (Intuition EasyRequest) */
void fg_message(const char *title, const char *text);

#endif
