/* Asset caches of the AmigaOS port: every file of PROGDIR:media is loaded
 * once at start-up (with a progress bar) and shared afterwards. The game's
 * Image / Sound / Music / Font objects only look up these caches. */

#include "amiga_internal.h"

#include <dos/dos.h>
#include <dos/exall.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

static std::map<std::string, std::shared_ptr<SDL_Texture> > sGfx;
static std::map<std::string, std::vector<uint8_t> > sFiles;     /* fonts */
static bool sPreloaded = false;

static bool ends_with(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char) s[s.size() - n + i]) != suffix[i]) return false;
    return true;
}

std::shared_ptr<SDL_Texture> fg_gfx_get(const std::string &path)
{
    std::map<std::string, std::shared_ptr<SDL_Texture> >::iterator it = sGfx.find(path);
    if (it != sGfx.end()) return it->second;
    SDL_Texture *t = fg_texture_load(path.c_str());
    if (!t) return std::shared_ptr<SDL_Texture>();
    if (sPreloaded) FG_LOG("[assets] late load %s\n", path.c_str());
    std::shared_ptr<SDL_Texture> p(t, SDL_DestroyTexture);
    sGfx[path] = p;
    return p;
}

const std::vector<uint8_t> *fg_file_get(const std::string &path)
{
    std::map<std::string, std::vector<uint8_t> >::iterator it = sFiles.find(path);
    if (it != sFiles.end()) return &it->second;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> &v = sFiles[path];
    v.resize(len > 0 ? len : 0);
    if (len > 0 && fread(&v[0], 1, len, f) != (size_t) len) {
        fclose(f);
        sFiles.erase(path);
        return NULL;
    }
    fclose(f);
    return &v;
}

/* all files below dir (recursive), as "dir/name" paths */
static void scan(const std::string &dir, std::vector<std::string> &out)
{
    BPTR lock = Lock((CONST_STRPTR) dir.c_str(), ACCESS_READ);
    if (!lock) return;
    struct FileInfoBlock *fib = (struct FileInfoBlock *) AllocDosObject(DOS_FIB, NULL);
    std::vector<std::string> dirs;
    if (fib && Examine(lock, fib)) {
        while (ExNext(lock, fib)) {
            std::string p = dir + "/" + (const char *) fib->fib_FileName;
            if (fib->fib_DirEntryType > 0) dirs.push_back(p);
            else out.push_back(p);
        }
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    for (size_t i = 0; i < dirs.size(); i++) scan(dirs[i], out);
}

/* (bool fg_audio_preload(path): audio.cpp; void fg_font_prewarm(path, size, chars): go_font.cpp) */
bool fg_audio_preload(const std::string &path);
void fg_font_prewarm(const std::string &path, unsigned size, const char *chars);

void fg_preload(const std::string &base, void (*progress)(void *ctx, int done, int total), void *ctx)
{
    uint32_t t0 = fg_micros();
    std::vector<std::string> files;
    scan(base + "media", files);

    /* the fonts in the sizes the game uses (and the characters it draws) */
    static const char ASCII[] = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
    static const struct { const char *file; unsigned size; const char *chars; } FONTS[] = {
        { "media/fuenteMenu.ttf", 23, ASCII }, { "media/fuenteMenu.ttf", 30, ASCII },
        { "media/fuenteMenu.ttf", 48, ASCII }, { "media/fuenteMenu.ttf", 60, ASCII },
        { "media/fuenteMenu.ttf", 64, ASCII },
        { "media/fuenteNormal.ttf", 27, ASCII }, { "media/fuenteNormal.ttf", 28, ASCII },
        { "media/fuenteNormal.ttf", 35, ASCII }, { "media/fuenteNormal.ttf", 37, ASCII },
        { "media/fuentelcd.ttf", 33, "0123456789:-" }, { "media/fuentelcd.ttf", 36, "0123456789:-" },
        { "media/fuentelcd.ttf", 60, "0123456789:-+x" }, { "media/fuentelcd.ttf", 62, "0123456789:-" },
        { "media/fuentelcd.ttf", 72, "0123456789:-" },
    };
    const int nFonts = sizeof(FONTS) / sizeof(FONTS[0]);
    int total = (int) files.size() + nFonts, done = 0;
    size_t bytes = 0;

    for (size_t i = 0; i < files.size(); i++) {
        const std::string &p = files[i];
        if (ends_with(p, ".gfx")) {
            std::shared_ptr<SDL_Texture> t = fg_gfx_get(p);
            if (t) bytes += (size_t) t->w * t->h * (t->alpha ? 3 : 2);
        } else if (ends_with(p, ".snd")) {
            fg_audio_preload(p);
        } else if (ends_with(p, ".ttf")) {
            const std::vector<uint8_t> *d = fg_file_get(p);
            if (d) bytes += d->size();
        }
        if (progress) progress(ctx, ++done, total);
    }
    for (int i = 0; i < nFonts; i++) {
        fg_font_prewarm(base + FONTS[i].file, FONTS[i].size, FONTS[i].chars);
        if (progress) progress(ctx, ++done, total);
    }
    /* precomputed fade: the menu logo over the menu background at (86, 0) */
    {
        std::shared_ptr<SDL_Texture> logo = fg_gfx_get(base + "media/stateMainMenu/mainMenuLogo.gfx");
        std::shared_ptr<SDL_Texture> bg = fg_gfx_get(base + "media/stateMainMenu/mainMenuBackground.gfx");
        if (logo && bg) fg_premix_register(logo.get(), bg.get(), 86, 0);
    }
    /* the particles at every size they are drawn at (108 px x 1.0 .. 1.5, shrinking) */
    {
        uint32_t tp = fg_micros();
        static const char *parts[] = { "media/partc1.gfx", "media/partc2.gfx" };
        for (int i = 0; i < 2; i++) {
            std::shared_ptr<SDL_Texture> p = fg_gfx_get(base + parts[i]);
            if (p && !g_fgConfig.noPrescale) fg_prescale_prewarm(p.get(), 164);
        }
        fg_info("[assets] particles pre-scaled in %ld ms\n", (long) ((fg_micros() - tp) / 1000));
    }
    sPreloaded = true;
    fg_info("[assets] %ld files preloaded (%ld KB graphics/fonts) in %ld ms\n", (long) files.size(),
           (long) (bytes / 1024), (long) ((fg_micros() - t0) / 1000));
}

void fg_assets_free()
{
    fg_premix_free();
    fg_prescale_free();
    sGfx.clear();
    sFiles.clear();
}
