/* Software compositor of the AmigaOS port: RGB565 back buffer in fast RAM,
 * textures in RGB565 + 8-bit alpha, dirty tiles so that only the parts of
 * the 800x600 frame that changed are composed and sent to the display. */

#include "amiga_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <proto/exec.h>

/* ================================================================ textures */

static uint32_t sTextureId = 0;

SDL_Texture *fg_texture_new(int w, int h, bool withAlpha)
{
    SDL_Texture *t = (SDL_Texture *) malloc(sizeof(SDL_Texture));
    if (!t) return NULL;
    t->w = w;
    t->h = h;
    t->id = ++sTextureId;
    t->bx0 = t->by0 = 0;
    t->bx1 = (int16_t) w;
    t->by1 = (int16_t) h;
    t->runs = NULL;
    t->rowRun = NULL;
    t->mask = NULL;
    t->kind = withAlpha ? FG_TEX_ALPHA : FG_TEX_OPAQUE;
    t->rgb = (uint16_t *) malloc((size_t) w * h * 2 + 16);     /* (+16: AMMX reads past a row) */
    t->alpha = withAlpha ? (uint8_t *) malloc((size_t) w * h + 16) : NULL;  /* (+16: AMMX loads 8 bytes) */
    if (!t->rgb || (withAlpha && !t->alpha)) {
        SDL_DestroyTexture(t);
        return NULL;
    }
    return t;
}

extern "C" void SDL_DestroyTexture(SDL_Texture *t)
{
    if (!t) return;
    free(t->rgb);
    free(t->alpha);
    free(t->runs);
    free(t->rowRun);
    free(t->mask);
    free(t);
}

/* the bounding box of the visible pixels and the skip/blend/opaque runs per row */
void fg_texture_finish(SDL_Texture *t)
{
    if (!t || !t->alpha) return;
    const int w = t->w, h = t->h;
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    size_t nRuns = 0;
    for (int pass = 0; pass < 2; pass++) {
        size_t k = 0;
        for (int y = 0; y < h; y++) {
            const uint8_t *a = t->alpha + y * w;
            if (pass) t->rowRun[y] = (uint32_t) k;
            int x = 0;
            while (x < w) {
                int kind = a[x] == 0 ? 0 : (a[x] == 255 ? 2 : 1);
                int s = x;
                while (x < w && x - s < 16383 && (a[x] == 0 ? 0 : (a[x] == 255 ? 2 : 1)) == kind) x++;
                if (pass) t->runs[k] = (uint16_t) ((kind << 14) | (x - s));
                k++;
                if (!pass && kind) {
                    if (s < x0) x0 = s;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y + 1 > y1) y1 = y + 1;
                }
            }
        }
        if (!pass) {
            nRuns = k;
            t->runs = (uint16_t *) malloc(nRuns * 2 + 2);
            t->rowRun = (uint32_t *) malloc((h + 1) * 4);
            if (!t->runs || !t->rowRun) {
                free(t->runs); free(t->rowRun);
                t->runs = NULL; t->rowRun = NULL;
                break;
            }
        } else {
            t->rowRun[h] = (uint32_t) k;
        }
    }
    if (x1 <= x0 || y1 <= y0) { x0 = y0 = x1 = y1 = 0; }
    t->bx0 = (int16_t) x0; t->by0 = (int16_t) y0;
    t->bx1 = (int16_t) x1; t->by1 = (int16_t) y1;
}

static inline uint16_t rd16(const unsigned char *p) { return (uint16_t) ((p[0] << 8) | p[1]); }

/* FGX2 (tools/convert_assets.py): everything as it is used, nothing computed */
SDL_Texture *fg_texture_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        FG_LOG("[gfx] cannot open %s\n", path);
        return NULL;
    }
    unsigned char hd[20];
    SDL_Texture *t = NULL;
    bool ok = false;
    if (fread(hd, 1, 20, f) == 20 && memcmp(hd, "FGX2", 4) == 0) {
        int w = rd16(hd + 4), h = rd16(hd + 6), kind = hd[8];
        t = fg_texture_new(w, h, kind == FG_TEX_ALPHA);
        if (t) {
            t->kind = (uint8_t) kind;
            t->bx0 = (int16_t) rd16(hd + 10); t->by0 = (int16_t) rd16(hd + 12);
            t->bx1 = (int16_t) rd16(hd + 14); t->by1 = (int16_t) rd16(hd + 16);
            size_t n = (size_t) w * h;
            ok = fread(t->rgb, 2, n, f) == n;
            if (ok && kind == FG_TEX_MASK) {
                size_t mn = (size_t) ((w + 7) / 8) * h;
                t->mask = (uint8_t *) malloc(mn);
                ok = t->mask && fread(t->mask, 1, mn, f) == mn;
            } else if (ok && kind == FG_TEX_ALPHA) {
                ok = fread(t->alpha, 1, n, f) == n;
            }
            if (ok && kind != FG_TEX_OPAQUE) {
                unsigned char c[4];
                ok = fread(c, 1, 4, f) == 4;
                uint32_t nr = ok ? ((uint32_t) c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3]) : 0;
                t->rowRun = (uint32_t *) malloc((h + 1) * 4);
                t->runs = (uint16_t *) malloc(nr * 2 + 2);
                ok = ok && t->rowRun && t->runs &&
                     fread(t->rowRun, 4, h + 1, f) == (size_t) (h + 1) && fread(t->runs, 2, nr, f) == nr;
            }
        }
    }
    fclose(f);
    if (!ok) {
        SDL_DestroyTexture(t);
        t = NULL;
        FG_LOG("[gfx] bad file %s\n", path);
    }
    return t;
}

SDL_Texture *fg_texture_from_rgba(const uint8_t *rgba, int w, int h)
{
    bool alpha = false;
    for (int i = 0; i < w * h; i++)
        if (rgba[i * 4 + 3] != 255) { alpha = true; break; }
    SDL_Texture *t = fg_texture_new(w > 0 ? w : 1, h > 0 ? h : 1, true);
    if (!t) return NULL;
    if (w <= 0 || h <= 0) {
        t->rgb[0] = 0;
        t->alpha[0] = 0;
        return t;
    }
    for (int i = 0; i < w * h; i++) {
        const uint8_t *p = rgba + i * 4;
        t->rgb[i] = (uint16_t) (((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        t->alpha[i] = p[3];
    }
    if (!alpha) {
        free(t->alpha);
        t->alpha = NULL;
        t->kind = FG_TEX_OPAQUE;
    }
    fg_texture_finish(t);
    return t;
}

SDL_Surface *fg_surface_new(int w, int h)
{
    SDL_Surface *s = (SDL_Surface *) malloc(sizeof(SDL_Surface));
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    s->rgba = (uint8_t *) calloc((size_t) (w > 0 ? w : 1) * (h > 0 ? h : 1), 4);
    if (!s->rgba) { free(s); return NULL; }
    return s;
}

void fg_surface_free(SDL_Surface *s)
{
    if (!s) return;
    free(s->rgba);
    free(s);
}

/* ================================================================ blending */

#define TILE 16

/* d + (s - d) * a / 32 on all three fields at once */
static inline uint16_t blend565(uint32_t d, uint32_t s, uint32_t a5)
{
    d = (d | (d << 16)) & 0x07E0F81Fu;
    s = (s | (s << 16)) & 0x07E0F81Fu;
    d = (d + (((s - d) * a5) >> 5)) & 0x07E0F81Fu;
    return (uint16_t) (d | (d >> 16));
}

static inline uint16_t modulate565(uint32_t c, uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t cr = ((c >> 11) & 31) * r / 255;
    uint32_t cg = ((c >> 5) & 63) * g / 255;
    uint32_t cb = (c & 31) * b / 255;
    return (uint16_t) ((cr << 11) | (cg << 5) | cb);
}

static void c_row_blend(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n)
{
    while (n--) {
        uint32_t a = *alpha++;
        uint32_t s = *src++;
        if (a == 255) {
            *dst = (uint16_t) s;
        } else if (a) {
            *dst = blend565(*dst, s, (a + 4) >> 3);
        }
        dst++;
    }
}

static void c_row_blend_mod(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n, int mod)
{
    while (n--) {
        uint32_t a = alpha ? *alpha++ : 255;
        uint32_t s = *src++;
        a = (a * (uint32_t) mod + 255) >> 8;
        if (a) *dst = blend565(*dst, s, (a + 4) >> 3);
        dst++;
    }
}

/* ---- AMMX (68080 build): four pixels per pass, the same results as above */
int g_ammx = 0;

#ifdef FG_V080
extern "C" {
/* (blend080.s: n pixels, any n; the constants must be set: fg_ammx_setup) */
void fg_ammx_setup(void);
void fg_ammx_blend_row(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n);
void fg_ammx_blend_row_mod(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n, int mod);
void fg_ammx_blend_const(uint16_t *dst, const uint16_t *src, int n, int a8);
void fg_ammx_spans(uint16_t *drow, const uint16_t *gs, const uint8_t *ga, const int16_t *spans, const int16_t *spansEnd, int mod);
void fg_ammx_runs(uint16_t *d, const uint16_t *s, const uint8_t *a, const uint16_t *runs, const uint16_t *runsEnd,
                  int sx, int n, int mod);
void fg_ammx_register(void);
}
#endif

/* ---- 68k kernels (kern68k.s), both builds; each one enabled by the self-test */
extern "C" {
void fg_k_blend_row(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n);
void fg_k_blend_row_mod(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n, int mod);
void fg_k_copy_row(uint16_t *dst, const uint16_t *src, int n);
void fg_k_scaled_row(uint16_t *dst, const uint16_t *srow, const uint8_t *arow, uint32_t fx, uint32_t step, int n, int mod);
void fg_k_gather(uint16_t *gs, uint8_t *ga, const uint16_t *srow, const uint8_t *arow, uint32_t fx, uint32_t step, int n);
int16_t *fg_k_scaled_spans(const uint16_t *srow, const uint8_t *arow, const uint16_t *runs, const uint16_t *runsEnd,
                           uint32_t fx, uint32_t step, int n, uint16_t *gs, uint8_t *ga, int16_t *spans);
#ifdef FG_V080
void fg_ammx_copy_row(uint16_t *dst, const uint16_t *src, int n);
#endif
}
int g_kBlend = 0, g_kBlendMod = 0, g_kCopy = 0, g_kScaled = 0, g_kGather = 0, g_kConv = 0, g_kMix = 0, g_kSpans = 0;
extern "C" {
void fg_k_conv16(uint32_t *dst, const uint32_t *src, int pairs, const uint32_t *hi, const uint32_t *lo);
void fg_k_mix_add(int16_t *acc, const int8_t *src, int n, int vol);
void fg_k_mix_out(int8_t *out, const int16_t *acc, int n);
}

static void c_copy_row(uint16_t *d, const uint16_t *s, int n)
{
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static void c_scaled_row(uint16_t *d, const uint16_t *srow, const uint8_t *arow, uint32_t fx, uint32_t step, int n, int mod)
{
    for (int i = 0; i < n; i++, fx += step, d++) {
        uint32_t xs = fx >> 16;
        uint32_t al = arow ? arow[xs] : 255;
        if (mod != 255) al = (al * (uint32_t) mod + 255) >> 8;
        if (!al) continue;
        *d = (al == 255) ? srow[xs] : blend565(*d, srow[xs], (al + 4) >> 3);
    }
}

static void c_gather(uint16_t *gs, uint8_t *ga, const uint16_t *srow, const uint8_t *arow, uint32_t fx, uint32_t step, int n)
{
    for (int i = 0; i < n; i++, fx += step) {
        uint32_t xs = fx >> 16;
        gs[i] = srow[xs];
        ga[i] = arow ? arow[xs] : 255;
    }
}

/* one row of a scaled texture: walk the runs, gather the visible pixels,
 * record the stretches (offset, kind, count); as fg_k_scaled_spans */
static int16_t *c_scaled_spans(const uint16_t *srow, const uint8_t *arow, const uint16_t *runs, const uint16_t *runsEnd,
                               uint32_t fx, uint32_t step, int n, uint16_t *gs, uint8_t *ga, int16_t *spans)
{
    uint32_t lim = 0;
    int kind = 0, open = -1, left = n;
    while (left > 0) {
        if (fx >= lim) {
            if (runs >= runsEnd) break;
            if (open >= 0) { *spans++ = (int16_t) open; open = -1; }
            lim += (uint32_t) (*runs & 0x3FFF) << 16;
            kind = *runs >> 14;
            runs++;
            continue;
        }
        if (kind == 0) {
            do { fx += step; left--; } while (left > 0 && fx < lim);
            continue;
        }
        if (open < 0) { *spans++ = (int16_t) (n - left); *spans++ = (int16_t) kind; open = 0; }
        uint32_t xs = fx >> 16;
        *gs++ = srow[xs];
        *ga++ = arow ? arow[xs] : 255;
        open++;
        fx += step;
        left--;
    }
    if (open >= 0) *spans++ = (int16_t) open;
    return spans;
}

static inline int16_t *scaled_spans(const uint16_t *srow, const uint8_t *arow, const uint16_t *runs, const uint16_t *runsEnd,
                                    uint32_t fx, uint32_t step, int n, uint16_t *gs, uint8_t *ga, int16_t *spans)
{
    if (g_kSpans) return fg_k_scaled_spans(srow, arow, runs, runsEnd, fx, step, n, gs, ga, spans);
    return c_scaled_spans(srow, arow, runs, runsEnd, fx, step, n, gs, ga, spans);
}

static inline void row_copy(uint16_t *d, const uint16_t *s, int n);
static void fg_row_blend(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n);
static void fg_row_blend_mod(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n, int mod);

/* one unscaled texture row, pixels sx .. sx+n-1 (d, s, a at sx; a NULL: hard
 * mask): skip / copy / blend per run -- one AMMX call on the 080 */
static void c_runs_row(uint16_t *d, const uint16_t *s, const uint8_t *a, const uint16_t *run, const uint16_t *end,
                       int sx, int n, int mod)
{
    int pos = 0, from = sx, to = sx + n;
    for (; run < end && pos < to; run++) {
        int len = *run & 0x3FFF, kind = *run >> 14;
        int s0 = pos > from ? pos : from, s1 = pos + len < to ? pos + len : to;
        pos += len;
        if (s0 >= s1 || kind == 0) continue;
        int o = s0 - sx;
        if (mod != 255)      fg_row_blend_mod(d + o, s + o, a ? a + o : NULL, s1 - s0, mod);
        else if (kind == 2)  row_copy(d + o, s + o, s1 - s0);
        else                 fg_row_blend(d + o, s + o, a + o, s1 - s0);
    }
}

static inline void runs_row(uint16_t *d, const uint16_t *s, const uint8_t *a, const uint16_t *run, const uint16_t *end,
                            int sx, int n, int mod)
{
#ifdef FG_V080
    if (g_ammx) { fg_ammx_runs(d, s, a, run, end, sx, n, mod); return; }
#endif
    c_runs_row(d, s, a, run, end, sx, n, mod);
}

/* the gathered pixels of a scaled row: per span a copy or a blend */
static void c_spans_apply(uint16_t *drow, const uint16_t *gs, const uint8_t *ga, const int16_t *sp, const int16_t *end, int mod)
{
    int pos = 0;
    for (; sp < end; sp += 3) {
        uint16_t *d = drow + sp[0];
        int kind = sp[1], cnt = sp[2];
        if (kind == 2 && mod == 255) row_copy(d, gs + pos, cnt);
        else if (mod != 255) fg_row_blend_mod(d, gs + pos, ga + pos, cnt, mod);
        else fg_row_blend(d, gs + pos, ga + pos, cnt);
        pos += cnt;
    }
}

static inline void spans_apply(uint16_t *drow, const uint16_t *gs, const uint8_t *ga, const int16_t *sp, const int16_t *end, int mod)
{
#ifdef FG_V080
    if (g_ammx) { fg_ammx_spans(drow, gs, ga, sp, end, mod); return; }
#endif
    c_spans_apply(drow, gs, ga, sp, end, mod);
}

/* the copy of a row: 64-bit AMMX on the 080, longwords on the 060 */
static inline void row_copy(uint16_t *d, const uint16_t *s, int n)
{
#ifdef FG_V080
    if (g_ammx) { fg_ammx_copy_row(d, s, n); return; }
#endif
    if (g_kCopy) fg_k_copy_row(d, s, n);
    else memcpy(d, s, n * 2);
}

static inline void scaled_row(uint16_t *d, const uint16_t *srow, const uint8_t *arow, uint32_t fx, uint32_t step, int n, int mod)
{
    if (g_kScaled) fg_k_scaled_row(d, srow, arow, fx, step, n, mod);
    else c_scaled_row(d, srow, arow, fx, step, n, mod);
}

static inline void gather(uint16_t *gs, uint8_t *ga, const uint16_t *srow, const uint8_t *arow, uint32_t fx, uint32_t step, int n)
{
    if (g_kGather) fg_k_gather(gs, ga, srow, arow, fx, step, n);
    else c_gather(gs, ga, srow, arow, fx, step, n);
}

static void fg_row_blend(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n);
static void fg_row_blend_mod(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n, int mod);

/* Checks every 68k kernel against its C reference on random data (all
 * alignments, alpha 0 / 255 / partial, faded and not); a kernel that differs
 * stays off. NOASM: all off (A/B measurements). */
static uint32_t sSeed = 4711;
static inline uint32_t rnd() { sSeed = sSeed * 1103515245u + 12345u; return sSeed >> 8; }

void fg_kernels_init()
{
    if (g_fgConfig.noAsm) { fg_info("[asm] off (NOASM): C loops\n"); return; }
    static uint16_t src[600 + 16], d0[600 + 16], d1[600 + 16];
    static uint8_t al[600 + 16], ga0[600 + 16], ga1[600 + 16];
    int ok[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    for (int round = 0; round < 40; round++) {
        for (int i = 0; i < 616; i++) {
            src[i] = (uint16_t) rnd(); d0[i] = d1[i] = (uint16_t) rnd();
            uint32_t v = rnd() & 255;
            al[i] = (uint8_t) ((i % 5) == 0 ? 0 : (i % 5) == 1 ? 255 : v);
        }
        int off = round & 3, n = 1 + (int) (rnd() % 300), mod = (round % 3) ? (int) (rnd() & 255) : 255;
        /* blend */
        c_row_blend(d0 + off, src + off, al + off, n);
        fg_k_blend_row(d1 + off, src + off, al + off, n);
        if (memcmp(d0, d1, sizeof(d0))) ok[0] = 0;
        /* faded blend, with and without alpha */
        memcpy(d1, d0, sizeof(d0));
        c_row_blend_mod(d0 + off, src + off, (round & 4) ? NULL : al + off, n, mod);
        fg_k_blend_row_mod(d1 + off, src + off, (round & 4) ? NULL : al + off, n, mod);
        if (memcmp(d0, d1, sizeof(d0))) ok[1] = 0;
        /* copy */
        memcpy(d1, d0, sizeof(d0));
        c_copy_row(d0 + off, src + (round & 7), n);
        fg_k_copy_row(d1 + off, src + (round & 7), n);
        if (memcmp(d0, d1, sizeof(d0))) ok[2] = 0;
        /* scaled row, gather (a 108-pixel texture row drawn 1..300 wide) */
        memcpy(d1, d0, sizeof(d0));
        int w = 1 + (int) (rnd() % 300);
        uint32_t step = (108u << 16) / (uint32_t) w, fx = (rnd() % 8) * step;
        int cnt = w - (int) (fx / step);
        c_scaled_row(d0 + off, src, (round & 2) ? NULL : al, fx, step, cnt, mod);
        fg_k_scaled_row(d1 + off, src, (round & 2) ? NULL : al, fx, step, cnt, mod);
        if (memcmp(d0, d1, sizeof(d0))) ok[3] = 0;
        memset(ga0, 0, sizeof(ga0)); memset(ga1, 0, sizeof(ga1));
        memcpy(d1, d0, sizeof(d0));
        c_gather(d0, ga0, src, (round & 2) ? NULL : al, fx, step, cnt);
        fg_k_gather(d1, ga1, src, (round & 2) ? NULL : al, fx, step, cnt);
        if (memcmp(d0, d1, sizeof(d0)) || memcmp(ga0, ga1, sizeof(ga0))) ok[4] = 0;
    }
    /* scaled spans: the partc textures' rows (hard and soft), random scale and start */
    {
        static uint16_t g0[1100], g1[1100];
        static uint8_t a0[1100], a1[1100];
        static int16_t s0[3 * 1100], s1[3 * 1100];
        static uint8_t alpha[108 * 4];
        static uint16_t runs[108 * 4 + 8];
        static uint32_t rowRun[5];
        static uint16_t px[108 * 4];
        /* four rows: soft edge, hard, all transparent, all opaque */
        for (int x = 0; x < 108; x++) {
            int d = x < 54 ? x : 107 - x;
            alpha[x] = (uint8_t) (d < 20 ? 0 : d < 40 ? d * 6 : 255);
            alpha[108 + x] = (uint8_t) ((x / 9) & 1 ? 255 : 0);
            alpha[216 + x] = 0;
            alpha[324 + x] = 255;
        }
        for (int i = 0; i < 432; i++) px[i] = (uint16_t) rnd();
        size_t k = 0;
        for (int y = 0; y < 4; y++) {
            rowRun[y] = (uint32_t) k;
            int x = 0;
            while (x < 108) {
                int kd = alpha[y * 108 + x] == 0 ? 0 : (alpha[y * 108 + x] == 255 ? 2 : 1), st = x;
                while (x < 108 && (alpha[y * 108 + x] == 0 ? 0 : (alpha[y * 108 + x] == 255 ? 2 : 1)) == kd) x++;
                runs[k++] = (uint16_t) ((kd << 14) | (x - st));
            }
        }
        rowRun[4] = (uint32_t) k;
        for (int round = 0; round < 200 && ok[7]; round++) {
            int y = round & 3, w = 1 + (int) (rnd() % 400);
            uint32_t step = (108u << 16) / (uint32_t) w;
            int rx = (int) (rnd() % (uint32_t) w);
            uint32_t fx = (uint32_t) rx * step;
            const uint8_t *ar = (round & 4) ? NULL : alpha + y * 108;
            int16_t *e0 = c_scaled_spans(px + y * 108, ar, runs + rowRun[y], runs + rowRun[y + 1], fx, step, w - rx, g0, a0, s0);
            int16_t *e1 = fg_k_scaled_spans(px + y * 108, ar, runs + rowRun[y], runs + rowRun[y + 1], fx, step, w - rx, g1, a1, s1);
            if (e0 - s0 != e1 - s1 || memcmp(s0, s1, (e0 - s0) * 2)) { ok[7] = 0; break; }
            int total = 0;
            for (int16_t *p = s0; p < e0; p += 3) total += p[2];
            if (memcmp(g0, g1, total * 2) || memcmp(a0, a1, total)) ok[7] = 0;
        }
    }
    /* screen conversion: random tables, two pixels per longword */
    {
        static uint32_t hi[256], lo[256], s32[64], o0[64], o1[64];
        for (int i = 0; i < 256; i++) { hi[i] = rnd() & 0xFFFF; lo[i] = rnd() & 0xFFFF; }
        for (int i = 0; i < 64; i++) s32[i] = (rnd() << 16) ^ rnd();
        for (int i = 0; i < 64; i++) {
            uint32_t v = s32[i], p0 = v >> 16, p1 = v & 0xFFFF;
            o0[i] = ((hi[p0 >> 8] | lo[p0 & 255]) << 16) | hi[p1 >> 8] | lo[p1 & 255];
        }
        fg_k_conv16(o1, s32, 64, hi, lo);
        if (memcmp(o0, o1, sizeof(o0))) ok[5] = 0;
    }
    /* audio: add voices at several volumes, clamp */
    {
        static int16_t a0[512], a1[512];
        static int8_t smp[512], out0[512], out1[512];
        for (int i = 0; i < 512; i++) { smp[i] = (int8_t) rnd(); a0[i] = a1[i] = (int16_t) ((int) (rnd() & 511) - 256); }
        static const int vols[] = { 256, 77, 64, 205, 1, 255 };
        for (unsigned v = 0; v < 6; v++) {
            for (int i = 0; i < 512; i++) a0[i] += (vols[v] == 256) ? smp[i] : (int16_t) ((smp[i] * vols[v]) >> 8);
            fg_k_mix_add(a1, smp, 512, vols[v]);
        }
        for (int i = 0; i < 512; i++) { int x = a0[i]; out0[i] = (int8_t) (x > 127 ? 127 : (x < -128 ? -128 : x)); }
        fg_k_mix_out(out1, a1, 512);
        if (memcmp(a0, a1, sizeof(a0)) || memcmp(out0, out1, sizeof(out0))) ok[6] = 0;
    }
    static const char *names[8] = { "blend", "fade", "copy", "scaled", "gather", "screen", "audio", "spans" };
    char line[200] = "[asm] 68k kernels:";
    for (int i = 0; i < 8; i++) {
        strcat(line, " ");
        strcat(line, names[i]);
        strcat(line, ok[i] ? " ok" : " FAILED");
    }
    strcat(line, "\n");
    fg_info("%s", line);
    g_kBlend = ok[0]; g_kBlendMod = ok[1]; g_kCopy = ok[2]; g_kScaled = ok[3]; g_kGather = ok[4];
    g_kConv = ok[5]; g_kMix = ok[6]; g_kSpans = ok[7];
}

/* Detects the 68080, registers the task for AMMX and checks every kernel
 * against the C path (random pixels, all byte offsets). Any difference:
 * AMMX stays off. */
void fg_ammx_init()
{
#ifdef FG_V080
    g_ammx = 0;
    if (g_fgConfig.noAmmx) { fg_info("[ammx] off (NOAMMX)\n"); return; }
    UWORD attn = SysBase->AttnFlags;
    if (!(attn & (1u << 10))) { fg_info("[ammx] no 68080 (AttnFlags %04lx), C path\n", (unsigned long) attn); return; }
    struct Library *vr = (struct Library *) OpenResource((CONST_STRPTR) "vampire.resource");
    struct Resident *vs = FindResident((CONST_STRPTR) "VampireSupport");
    if (!vr || vr->lib_Version < 45 || !vs) {
        fg_info("[ammx] task cannot be registered (vampire.resource %ld, VampireSupport %s), C path\n",
                (long) (vr ? vr->lib_Version : 0), vs ? "yes" : "no");
        return;
    }
    fg_info("[ammx] 68080, vampire.resource %ld.%ld: registering the task\n", (long) vr->lib_Version, (long) vr->lib_Revision);
    fg_ammx_register();
    fg_info("[ammx] registered, self-test\n");
    fg_ammx_setup();

    static uint16_t src[160 + 16], d0[160 + 16], d1[160 + 16];
    static uint8_t al[160 + 16];
    uint32_t seed = 12345;
    int bad = 0;
    /* blend rows: every length 1..69, all four 2-byte offsets, the three modes */
    for (int round = 0; round < 3 * 69 * 2 && !bad; round++) {
        for (int i = 0; i < 176; i++) {
            seed = seed * 1103515245u + 12345u; src[i] = (uint16_t) (seed >> 8);
            seed = seed * 1103515245u + 12345u; d0[i] = d1[i] = (uint16_t) (seed >> 8);
            seed = seed * 1103515245u + 12345u;
            uint32_t v = (seed >> 16) & 255;
            al[i] = (uint8_t) ((i % 7) == 0 ? 0 : (i % 7) == 1 ? 255 : v);
        }
        int n = 1 + (round / 6) % 69, off = round & 3, mode = (round / 2) % 3;
        int mod = (int) ((seed >> 12) & 255);
        if (mode == 0) {
            c_row_blend(d0 + off, src + off, al + off, n);
            g_ammx = 1; fg_row_blend(d1 + off, src + off, al + off, n); g_ammx = 0;
        } else if (mode == 1) {
            c_row_blend_mod(d0 + off, src + off, al + off, n, mod);
            g_ammx = 1; fg_row_blend_mod(d1 + off, src + off, al + off, n, mod); g_ammx = 0;
        } else {
            c_row_blend_mod(d0 + off, src + off, NULL, n, mod);
            g_ammx = 1; fg_row_blend_mod(d1 + off, src + off, NULL, n, mod); g_ammx = 0;
        }
        for (int i = 0; i < 176; i++)
            if (d0[i] != d1[i]) {
                fg_info("[ammx] SELF-TEST FAILED: blend mode %ld n %ld offset %ld pixel %ld: C %04lx AMMX %04lx\n",
                        (long) mode, (long) n, (long) off, (long) i, (unsigned long) d0[i], (unsigned long) d1[i]);
                bad = 1;
                break;
            }
    }
    /* whole rows: runs (unscaled) and spans (scaled), soft and hard alpha, faded or not */
    if (!bad) {
        static uint8_t ra[120 + 16];
        static uint16_t runs[130], rs[120 + 16];
        static int16_t sp0[3 * 130], sp1[3 * 130];
        static uint16_t g0[200 + 16];
        static uint8_t ga0[200 + 16];
        for (int round = 0; round < 120 && !bad; round++) {
            bool hard = round & 1;
            for (int x = 0; x < 120; x++) {
                seed = seed * 1103515245u + 12345u;
                int band = (x / (3 + round % 5)) % 3;
                ra[x] = (uint8_t) (band == 0 ? 0 : band == 2 ? 255 : (hard ? 255 : 1 + (seed >> 8) % 254));
                rs[x] = (uint16_t) (seed >> 12);
            }
            int k = 0, x = 0;
            while (x < 120) {
                int kd = ra[x] == 0 ? 0 : ra[x] == 255 ? 2 : 1, st = x;
                while (x < 120 && (ra[x] == 0 ? 0 : ra[x] == 255 ? 2 : 1) == kd) x++;
                runs[k++] = (uint16_t) ((kd << 14) | (x - st));
            }
            int mod = (round % 3) ? 255 : (int) ((seed >> 4) & 255);
            int sx = (int) (seed % 60), n = 1 + (int) ((seed >> 8) % (uint32_t) (120 - sx));
            for (int i = 0; i < 176; i++) { seed = seed * 1103515245u + 12345u; d0[i] = d1[i] = (uint16_t) (seed >> 8); }
            const uint8_t *aa = hard ? NULL : ra + sx;
            c_runs_row(d0 + 3, rs + sx, aa, runs, runs + k, sx, n, mod);
            g_ammx = 1; fg_ammx_runs(d1 + 3, rs + sx, aa, runs, runs + k, sx, n, mod); g_ammx = 0;
            if (memcmp(d0, d1, sizeof(d0))) {
                fg_info("[ammx] SELF-TEST FAILED: runs row (round %ld)\n", (long) round);
                bad = 1;
                break;
            }
            /* spans: a 120-pixel row drawn 37..160 wide */
            int w = 37 + (int) (seed % 124);
            uint32_t step = (120u << 16) / (uint32_t) w;
            int16_t *e = c_scaled_spans(rs, hard ? NULL : ra, runs, runs + k, 0, step, w, g0, ga0, sp0);
            for (int i = 0; i < 176; i++) { seed = seed * 1103515245u + 12345u; d0[i] = d1[i] = (uint16_t) (seed >> 8); }
            memcpy(sp1, sp0, sizeof(sp0));
            c_spans_apply(d0 + 2, g0, ga0, sp0, e, mod);
            g_ammx = 1; fg_ammx_spans(d1 + 2, g0, ga0, sp1, sp1 + (e - sp0), mod); g_ammx = 0;
            if (memcmp(d0, d1, sizeof(d0))) {
                fg_info("[ammx] SELF-TEST FAILED: spans row (round %ld)\n", (long) round);
                bad = 1;
                break;
            }
        }
    }
    g_ammx = !bad;
    fg_info(bad ? "[ammx] off, C path\n" : "[ammx] self-test passed: AMMX blending on\n");

    if (g_fgConfig.frameStats && !bad) {
        /* kernel throughput: 32 rows of 2048 pixels, per-pixel alpha with and without a fade */
        const int W = 2048, H = 32;
        uint16_t *bd = (uint16_t *) malloc(W * H * 2 + 16), *bs = (uint16_t *) malloc(W * H * 2 + 16);
        uint8_t *ba = (uint8_t *) malloc(W * H + 16);
        if (bd && bs && ba) {
            for (int i = 0; i < W * H; i++) { bd[i] = (uint16_t) (i * 7); bs[i] = (uint16_t) (i * 13); ba[i] = (uint8_t) (i * 5 + 1); }
            int kb = g_kBlend, km = g_kBlendMod;
            for (int pass = 0; pass < 6; pass++) {
                int way = pass % 3;                     /* 0 C, 1 68k asm, 2 AMMX */
                bool fade = pass >= 3;
                g_ammx = way == 2;
                g_kBlend = way == 1 ? kb : 0;
                g_kBlendMod = way == 1 ? km : 0;
                uint32_t t0 = fg_micros();
                for (int rep = 0; rep < 4; rep++)
                    for (int y = 0; y < H; y++) {
                        if (fade) fg_row_blend_mod(bd + y * W, bs + y * W, ba + y * W, W, 170);
                        else fg_row_blend(bd + y * W, bs + y * W, ba + y * W, W);
                    }
                uint32_t us = fg_micros() - t0;
                fg_info("[ammx] bench %s %s: %ld ns per pixel\n", way == 2 ? "AMMX" : way == 1 ? "asm " : "C   ",
                        fade ? "fade " : "alpha", (long) (us * 1000 / (4u * W * H)));
            }
            g_ammx = 1;
            g_kBlend = kb;
            g_kBlendMod = km;
        }
        free(bd); free(bs); free(ba);
    }
#endif
}

static void fg_row_blend(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n)
{
#ifdef FG_V080
    if (g_ammx) { if (n > 0) fg_ammx_blend_row(dst, src, alpha, n); return; }
#endif
    if (g_kBlend) fg_k_blend_row(dst, src, alpha, n);
    else c_row_blend(dst, src, alpha, n);
}

static void fg_row_blend_mod(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n, int mod)
{
#ifdef FG_V080
    if (g_ammx) {
        if (n <= 0) return;
        if (alpha) fg_ammx_blend_row_mod(dst, src, alpha, n, mod);
        else {
            uint32_t a = (255u * (uint32_t) mod + 255) >> 8;
            fg_ammx_blend_const(dst, src, n, (int) ((a + 4) & ~7u));
        }
        return;
    }
#endif
    if (g_kBlendMod) fg_k_blend_row_mod(dst, src, alpha, n, mod);
    else c_row_blend_mod(dst, src, alpha, n, mod);
}

/* tinted copies (color modulation) of textures: few combinations in the game
 * (particle colors, the selected / hint selector), made once */
static std::map<uint64_t, SDL_Texture *> sTinted;

static const SDL_Texture *tinted_texture(const SDL_Texture *t, uint8_t r, uint8_t g, uint8_t b)
{
    uint64_t key = ((uint64_t) t->id << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | b;
    std::map<uint64_t, SDL_Texture *>::iterator it = sTinted.find(key);
    if (it != sTinted.end()) return it->second;
    SDL_Texture *n = fg_texture_new(t->w, t->h, t->kind == FG_TEX_ALPHA);
    if (!n) return t;
    uint8_t rt[32], gt[64], bt[32];
    for (int i = 0; i < 32; i++) { rt[i] = (uint8_t) (i * r / 255); bt[i] = (uint8_t) (i * b / 255); }
    for (int i = 0; i < 64; i++) gt[i] = (uint8_t) (i * g / 255);
    for (int i = 0; i < t->w * t->h; i++) {
        uint32_t c = t->rgb[i];
        n->rgb[i] = (uint16_t) (((uint32_t) rt[c >> 11] << 11) | ((uint32_t) gt[(c >> 5) & 63] << 5) | bt[c & 31]);
    }
    n->kind = t->kind;
    n->bx0 = t->bx0; n->by0 = t->by0; n->bx1 = t->bx1; n->by1 = t->by1;
    if (t->alpha) memcpy(n->alpha, t->alpha, (size_t) t->w * t->h);
    if (t->runs) {      /* (the same shape: the same runs) */
        size_t nr = t->rowRun[t->h];
        n->rowRun = (uint32_t *) malloc((t->h + 1) * 4);
        n->runs = (uint16_t *) malloc(nr * 2 + 2);
        if (n->rowRun && n->runs) {
            memcpy(n->rowRun, t->rowRun, (t->h + 1) * 4);
            memcpy(n->runs, t->runs, nr * 2);
        } else {
            free(n->rowRun); free(n->runs);
            n->rowRun = NULL; n->runs = NULL;
        }
    }
    sTinted[key] = n;
    return n;
}

void fg_tint_cache_free()
{
    for (std::map<uint64_t, SDL_Texture *>::iterator it = sTinted.begin(); it != sTinted.end(); ++it)
        SDL_DestroyTexture(it->second);
    sTinted.clear();
}


/* the screen rectangle of the op's visible pixels (texture bounding box, scaled) */
uint32_t g_pxScaledWalk = 0, g_pxScaledVis = 0, g_pxRuns = 0, g_scaledRows = 0;   /* STATS: pixel counts */

/* STATS on the 68080: cycle counter sections (fg_ccc, blend080.s) */
enum { CY_SPANS, CY_APPLY, CY_SCALED, CY_RUNS, CY_COPY, CY_RECTS, CY_LAYER, CY_DIFF, CY_N };
uint32_t g_cyc[CY_N], g_nSpans = 0;
int g_cycOn = 0;
#ifdef FG_V080
extern "C" uint32_t fg_ccc(void);
#define CYC() (g_cycOn ? fg_ccc() : 0)
#else
#define CYC() 0
#endif

/* ---- pre-scaled textures: a scaled op is drawn from a texture already at
 * its size, sampled with exactly the scaled path's formula (16.16 steps
 * (tw << 16) / w, (th << 16) / h) -- bit-identical, but drawn as an unscaled
 * sprite: one blend call per row, no walking and no sampling per frame. The
 * particle textures are made for every size at start; others on first use,
 * within a memory budget. */
static std::map<uint64_t, SDL_Texture *> sScaled;
static size_t sScaledBytes = 0;
static const size_t SCALED_BUDGET = 24u << 20;
static inline void gather(uint16_t *gs, uint8_t *ga, const uint16_t *srow, const uint8_t *arow, uint32_t fx, uint32_t step, int n);

static SDL_Texture *prescale_make(const SDL_Texture *t, int w, int h)
{
    SDL_Texture *n = fg_texture_new(w, h, true);
    if (!n) return NULL;
    const uint32_t stepX = ((uint32_t) t->w << 16) / (uint32_t) w;
    const uint32_t stepY = ((uint32_t) t->h << 16) / (uint32_t) h;
    for (int y = 0; y < h; y++) {
        int sy = (int) (((uint32_t) y * stepY) >> 16);
        gather(n->rgb + y * w, n->alpha + y * w, t->rgb + sy * t->w, t->alpha + sy * t->w, 0, stepX, w);
    }
    n->kind = FG_TEX_ALPHA;
    fg_texture_finish(n);
    return n;
}

static const SDL_Texture *prescaled(const SDL_Texture *t, int w, int h)
{
    if (!t->alpha || w <= 0 || h <= 0 || w > 1024) return NULL;
    uint64_t key = ((uint64_t) t->id << 32) | ((uint32_t) w << 16) | (uint32_t) h;
    std::map<uint64_t, SDL_Texture *>::iterator it = sScaled.find(key);
    if (it != sScaled.end()) return it->second;
    size_t bytes = (size_t) w * h * 3;
    if (sScaledBytes + bytes > SCALED_BUDGET) return NULL;      /* (then the scaled path) */
    SDL_Texture *n = prescale_make(t, w, h);
    if (!n) return NULL;
    sScaled[key] = n;
    sScaledBytes += bytes;
    return n;
}

void fg_prescale_prewarm(const SDL_Texture *t, int maxSize)
{
    for (int s = 1; s <= maxSize; s++) prescaled(t, s, s);
}

void fg_prescale_free()
{
    for (std::map<uint64_t, SDL_Texture *>::iterator it = sScaled.begin(); it != sScaled.end(); ++it)
        SDL_DestroyTexture(it->second);
    sScaled.clear();
    sScaledBytes = 0;
}

/* ---- precomputed fades: F = fg drawn at full alpha over bg at (dx, dy).
 * Drawing fg at alpha a over exactly that bg is then one constant blend of
 * the target (holding bg) towards F -- no per-pixel alpha, no runs. Only used
 * where the target below fg is that bg (the menu logo directly on the menu
 * background); the result differs from the per-pixel blend by at most one
 * 5-bit step. */
struct FgPremix { const SDL_Texture *fg, *bg; int dx, dy; uint16_t *F; };
static std::vector<FgPremix> sPremix;

static void c_row_blend(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n);
static void fg_row_blend(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n);
static void fg_row_blend_mod(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n, int mod);
static inline void row_copy(uint16_t *d, const uint16_t *s, int n);

void fg_premix_register(const SDL_Texture *fg, const SDL_Texture *bg, int dx, int dy)
{
    if (!fg || !bg || !fg->alpha || dx < 0 || dy < 0 || dx + fg->w > bg->w || dy + fg->h > bg->h) return;
    uint16_t *F = (uint16_t *) malloc((size_t) fg->w * fg->h * 2 + 16);
    if (!F) return;
#ifdef FG_V080
    if (g_ammx) fg_ammx_setup();
#endif
    for (int y = 0; y < fg->h; y++) {
        uint16_t *row = F + y * fg->w;
        memcpy(row, bg->rgb + (dy + y) * bg->w + dx, fg->w * 2);
        fg_row_blend(row, fg->rgb + y * fg->w, fg->alpha + y * fg->w, fg->w);
    }
    FgPremix p = { fg, bg, dx, dy, F };
    sPremix.push_back(p);
}

void fg_premix_free()
{
    for (size_t i = 0; i < sPremix.size(); i++) free(sPremix[i].F);
    sPremix.clear();
}

static inline const FgPremix *premix_for(const FgOp &op)
{
    for (size_t i = 0; i < sPremix.size(); i++) {
        const FgPremix &p = sPremix[i];
        if (p.fg == op.tex && op.x == p.dx && op.y == p.dy && op.w == p.fg->w && op.h == p.fg->h &&
            (op.r & op.g & op.b) == 255)
            return &p;
    }
    return NULL;
}

/* computes the derived fields of an op, once per frame (go_window.cpp) */
void fg_op_prepare(FgOp &op)
{
    const SDL_Texture *t = op.tex;
    if (op.w == t->w && op.h == t->h) {
        op.bx0 = (int16_t) (op.x + t->bx0); op.by0 = (int16_t) (op.y + t->by0);
        op.bx1 = (int16_t) (op.x + t->bx1); op.by1 = (int16_t) (op.y + t->by1);
        op.stepX = op.stepY = 1u << 16;
        return;
    }
    /* (a full-width box needs no division: (w*W + w-1)/w = W) */
    op.bx0 = (int16_t) (op.x + (t->bx0 ? t->bx0 * op.w / t->w : 0));
    op.by0 = (int16_t) (op.y + (t->by0 ? t->by0 * op.h / t->h : 0));
    op.bx1 = (int16_t) (op.x + (t->bx1 == t->w ? op.w : (t->bx1 * op.w + t->w - 1) / t->w));
    op.by1 = (int16_t) (op.y + (t->by1 == t->h ? op.h : (t->by1 * op.h + t->h - 1) / t->h));
    op.stepX = ((uint32_t) t->w << 16) / (uint32_t) op.w;
    op.stepY = ((uint32_t) t->h << 16) / (uint32_t) op.h;
}

static inline FgRect op_box(const FgOp &op)
{
    FgRect b = { op.bx0, op.by0, op.bx1, op.by1 };
    return b;
}

static inline FgRect op_box_calc(const FgOp &op)
{
    const SDL_Texture *t = op.tex;
    FgRect b;
    if (op.w == t->w && op.h == t->h) {
        b.x0 = op.x + t->bx0; b.y0 = op.y + t->by0;
        b.x1 = op.x + t->bx1; b.y1 = op.y + t->by1;
    } else {
        b.x0 = op.x + t->bx0 * op.w / t->w;
        b.y0 = op.y + t->by0 * op.h / t->h;
        b.x1 = op.x + (t->bx1 * op.w + t->w - 1) / t->w;
        b.y1 = op.y + (t->by1 * op.h + t->h - 1) / t->h;
    }
    return b;
}

/* draws op clipped to the rectangle c */
static void draw_op(SDL_Renderer *r, const FgOp &op, const FgRect &c)
{
    if (!sPremix.empty()) {
        const FgPremix *pm = premix_for(op);
        if (pm) {
            /* the target holds the background here: one constant blend towards F */
            int x0 = std::max<int>(op.x, c.x0), y0 = std::max<int>(op.y, c.y0);
            int x1 = std::min<int>(op.x + op.w, c.x1), y1 = std::min<int>(op.y + op.h, c.y1);
            if (x0 >= x1 || y0 >= y1 || op.alpha == 0) return;
            for (int y = y0; y < y1; y++) {
                uint16_t *d = r->target + y * r->w + x0;
                const uint16_t *f = pm->F + (y - op.y) * op.w + (x0 - op.x);
                if (op.alpha == 255) row_copy(d, f, x1 - x0);
                else fg_row_blend_mod(d, f, NULL, x1 - x0, op.alpha);
            }
            return;
        }
    }
    const SDL_Texture *t = op.tex;
    if ((op.r & op.g & op.b) != 255) t = tinted_texture(t, op.r, op.g, op.b);
    const bool tinted = false;
    bool scaled = op.w != t->w || op.h != t->h;
    if (scaled && g_fgConfig.noPrescale == 0) {
        const SDL_Texture *ps = prescaled(t, op.w, op.h);
        if (ps) { t = ps; scaled = false; }            /* (the same pixels, drawn unscaled) */
    }
    FgRect b = op_box(op);                       /* (only the visible part of the texture) */
    int x0 = std::max<int>(b.x0, c.x0), y0 = std::max<int>(b.y0, c.y0);
    int x1 = std::min<int>(b.x1, c.x1), y1 = std::min<int>(b.y1, c.y1);
    if (x0 >= x1 || y0 >= y1 || op.alpha == 0) return;

    const int pitch = r->w;
    const int n = x1 - x0;

    if (!scaled && !tinted) {
        int sx = x0 - op.x;
        for (int y = y0; y < y1; y++) {
            int sy = y - op.y;
            uint16_t *d = r->target + y * pitch + x0;
            const uint16_t *s = t->rgb + sy * t->w + sx;
            if (t->kind == FG_TEX_OPAQUE) {
                if (op.alpha == 255) row_copy(d, s, n);
                else fg_row_blend_mod(d, s, NULL, n, op.alpha);
                continue;
            }
            /* (hard masks have no alpha bytes: their runs are only skip / copy) */
            const uint8_t *a = t->alpha ? t->alpha + sy * t->w + sx : NULL;
            if (!t->runs) {
                if (op.alpha == 255) fg_row_blend(d, s, a, n);
                else fg_row_blend_mod(d, s, a, n, op.alpha);
                continue;
            }
            /* the row's runs inside [sx, sx + n): skip / blend / copy */
            uint32_t c0 = CYC();
            runs_row(d, s, a, t->runs + t->rowRun[sy], t->runs + t->rowRun[sy + 1], sx, n, op.alpha);
            if (g_cycOn) g_cyc[CY_RUNS] += CYC() - c0;
            if (g_fgConfig.frameStats) g_pxRuns += n;
        }
        return;
    }

    /* scaled: the particles and the hint (tinted ones come from the tint cache) */
    struct CycScope { uint32_t t0; CycScope() : t0(CYC()) {} ~CycScope() { if (g_cycOn) g_cyc[CY_SCALED] += CYC() - t0; } } cycScope;
    const uint32_t stepX = op.stepX, stepY = op.stepY;
    const uint32_t mod = op.alpha;
    /* (stepX = tw/w in 16.16: the last pixel samples at most tw-1, no clamping needed) */
    for (int y = y0; y < y1; y++) {
        int sy = (int) (((uint32_t) (y - op.y) * stepY) >> 16);
        const uint16_t *srow = t->rgb + sy * t->w;
        uint16_t *drow = r->target + y * pitch + op.x;        /* indexed by op-relative x */
        int rx = x0 - op.x;
        const int rxEnd = x1 - op.x;
        uint32_t fx = (uint32_t) rx * stepX;
        if (!t->runs) {
            scaled_row(drow + rx, srow, t->alpha ? t->alpha + sy * t->w : NULL, fx, stepX, rxEnd - rx, (int) mod);
            continue;
        }
        /* one kernel call walks the row's runs and gathers the visible pixels,
           then one blend (or copy) per visible stretch */
        static uint16_t gs[1024 + 16];
        static uint8_t ga[1024 + 16];
        static int16_t spans[3 * 1024];
        const uint8_t *arow = t->alpha ? t->alpha + sy * t->w : NULL;     /* (NULL: hard mask) */
        int n = rxEnd - rx;
        if (n > 1024) n = 1024;
        uint32_t c0 = CYC();
        int16_t *end = scaled_spans(srow, arow, t->runs + t->rowRun[sy], t->runs + t->rowRun[sy + 1],
                                    fx, stepX, n, gs, ga, spans);
        uint32_t c1 = CYC();
        if (g_cycOn) { g_cyc[CY_SPANS] += c1 - c0; g_nSpans += (uint32_t) (end - spans) / 3; }
        if (g_fgConfig.frameStats) {
            g_scaledRows++;
            g_pxScaledWalk += n;
            for (int16_t *q = spans; q < end; q += 3) g_pxScaledVis += q[2];
        }
        spans_apply(drow + rx, gs, ga, spans, end, (int) mod);
        if (g_cycOn) g_cyc[CY_APPLY] += CYC() - c1;
    }
}

/* ================================================================ renderer */

SDL_Renderer *fg_renderer_new(int w, int h)
{
    SDL_Renderer *r = new SDL_Renderer;
    r->w = w;
    r->h = h;
    r->tilesX = (w + TILE - 1) / TILE;
    r->tilesY = (h + TILE - 1) / TILE;
    r->back = (uint16_t *) calloc((size_t) w * h + 16, 2);    /* (+16: AMMX remainders) */
    r->layer = (uint16_t *) calloc((size_t) w * h + 16, 2);   /* (none: works without it) */
    r->target = r->back;
    r->layerN = 0;
    r->lastS = 0;
    r->stableS = 0;
    r->fade.active = r->fade.pending = r->fade.suppress = false;
    r->dirty = (uint8_t *) calloc(r->tilesX * r->tilesY, 1);
    r->fullDirty = true;
    if (!r->back || !r->dirty) {
        fg_renderer_free(r);
        return NULL;
    }
    return r;
}

void fg_renderer_free(SDL_Renderer *r)
{
    if (!r) return;
    free(r->back);
    free(r->layer);
    free(r->dirty);
    delete r;
}

static bool op_less(const FgOp &a, const FgOp &b)
{
    if (a.texId != b.texId) return a.texId < b.texId;
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    if (a.w != b.w) return a.w < b.w;
    if (a.h != b.h) return a.h < b.h;
    if (a.alpha != b.alpha) return a.alpha < b.alpha;
    if (a.r != b.r) return a.r < b.r;
    if (a.g != b.g) return a.g < b.g;
    return a.b < b.b;
}

static void mark_rect(SDL_Renderer *r, const FgRect &b)
{
    int x0 = std::max(0, b.x0), y0 = std::max(0, b.y0);
    int x1 = std::min(r->w, b.x1), y1 = std::min(r->h, b.y1);
    if (x0 >= x1 || y0 >= y1) return;
    for (int ty = y0 / TILE; ty <= (y1 - 1) / TILE; ty++)
        memset(r->dirty + ty * r->tilesX + x0 / TILE, 1, (x1 - 1) / TILE - x0 / TILE + 1);
}

/* the fading image handled band by band this frame: not marked as a whole */
static inline bool fade_op(const SDL_Renderer *r, const FgOp &o)
{
    return r->fade.suppress && o.texId == r->fade.texId && o.x == r->fade.x && o.y == r->fade.y &&
           o.w == r->fade.w && o.h == r->fade.h;
}

static void mark(SDL_Renderer *r, const FgOp &op)
{
    FgRect b = op_box(op);
    int x0 = std::max(0, b.x0), y0 = std::max(0, b.y0);
    int x1 = std::min(r->w, b.x1), y1 = std::min(r->h, b.y1);
    if (x0 >= x1 || y0 >= y1) return;
    for (int ty = y0 / TILE; ty <= (y1 - 1) / TILE; ty++)
        memset(r->dirty + ty * r->tilesX + x0 / TILE, 1, (x1 - 1) / TILE - x0 / TILE + 1);
}

static void draw_op(SDL_Renderer *r, const FgOp &op, const FgRect &c);
static inline bool op_covers(const FgOp &o, const FgRect &c);

/* the static layer follows the stable part of the frame:
 *  - it holds ops[0 .. layerN); valid while those ops do not change
 *  - grows by painting the newly stable ops on top (cheap)
 *  - is rebuilt from scratch only when its part changed, after the first
 *    changed index has stayed put for a few frames */
uint32_t g_layerBuilds = 0, g_layerGrows = 0, g_layerRepairs = 0;
uint32_t g_tLayer = 0, g_tRects = 0;     /* STATS: us in update_layer / in composing the rects */

static inline bool op_same(const FgOp &a, const FgOp &b);

/* paints ops[0 .. n) into the layer, clipped to c (from the topmost op that
   covers c, else from black) */
static void layer_compose(SDL_Renderer *r, const std::vector<FgOp> &ops, size_t n, const FgRect &c)
{
    int start = -1;
    for (int j = (int) n - 1; j >= 0; j--) {
        const FgRect b = op_box(ops[j]);
        if (b.x0 >= c.x1 || b.y0 >= c.y1 || b.x1 <= c.x0 || b.y1 <= c.y0) continue;
        if (op_covers(ops[j], c)) { start = j; break; }
    }
    if (start < 0) {
        for (int y = c.y0; y < c.y1; y++) memset(r->layer + y * r->w + c.x0, 0, (c.x1 - c.x0) * 2);
        start = 0;
    }
    for (size_t i = start; i < n; i++) draw_op(r, ops[i], c);
}

static FgRect clip_box(const SDL_Renderer *r, const FgRect &b)
{
    FgRect c = { std::max(0, b.x0), std::max(0, b.y0), std::min(r->w, b.x1), std::min(r->h, b.y1) };
    return c;
}

/* The static layer: the composite of ops[0 .. layerN) (in depth order), the
 * part of the frame below the first op that keeps moving.
 *  - "moving" = changed in this AND the last frame (falling gems, particles);
 *    rare in-place changes (the time display, a fade step, a vanishing gem)
 *    are repaired locally in the layer, in their old and new rectangle
 *  - an op inserted or removed inside it (particles, scores appear) shrinks
 *    it to the ops below: only the rectangles of the ops above are composed
 *    again, the rest stays
 *  - it grows (cheap) when the moving part starts higher for a while */
static void layer_repair(SDL_Renderer *r, const std::vector<FgOp> &ops, size_t n, const FgOp &o)
{
    FgRect c = clip_box(r, op_box(o));
    if (g_fgConfig.debugLayer)
        FG_LOG("[layer] repair tex %ld at %ld,%ld box %ldx%ld (n %ld)\n", (long) o.texId, (long) o.x, (long) o.y,
               (long) (c.x1 - c.x0), (long) (c.y1 - c.y0), (long) n);
    if (c.x0 < c.x1 && c.y0 < c.y1) layer_compose(r, ops, n, c);
    g_layerRepairs++;
}

static void update_layer(SDL_Renderer *r, const std::vector<FgOp> &ops, const std::vector<FgOp> &prev)
{
    if (!r->layer || g_fgConfig.fullRedraw) { r->layerN = 0; return; }

    /* positions that hold the same op slot in both frames: up to the first slot
       with another texture (an op inserted or removed). (Not the first changed
       op: a fade step or a move in the same frame is an in-place change.) */
    size_t aligned = std::min(ops.size(), prev.size());
    if (ops.size() != prev.size()) {
        size_t k = 0;
        while (k < aligned && ops[k].texId == prev[k].texId) k++;
        aligned = k;
    }

    /* changed flags per position; "moving" = changed twice in a row */
    std::vector<uint8_t> &ch = r->changed, &chPrev = r->changedPrev;
    chPrev.swap(ch);
    ch.assign(ops.size(), 1);
    int fadeIdx = -1;
    for (size_t i = 0; i < aligned; i++) {
        ch[i] = !op_same(ops[i], prev[i]);
        /* a big image whose alpha alone changes (the logo fading in) does not
           count as a change: it stays in the layer, updated band by band below */
        if (ch[i] && ops[i].texId == prev[i].texId && ops[i].x == prev[i].x && ops[i].y == prev[i].y &&
            ops[i].w == prev[i].w && ops[i].h == prev[i].h && ops[i].r == prev[i].r && ops[i].g == prev[i].g &&
            ops[i].b == prev[i].b && (int) ops[i].w * ops[i].h > 65536) {
            ch[i] = 0;
            if (fadeIdx < 0) fadeIdx = (int) i;
        }
    }
    size_t top = ops.size();
    for (size_t i = 0; i < ops.size(); i++)
        if (ch[i] && (i >= chPrev.size() || chPrev[i])) { top = i; break; }

    r->target = r->layer;
    if (r->layerN) {
        /* shrink: ops of the layer above an insertion / removal leave it */
        if (r->layerN > aligned) {
            if (g_fgConfig.debugLayer) FG_LOG("[layer] shrink %ld -> %ld (shift)\n", (long) r->layerN, (long) aligned);
            size_t old = r->layerN;
            r->layerN = aligned;
            for (size_t i = aligned; i < old && i < prev.size(); i++) layer_repair(r, ops, r->layerN, prev[i]);
        }
        /* in-place changes inside it */
        for (size_t i = 0; i < r->layerN; i++) {
            if (!ch[i]) continue;
            FgRect ob = op_box(prev[i]), nb = op_box(ops[i]);
            /* (a fade step: the same rectangle twice -- repaired once) */
            if (ob.x0 != nb.x0 || ob.y0 != nb.y0 || ob.x1 != nb.x1 || ob.y1 != nb.y1)
                layer_repair(r, ops, r->layerN, prev[i]);
            layer_repair(r, ops, r->layerN, ops[i]);
        }
        /* the moving part reaches down into it: give those ops up as well */
        if (top < r->layerN) {
            if (g_fgConfig.debugLayer) FG_LOG("[layer] shrink %ld -> %ld (moving)\n", (long) r->layerN, (long) top);
            size_t old = r->layerN;
            r->layerN = top;
            for (size_t i = top; i < old; i++) {
                layer_repair(r, ops, r->layerN, ops[i]);
                if (i < prev.size()) {
                    FgRect ob = op_box(prev[i]), nb = op_box(ops[i]);
                    if (ob.x0 != nb.x0 || ob.y0 != nb.y0 || ob.x1 != nb.x1 || ob.y1 != nb.y1)
                        layer_repair(r, ops, r->layerN, prev[i]);
                }
            }
        }
    }

    if (top == r->lastS) r->stableS++;
    else r->stableS = 0;
    r->lastS = top;

    if (r->layerN && top > r->layerN && r->stableS >= 2) {
        /* grow: paint the newly stable ops on top */
        if (g_fgConfig.debugLayer) FG_LOG("[layer] grow %ld -> %ld\n", (long) r->layerN, (long) top);
        for (size_t i = r->layerN; i < top; i++) {
            FgRect c = clip_box(r, op_box(ops[i]));
            if (c.x0 < c.x1 && c.y0 < c.y1) draw_op(r, ops[i], c);
        }
        r->layerN = top;
        g_layerGrows++;
    } else if (!r->layerN && top >= 2 && r->stableS >= 3) {
        /* build from scratch (at the start of a screen) */
        const FgRect full = { 0, 0, r->w, r->h };
        layer_compose(r, ops, top, full);
        r->layerN = top;
        g_layerBuilds++;
    }

    /* the fading image in the layer: one of its three bands per frame */
    r->fade.suppress = false;
    int f = -1;
    if (fadeIdx >= 0 && (size_t) fadeIdx < r->layerN) {
        f = fadeIdx;
        const FgOp &o = ops[f];
        if (!r->fade.active || r->fade.texId != o.texId || r->fade.x != o.x || r->fade.y != o.y) {
            r->fade.active = true;
            r->fade.texId = o.texId; r->fade.x = o.x; r->fade.y = o.y; r->fade.w = o.w; r->fade.h = o.h;
            for (int b = 0; b < 3; b++) r->fade.bandAlpha[b] = prev[f].alpha;   /* (what the layer holds) */
            r->fade.next = 0;
        }
    } else if (r->fade.active && r->fade.pending) {
        for (size_t i = 0; i < r->layerN; i++)
            if (ops[i].texId == r->fade.texId && ops[i].x == r->fade.x && ops[i].y == r->fade.y) { f = (int) i; break; }
        if (f < 0) r->fade.active = r->fade.pending = false;   /* (gone or out of the layer: drawn normally) */
    } else if (fadeIdx < 0) {
        r->fade.active = false;
    }
    if (f >= 0) {
        const FgOp &o = ops[f];
        const int bh = (o.h + 2) / 3;
        for (int k = 0; k < 3; k++) {
            int b = (r->fade.next + k) % 3;
            if (r->fade.bandAlpha[b] == o.alpha) continue;
            FgRect c = clip_box(r, op_box(o));
            c.y0 = std::max(c.y0, o.y + b * bh);
            c.y1 = std::min(c.y1, o.y + (b + 1) * bh);
            if (c.y0 < c.y1 && c.x0 < c.x1) {
                layer_compose(r, ops, r->layerN, c);
                r->extraDirty.push_back(c);
            }
            r->fade.bandAlpha[b] = o.alpha;
            r->fade.next = b + 1;
            break;
        }
        r->fade.pending = false;
        for (int b = 0; b < 3; b++) if (r->fade.bandAlpha[b] != o.alpha) r->fade.pending = true;
        r->fade.suppress = true;          /* (its own box is not marked dirty: only the band) */
    }
    r->target = r->back;
}

static inline bool op_same(const FgOp &a, const FgOp &b)
{
    return a.texId == b.texId && a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h &&
           a.alpha == b.alpha && a.r == b.r && a.g == b.g && a.b == b.b;
}

/* an op that paints every pixel of c with its own color (nothing below it shows) */
static inline bool op_covers(const FgOp &o, const FgRect &c)
{
    return o.tex->kind == FG_TEX_OPAQUE && o.alpha == 255 && (o.r & o.g & o.b) == 255 &&
           o.w == o.tex->w && o.h == o.tex->h &&
           o.x <= c.x0 && o.y <= c.y0 && o.x + o.w >= c.x1 && o.y + o.h >= c.y1;
}

void fg_renderer_compose(SDL_Renderer *r, const std::vector<FgOp> &ops)
{
#ifdef FG_V080
    if (g_ammx) fg_ammx_setup();      /* (the AMMX constants, once per frame) */
#endif
    /* 0. the very same frame as before (the usual case): nothing to do */
    if (!r->fullDirty && !r->fade.pending && ops.size() == r->prev.size()) {
        size_t i = 0;
        while (i < ops.size() && op_same(ops[i], r->prev[i])) i++;
        if (i == ops.size()) {
            r->rects.clear();
            return;
        }
    }
    uint32_t tl0 = fg_micros(), cl0 = CYC();
    update_layer(r, ops, r->prev);
    g_tLayer += fg_micros() - tl0;
    uint32_t cd0 = CYC();
    if (g_cycOn) g_cyc[CY_LAYER] += cd0 - cl0;
    r->prev = ops;

    /* 1. what changed since the last frame: ops of only one of the two frames */
    if (r->fullDirty) {
        memset(r->dirty, 1, r->tilesX * r->tilesY);
    } else {
        memset(r->dirty, 0, r->tilesX * r->tilesY);
        r->sortedCur = ops;
        std::sort(r->sortedCur.begin(), r->sortedCur.end(), op_less);
        size_t i = 0, j = 0;
        const std::vector<FgOp> &a = r->sortedPrev, &b = r->sortedCur;
        while (i < a.size() || j < b.size()) {
            if (j >= b.size() || (i < a.size() && op_less(a[i], b[j]))) { if (!fade_op(r, a[i])) mark(r, a[i]); i++; }
            else if (i >= a.size() || op_less(b[j], a[i])) { if (!fade_op(r, b[j])) mark(r, b[j]); j++; }
            else { i++; j++; }
        }
    }

    if (g_cycOn) g_cyc[CY_DIFF] += CYC() - cd0;
    for (size_t k = 0; k < r->extraDirty.size(); k++) mark_rect(r, r->extraDirty[k]);
    r->extraDirty.clear();

    /* 2. dirty tiles -> rectangles (row runs, joined downwards when equal) */
    r->rects.clear();
    size_t rowStart = 0;
    for (int ty = 0; ty < r->tilesY; ty++) {
        size_t thisRow = r->rects.size();
        const uint8_t *row = r->dirty + ty * r->tilesX;
        for (int tx = 0; tx < r->tilesX;) {
            if (!row[tx]) { tx++; continue; }
            int s = tx;
            while (tx < r->tilesX && row[tx]) tx++;
            FgRect rc = { s * TILE, ty * TILE, std::min(r->w, tx * TILE), std::min(r->h, (ty + 1) * TILE) };
            bool joined = false;
            for (size_t k = rowStart; k < thisRow; k++) {
                FgRect &o = r->rects[k];
                if (o.x0 == rc.x0 && o.x1 == rc.x1 && o.y1 == rc.y0) {
                    o.y1 = rc.y1;
                    /* keep it in the "previous row" window */
                    FgRect moved = o;
                    r->rects.erase(r->rects.begin() + k);
                    r->rects.push_back(moved);
                    thisRow--;
                    joined = true;
                    break;
                }
            }
            if (!joined) r->rects.push_back(rc);
        }
        rowStart = thisRow;
    }

    /* 3. compose every rectangle: only the ops that touch it, from the topmost
     *    one that covers it completely (no clear then) upwards */
    std::vector<int> &idx = r->idx;
    uint32_t tr0 = fg_micros(), cr0 = CYC();
    for (size_t k = 0; k < r->rects.size(); k++) {
        const FgRect &c = r->rects[k];
        idx.clear();
        for (size_t i = 0; i < ops.size(); i++) {
            const FgOp &o = ops[i];
            FgRect b = op_box(o);
            if (b.x0 < c.x1 && b.y0 < c.y1 && b.x1 > c.x0 && b.y1 > c.y0) idx.push_back((int) i);
        }
        int start = -1;
        for (int j = (int) idx.size() - 1; j >= 0; j--) {
            if ((size_t) idx[j] < r->layerN) break;          /* (below: the layer has it) */
            if (op_covers(ops[idx[j]], c)) { start = j; break; }
        }
        if (start < 0) {
            /* the layer (or black) as the base, then only the ops above it */
            for (int y = c.y0; y < c.y1; y++) {
                if (r->layerN) {
                    uint32_t c0 = CYC();
                    row_copy(r->back + y * r->w + c.x0, r->layer + y * r->w + c.x0, c.x1 - c.x0);
                    if (g_cycOn) g_cyc[CY_COPY] += CYC() - c0;
                }
                else memset(r->back + y * r->w + c.x0, 0, (c.x1 - c.x0) * 2);
            }
            start = 0;
            while ((size_t) start < idx.size() && (size_t) idx[start] < r->layerN) start++;
        }
        for (size_t j = start; j < idx.size(); j++)
            draw_op(r, ops[idx[j]], c);
    }

    g_tRects += fg_micros() - tr0;
    if (g_cycOn) g_cyc[CY_RECTS] += CYC() - cr0;
    if (r->fullDirty) {
        r->sortedCur = ops;
        std::sort(r->sortedCur.begin(), r->sortedCur.end(), op_less);
        r->fullDirty = false;
    }
    r->sortedPrev.swap(r->sortedCur);
}
