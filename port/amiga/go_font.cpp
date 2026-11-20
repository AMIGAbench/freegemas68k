/* GoSDL::Font for AmigaOS: FreeType (static libfreetype of the toolchain)
 * renders the TrueType fonts into RGBA scratch images, which become
 * RGB565 + A8 textures. Faces and glyphs are cached for the whole run, the
 * game makes Font objects on the fly. Layout follows SDL3_ttf. */

#include "go_font.h"
#include "amiga_internal.h"
#include "Util.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

struct FgGlyph {
    int left, top, w, h, pitch;
    int advance;               /* 26.6 */
    std::vector<uint8_t> bits;
};

struct TTF_Font {
    FT_Face face;
    int ascent, descent, height, lineskip;
    std::map<uint32_t, FgGlyph> glyphs;
};

static FT_Library sFT = NULL;
static std::map<std::string, TTF_Font *> sFonts;

static inline int ft_ceil(FT_Long v) { return (int) ((v + 63) & -64) / 64; }
static inline int ft_floor(FT_Long v) { return (int) (v & -64) / 64; }

static TTF_Font *open_font(const std::string &path, unsigned size)
{
    std::string key = path + "#" + std::to_string(size);
    std::map<std::string, TTF_Font *>::iterator it = sFonts.find(key);
    if (it != sFonts.end()) return it->second;

    if (!sFT && FT_Init_FreeType(&sFT)) return NULL;
    FT_Face face;
    const std::vector<uint8_t> *data = fg_file_get(path);      /* (in memory: no disk access later) */
    if (!data || data->empty() || FT_New_Memory_Face(sFT, &(*data)[0], (FT_Long) data->size(), 0, &face))
        return NULL;
    if (FT_Set_Char_Size(face, 0, size * 64, 0, 0)) {
        FT_Done_Face(face);
        return NULL;
    }
    TTF_Font *f = new TTF_Font;
    f->face = face;
    FT_Fixed scale = face->size->metrics.y_scale;
    f->ascent = ft_ceil(FT_MulFix(face->ascender, scale));
    f->descent = ft_ceil(FT_MulFix(face->descender, scale));
    f->height = f->ascent - f->descent;
    f->lineskip = ft_ceil(FT_MulFix(face->height, scale));
    sFonts[key] = f;
    return f;
}

static const FgGlyph *glyph(TTF_Font *f, uint32_t cp)
{
    std::map<uint32_t, FgGlyph>::iterator it = f->glyphs.find(cp);
    if (it != f->glyphs.end()) return &it->second;
    FgGlyph &g = f->glyphs[cp];
    g.left = g.top = g.w = g.h = g.pitch = g.advance = 0;
    FT_UInt idx = FT_Get_Char_Index(f->face, cp);
    if (FT_Load_Glyph(f->face, idx, FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL) ||
        FT_Render_Glyph(f->face->glyph, FT_RENDER_MODE_NORMAL))
        return &g;
    FT_GlyphSlot s = f->face->glyph;
    g.left = s->bitmap_left;
    g.top = s->bitmap_top;
    g.w = s->bitmap.width;
    g.h = s->bitmap.rows;
    g.pitch = g.w;
    g.advance = (int) s->advance.x;
    g.bits.resize((size_t) g.w * g.h);
    for (int y = 0; y < g.h; y++)
        memcpy(&g.bits[y * g.w], s->bitmap.buffer + y * s->bitmap.pitch, g.w);
    return &g;
}

void fg_font_prewarm(const std::string &path, unsigned size, const char *chars)
{
    TTF_Font *f = open_font(path, size);
    if (!f) return;
    for (const char *c = chars; *c; c++) glyph(f, (unsigned char) *c);
}

static std::vector<uint32_t> utf8(const std::string &s)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

/* width of a line and the leftmost ink offset */
static int measure(TTF_Font *f, const std::vector<uint32_t> &cps, int *minxOut)
{
    FT_Pos pen = 0;
    int minx = 0, maxx = 0;
    FT_UInt prev = 0;
    bool kern = FT_HAS_KERNING(f->face);
    for (size_t i = 0; i < cps.size(); i++) {
        FT_UInt idx = FT_Get_Char_Index(f->face, cps[i]);
        if (kern && prev && idx) {
            FT_Vector d;
            FT_Get_Kerning(f->face, prev, idx, FT_KERNING_DEFAULT, &d);
            pen += d.x;
        }
        const FgGlyph *g = glyph(f, cps[i]);
        int x = ft_floor(pen) + g->left;
        minx = std::min(minx, x);
        maxx = std::max(maxx, x + g->w);
        pen += g->advance;
        prev = idx;
    }
    maxx = std::max(maxx, ft_ceil(pen));
    if (minxOut) *minxOut = minx;
    return maxx - minx;
}

/* draws one line into the surface at (ox, baseline oy) with the color */
static void draw_line(TTF_Font *f, SDL_Surface *s, const std::vector<uint32_t> &cps, int ox, int oy, SDL_Color c)
{
    FT_Pos pen = 0;
    FT_UInt prev = 0;
    bool kern = FT_HAS_KERNING(f->face);
    for (size_t i = 0; i < cps.size(); i++) {
        FT_UInt idx = FT_Get_Char_Index(f->face, cps[i]);
        if (kern && prev && idx) {
            FT_Vector d;
            FT_Get_Kerning(f->face, prev, idx, FT_KERNING_DEFAULT, &d);
            pen += d.x;
        }
        const FgGlyph *g = glyph(f, cps[i]);
        int gx = ox + ft_floor(pen) + g->left;
        int gy = oy - g->top;
        for (int y = 0; y < g->h; y++) {
            int dy = gy + y;
            if (dy < 0 || dy >= s->h) continue;
            for (int x = 0; x < g->w; x++) {
                int dx = gx + x;
                if (dx < 0 || dx >= s->w) continue;
                unsigned cov = g->bits[y * g->w + x];
                if (!cov) continue;
                uint8_t *p = s->rgba + (dy * s->w + dx) * 4;
                unsigned a = cov * c.a / 255;
                if (a > p[3]) {          /* overlapping glyphs: keep the stronger coverage */
                    p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = (uint8_t) a;
                }
            }
        }
        pen += g->advance;
        prev = idx;
    }
}

static SDL_Surface *render_lines(TTF_Font *f, const std::vector<std::vector<uint32_t> > &lines, SDL_Color c)
{
    int w = 0, minx = 0;
    std::vector<int> minxs(lines.size());
    for (size_t i = 0; i < lines.size(); i++) {
        w = std::max(w, measure(f, lines[i], &minxs[i]));
        minx = std::min(minx, minxs[i]);
    }
    int h = f->height + (int) (lines.size() > 0 ? lines.size() - 1 : 0) * f->lineskip;
    SDL_Surface *s = fg_surface_new(w > 0 ? w : 1, h > 0 ? h : 1);
    if (!s) return NULL;
    for (size_t i = 0; i < lines.size(); i++)
        draw_line(f, s, lines[i], -minx, f->ascent + (int) i * f->lineskip, c);
    return s;
}

static std::vector<std::vector<uint32_t> > wrap(TTF_Font *f, const std::string &text, unsigned width)
{
    std::vector<std::vector<uint32_t> > lines;
    std::vector<uint32_t> all = utf8(text);
    std::vector<uint32_t> line, word;
    const uint32_t SP = ' ';
    for (size_t i = 0; i <= all.size(); i++) {
        uint32_t c = i < all.size() ? all[i] : '\n';
        if (c == ' ' || c == '\n' || c == '\t') {
            if (!word.empty()) {
                std::vector<uint32_t> test = line;
                if (!test.empty()) test.push_back(SP);
                test.insert(test.end(), word.begin(), word.end());
                if (width > 0 && !line.empty() && (unsigned) measure(f, test, NULL) > width) {
                    lines.push_back(line);
                    line = word;
                } else {
                    line = test;
                }
                word.clear();
            }
            if (c == '\n') {
                lines.push_back(line);
                line.clear();
            }
        } else {
            word.push_back(c);
        }
    }
    while (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    return lines;
}

/* ------------------------------------------------------------------ text cache */
/* Rendered texts are kept: the same font, text, colors and layout give the same
 * texture (no FreeType work, and a stable texture id for the dirty-rect diff). */

static std::map<std::string, GoSDL::Image> sTextCache;
uint32_t g_tFont = 0;     /* STATS: us spent rendering new texts */

static std::string text_key(TTF_Font *f, char kind, const std::string &text, SDL_Color c,
                            unsigned width, int sx, int sy, SDL_Color sc)
{
    char hd[96];
    snprintf(hd, sizeof(hd), "%p|%c|%02x%02x%02x%02x|%u|%d|%d|%02x%02x%02x%02x|", (void *) f, kind,
             c.r, c.g, c.b, c.a, width, sx, sy, sc.r, sc.g, sc.b, sc.a);
    return std::string(hd) + text;
}

uint32_t g_textT0 = 0;

static bool text_lookup(const std::string &key, GoSDL::Window *w, GoSDL::Image &out)
{
    g_textT0 = fg_micros();
    std::map<std::string, GoSDL::Image>::iterator it = sTextCache.find(key);
    if (it == sTextCache.end()) return false;
    out = GoSDL::Image(it->second);
    out.setWindow(w);
    return true;
}

static GoSDL::Image text_store(const std::string &key, GoSDL::Image img)
{
    extern uint32_t g_textT0;
    g_tFont += fg_micros() - g_textT0;
    if (sTextCache.size() >= 768) sTextCache.clear();     /* (a bound; never reached in play) */
    sTextCache.insert(std::make_pair(key, GoSDL::Image(img)));
    return img;
}

/* ------------------------------------------------------------------ GoSDL::Font */

GoSDL::Font::Font() { checkInit(); }

GoSDL::Font::~Font()
{
    mFont = NULL;           /* (cached for the whole run) */
}

void GoSDL::Font::checkInit() { }

void GoSDL::Font::setPathAndSize(std::string path, unsigned size)
{
    mPath = getBasePath() + path;
    mSize = size;
    mFont = open_font(mPath, mSize);
    if (!mFont)
        throw std::runtime_error("Cannot load the font " + mPath);
}

void GoSDL::Font::setAll(Window *parentWindow, std::string path, unsigned size)
{
    setWindow(parentWindow);
    setPathAndSize(path, size);
}

int GoSDL::Font::getTextWidth(std::string text)
{
    if (!mFont) return 0;
    return measure(mFont, utf8(text), NULL);
}

void GoSDL::Font::setWindow(Window *parentWindow) { mParentWindow = parentWindow; }

GoSDL::Image GoSDL::Font::renderText(std::string text, SDL_Color color)
{
    static const SDL_Color none = { 0, 0, 0, 0 };
    std::string key = text_key(mFont, 'T', text, color, 0, 0, 0, none);
    GoSDL::Image img;
    if (text_lookup(key, mParentWindow, img)) return img;
    std::vector<std::vector<uint32_t> > lines(1, utf8(text));
    return text_store(key, surfaceToImage(render_lines(mFont, lines, color)));
}

GoSDL::Image GoSDL::Font::renderTextWithShadow(std::string text, SDL_Color color, int shadowX, int shadowY, SDL_Color shadowColor)
{
    std::string key = text_key(mFont, 'S', text, color, 0, shadowX, shadowY, shadowColor);
    GoSDL::Image img;
    if (text_lookup(key, mParentWindow, img)) return img;
    std::vector<std::vector<uint32_t> > lines(1, utf8(text));
    return text_store(key, surfaceToImageWithShadow(render_lines(mFont, lines, color),
                                                    render_lines(mFont, lines, shadowColor), shadowX, shadowY));
}

GoSDL::Image GoSDL::Font::renderBlock(std::string text, SDL_Color color, unsigned width)
{
    static const SDL_Color none = { 0, 0, 0, 0 };
    std::string key = text_key(mFont, 'B', text, color, width, 0, 0, none);
    GoSDL::Image img;
    if (text_lookup(key, mParentWindow, img)) return img;
    return text_store(key, surfaceToImage(render_lines(mFont, wrap(mFont, text, width), color)));
}

GoSDL::Image GoSDL::Font::renderBlockWithShadow(std::string text, SDL_Color color, unsigned width, int shadowX, int shadowY, SDL_Color shadowColor)
{
    std::string key = text_key(mFont, 'W', text, color, width, shadowX, shadowY, shadowColor);
    GoSDL::Image img;
    if (text_lookup(key, mParentWindow, img)) return img;
    std::vector<std::vector<uint32_t> > lines = wrap(mFont, text, width);
    return text_store(key, surfaceToImageWithShadow(render_lines(mFont, lines, color),
                                                    render_lines(mFont, lines, shadowColor), shadowX, shadowY));
}

GoSDL::Image GoSDL::Font::surfaceToImage(SDL_Surface *s)
{
    GoSDL::Image img;
    img.setWindow(mParentWindow);
    if (s) {
        img.setTexture(fg_texture_from_rgba(s->rgba, s->w, s->h));
        fg_surface_free(s);
    }
    return img;
}

/* like SDL: the shadow copied at (shadowX, shadowY), the text blended over it at (0, 0) */
GoSDL::Image GoSDL::Font::surfaceToImageWithShadow(SDL_Surface *text, SDL_Surface *shadow, int shadowX, int shadowY)
{
    if (!text || !shadow) {
        fg_surface_free(shadow);
        return surfaceToImage(text);
    }
    int sx = std::max(0, shadowX), sy = std::max(0, shadowY);
    SDL_Surface *o = fg_surface_new(sx + shadow->w, sy + shadow->h);
    if (!o) {
        fg_surface_free(shadow);
        return surfaceToImage(text);
    }
    for (int y = 0; y < shadow->h; y++)
        memcpy(o->rgba + ((y + sy) * o->w + sx) * 4, shadow->rgba + y * shadow->w * 4, shadow->w * 4);
    for (int y = 0; y < text->h && y < o->h; y++) {
        for (int x = 0; x < text->w && x < o->w; x++) {
            const uint8_t *s = text->rgba + (y * text->w + x) * 4;
            uint8_t *d = o->rgba + (y * o->w + x) * 4;
            unsigned a = s[3];
            if (!a) continue;
            for (int k = 0; k < 3; k++) d[k] = (uint8_t) ((s[k] * a + d[k] * (255 - a)) / 255);
            d[3] = (uint8_t) (a + d[3] * (255 - a) / 255);
        }
    }
    fg_surface_free(text);
    fg_surface_free(shadow);
    return surfaceToImage(o);
}
