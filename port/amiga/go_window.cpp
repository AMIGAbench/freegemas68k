/* GoSDL::Window for AmigaOS: frame loop, input dispatch, composition. */

#include "go_window.h"
#include "amiga_internal.h"
#include "log.h"

#include <proto/exec.h>
#include <dos/dos.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

extern uint32_t g_layerBuilds, g_layerGrows, g_layerRepairs, g_tLayer, g_tRects;   /* (render.cpp) */
extern uint32_t g_tFont;   /* (go_font.cpp) */
extern uint32_t g_pxScaledWalk, g_pxScaledVis, g_pxRuns, g_scaledRows;
extern uint32_t g_cyc[8], g_nSpans;
extern int g_cycOn, g_ammx;

/* ---- test support: scripted input and frame dumps (SCRIPT, SHOTS) */
struct FgScriptStep { uint32_t frame; char action[16]; int x, y; };
static std::vector<FgScriptStep> sScript;
static size_t sScriptPos = 0;
static uint32_t sFrame = 0;
static std::vector<std::string> sStatLines;   /* STATS, written at the end */

static void parse_script(const char *s)
{
    while (s && *s) {
        FgScriptStep st;
        memset(&st, 0, sizeof(st));
        st.frame = strtoul(s, (char **) &s, 10);
        if (*s == '=') s++;
        int n = 0;
        while (*s && *s != '/' && *s != ';' && *s != '+' && n < 15) st.action[n++] = *s++;
        if (*s == '/') { st.x = strtol(s + 1, (char **) &s, 10); }
        if (*s == '/') { st.y = strtol(s + 1, (char **) &s, 10); }
        while (*s && *s != ';' && *s != '+') s++;
        if (*s == ';' || *s == '+') s++;
        sScript.push_back(st);
    }
}

static bool want_shot(uint32_t frame)
{
    const char *s = g_fgConfig.shots;
    while (s && *s) {
        if (strtoul(s, (char **) &s, 10) == frame) return true;
        while (*s == ',' || *s == ' ') s++;
    }
    return false;
}

static void dump_frame(const SDL_Renderer *r, uint32_t frame)
{
    char name[64];
    snprintf(name, sizeof(name), "PROGDIR:shot_%05lu.raw", (unsigned long) frame);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fwrite(r->back, 2, (size_t) r->w * r->h, f);
    fclose(f);
    FG_LOG("[shot] %s\n", name);
}

/* ---- start-up: progress bar while all assets are loaded */
struct FgProgressCtx { SDL_Renderer *r; SDL_Window *d; int lastW; };

static void preload_progress(void *ctx, int done, int total)
{
    FgProgressCtx *c = (FgProgressCtx *) ctx;
    const int BW = 400, BH = 12;
    const int x0 = (c->r->w - BW) / 2, y0 = (c->r->h - BH) / 2;
    int w = total > 0 ? (BW - 4) * done / total : 0;
    if (w == c->lastW && done < total) return;
    c->lastW = w;
    for (int y = 0; y < BH; y++) {
        uint16_t *row = c->r->back + (y0 + y) * c->r->w + x0;
        for (int x = 0; x < BW; x++) {
            bool frame = y == 0 || y == BH - 1 || x == 0 || x == BW - 1;
            bool fill = x >= 2 && x < 2 + w && y >= 2 && y < BH - 2;
            row[x] = frame ? 0x52BF : (fill ? 0xE41F : 0x0000);    /* (blue frame, magenta bar) */
        }
    }
    std::vector<FgRect> rc(1);
    rc[0].x0 = x0; rc[0].y0 = y0; rc[0].x1 = x0 + BW; rc[0].y1 = y0 + BH;
    fg_display_present(c->d, c->r->back, c->r->w, rc);
}

GoSDL::Window::Window(unsigned width, unsigned height, std::string caption, Uint32 updateInterval)
    : mUpdateInterval(updateInterval)
{
    if (!fg_timer_open())
        throw std::runtime_error("Cannot open timer.device.");

    mRenderer = fg_renderer_new(width, height);
    if (!mRenderer)
        throw std::runtime_error("Not enough memory for the frame buffer.");

    mOptions.loadResources();
    bool fullscreen = mOptions.getFullscreenEnabled();
    if (g_fgConfig.forceDisplay >= 0) {
        fullscreen = g_fgConfig.forceDisplay != 0;
        /* (the tool type / argument also sets the option, so the Options menu shows it) */
        if (fullscreen != mOptions.getFullscreenEnabled()) mOptions.setFullscreenEnabled(fullscreen);
    }

    const char *err = NULL;
    mWindow = fg_display_open(width, height, fullscreen, caption.c_str(), &err);
    if (!mWindow) {
        /* the other way round before giving up (e.g. a Workbench in 8 bits) */
        const char *err2 = NULL;
        FG_LOG("[video] %s\n", err);
        mWindow = fg_display_open(width, height, !fullscreen, caption.c_str(), &err2);
        if (!mWindow) throw std::runtime_error(err);
    }

    parse_script(g_fgConfig.script);
    g_cycOn = g_fgConfig.frameStats && g_ammx;   /* (the 080's cycle counter: measuring runs only) */
    fg_audio_open(g_fgConfig.audioMode, g_fgConfig.ahiUnit);

    /* everything the game will need, now: no disk access while it runs */
    FgProgressCtx pc = { mRenderer, mWindow, -1 };
    fg_preload("PROGDIR:", preload_progress, &pc);
    mRenderer->fullDirty = true;
    fg_pad_open();
}

GoSDL::Window::~Window()
{
    for (size_t i = 0; i < sStatLines.size(); i++) fg_info("%s", sStatLines[i].c_str());
    sStatLines.clear();
    fg_pad_close();
    fg_audio_close();
    fg_display_close(mWindow);
    mWindow = NULL;
    fg_renderer_free(mRenderer);
    mRenderer = NULL;
    fg_timer_close();
}

void GoSDL::Window::show()
{
    mShouldRun = true;
    mApplicationActive = true;
    while (mShouldRun)
        tick();
}

void GoSDL::Window::tick()
{
    FgEvent e;
    while (mWindow && fg_display_poll(mWindow, &e)) {
        /* test runs with SCRIPT: only the script's input (reproducible frames) */
        if (g_fgConfig.script && e.type != FG_EV_QUIT && e.type != FG_EV_NONE) continue;
        switch (e.type) {
        case FG_EV_QUIT:
            close();
            break;
        case FG_EV_NONE:
            if (e.button == -1) mRenderer->fullDirty = true;
            break;
        case FG_EV_KEYDOWN:
            mMouseActive = false;
            buttonDown(e.key);
            break;
        case FG_EV_KEYUP:
            buttonUp(e.key);
            break;
        case FG_EV_MOTION:
            mMouseActive = true;
            mMouseX = e.x;
            mMouseY = e.y;
            break;
        case FG_EV_BUTTONDOWN:
            mMouseActive = true;
            mMouseX = e.x;
            mMouseY = e.y;
            mouseButtonDown((Uint8) e.button);
            break;
        case FG_EV_BUTTONUP:
            mouseButtonUp((Uint8) e.button);
            break;
        }
    }

    while (sScriptPos < sScript.size() && sScript[sScriptPos].frame <= sFrame) {
        const FgScriptStep &st = sScript[sScriptPos++];
        static const struct { const char *name; SDL_Keycode key; } keys[] = {
            { "UP", SDLK_UP }, { "DOWN", SDLK_DOWN }, { "LEFT", SDLK_LEFT }, { "RIGHT", SDLK_RIGHT },
            { "SPACE", SDLK_SPACE }, { "RETURN", SDLK_RETURN }, { "ESC", SDLK_ESCAPE }, { "H", SDLK_H },
        };
        FG_LOG("[script] %lu %s %ld %ld\n", (unsigned long) sFrame, st.action, (long) st.x, (long) st.y);
        for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
            if (!strcmp(st.action, keys[k].name)) {
                mMouseActive = false;
                buttonDown(keys[k].key);
                buttonUp(keys[k].key);
            }
        if (!strcmp(st.action, "MOVE") || !strcmp(st.action, "CLICK")) {
            mMouseActive = true;
            mMouseX = st.x;
            mMouseY = st.y;
        }
        if (!strcmp(st.action, "CLICK")) {
            mouseButtonDown(SDL_BUTTON_LEFT);
            mouseButtonUp(SDL_BUTTON_LEFT);
        }
        if (!strcmp(st.action, "FULL")) setFullscreen(true);
        if (!strcmp(st.action, "WIN")) setFullscreen(false);
        if (!strcmp(st.action, "QUIT")) close();
    }

    int pad[8];
    int n = fg_pad_poll(pad, 8);
    for (int i = 0; i < n; i++) {
        mMouseActive = false;
        controllerButtonDown((Uint8) pad[i]);
    }

    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        close();

    if (!mShouldRun) return;

    /* Frame pacing against a fixed schedule (one step every mUpdateInterval ms,
     * as the original's 17 ms). When a frame took longer, the game logic
     * catches up (up to 4 steps, only the last one is drawn): animations keep
     * their real speed. Not with SCRIPT (test runs stay frame-exact). */
    static uint32_t next = 0;
    const uint32_t interval = mUpdateInterval * 1000;
    uint32_t now = fg_micros();
    if (!next) next = now;
    int steps = 1;
    if (!g_fgConfig.script && (int32_t) (now - next) > (int32_t) interval) {
        steps = 1 + (int) ((now - next) / interval);
        if (steps > 4) steps = 4;
    }
    gameLoop(steps);
    next += interval * steps;
    now = fg_micros();
    if ((int32_t) (now - next) > (int32_t) (3 * interval))
        next = now;                                   /* (far behind: start again from now) */
    else if ((int32_t) (next - now) > 0)
        fg_delay_us(next - now);
}

void GoSDL::Window::gameLoop(int steps)
{
    uint32_t &frame = sFrame;
    static uint32_t tUpdate = 0, tCompose = 0, tPresent = 0, nRects = 0;
    uint32_t t0 = fg_micros();

    /* catch-up steps: logic and animation only (upstream animates in draw()) */
    for (int i = 1; i < steps; i++) {
        update();
        draw();
        mDrawingQueue.clear();
    }
    update();
    draw();

    uint32_t t1 = fg_micros();

    static std::vector<FgOp> ops;      /* (kept: no allocation per frame) */
    ops.clear();
    mDrawingQueue.sortByDepth();
    for (GoSDL::DrawingQueueIterator it = mDrawingQueue.begin(); it != mDrawingQueue.end(); ++it) {
        const DrawingQueueOperation &q = it->second;
        if (!q.mTexture) continue;
        FgOp op;
        op.tex = q.mTexture;
        op.texId = q.mTexture->id;
        op.x = (int16_t) q.mDstRect.x;
        op.y = (int16_t) q.mDstRect.y;
        op.w = (int16_t) (q.mDstRect.w + 0.5f);
        op.h = (int16_t) (q.mDstRect.h + 0.5f);
        op.alpha = q.mAlpha;
        if (op.alpha == 0) continue;
        if (op.w <= 0 || op.h <= 0) continue;
        fg_op_prepare(op);
        op.r = q.mColor.r;
        op.g = q.mColor.g;
        op.b = q.mColor.b;
        if (op.w > 0 && op.h > 0) ops.push_back(op);
    }
    mDrawingQueue.clear();

    if (g_fgConfig.fullRedraw) mRenderer->fullDirty = true;
    fg_renderer_compose(mRenderer, ops);
    uint32_t t2 = fg_micros();
    if (mWindow)
        fg_display_present(mWindow, mRenderer->back, mRenderer->w, mRenderer->rects);
    uint32_t t3 = fg_micros();

    frame++;
    if (want_shot(frame)) dump_frame(mRenderer, frame);
    if (g_fgConfig.frameStats) {
        tUpdate += t1 - t0;
        tCompose += t2 - t1;
        tPresent += t3 - t2;
        nRects += mRenderer->rects.size();
        static uint32_t tMax = 0;
        if (t3 - t0 > tMax) tMax = t3 - t0;
        const uint32_t N = g_fgConfig.statN > 0 ? g_fgConfig.statN : 20;
        if ((frame % N) == 0) {
            /* collected in memory, written to the log at the end (no file access while measuring) */
            char line[200];
            snprintf(line, sizeof(line), "[frame] %lu: update %lu us (text %lu), compose %lu us (layer %lu, rects %lu), present %lu us, max %lu us, %lu rects, %lu ops, layer b%lu g%lu r%lu (n %lu)\n",
                     (unsigned long) frame, (unsigned long) tUpdate / N, (unsigned long) g_tFont / N, (unsigned long) tCompose / N,
                     (unsigned long) g_tLayer / N, (unsigned long) g_tRects / N,
                     (unsigned long) tPresent / N, (unsigned long) tMax, (unsigned long) nRects / N, (unsigned long) ops.size(),
                     (unsigned long) g_layerBuilds, (unsigned long) g_layerGrows, (unsigned long) g_layerRepairs,
                     (unsigned long) mRenderer->layerN);
            g_tFont = g_tLayer = g_tRects = 0;
            sStatLines.push_back(line);
            snprintf(line, sizeof(line), "[px] %lu: scaled rows %lu, walked %lu, visible %lu, unscaled %lu\n",
                     (unsigned long) frame, (unsigned long) g_scaledRows / N, (unsigned long) g_pxScaledWalk / N,
                     (unsigned long) g_pxScaledVis / N, (unsigned long) g_pxRuns / N);
            sStatLines.push_back(line);
            g_scaledRows = g_pxScaledWalk = g_pxScaledVis = g_pxRuns = 0;
            if (g_cycOn) {
                /* (thousands of cycles per frame: 85 per microsecond on the V1200) */
                snprintf(line, sizeof(line), "[cyc] %lu: kcycles spans %lu apply %lu scaled-all %lu runs %lu copy %lu rects-all %lu layer %lu diff %lu, %lu spans\n",
                         (unsigned long) frame, (unsigned long) g_cyc[0] / N / 1000, (unsigned long) g_cyc[1] / N / 1000,
                         (unsigned long) g_cyc[2] / N / 1000, (unsigned long) g_cyc[3] / N / 1000, (unsigned long) g_cyc[4] / N / 1000,
                         (unsigned long) g_cyc[5] / N / 1000, (unsigned long) g_cyc[6] / N / 1000, (unsigned long) g_cyc[7] / N / 1000,
                         (unsigned long) g_nSpans / N);
                sStatLines.push_back(line);
                for (int k = 0; k < 8; k++) g_cyc[k] = 0;
                g_nSpans = 0;
            }
            g_layerBuilds = g_layerGrows = g_layerRepairs = 0;
            tUpdate = tCompose = tPresent = nRects = tMax = 0;
        }
    }
    if (frame == 1) FG_LOG("[OK] first frame\n");
    if (g_fgConfig.frameStats && (frame == 2 || frame == 300)) fg_display_debug(mWindow);
    if (g_fgConfig.maxFrames > 0 && (int) frame >= g_fgConfig.maxFrames) {
        FG_LOG("[OK] frame %lu\n", (unsigned long) frame);
        close();
    }
}

void GoSDL::Window::close()
{
    mShouldRun = false;
}

void GoSDL::Window::enqueueDraw(SDL_Texture *texture, SDL_FRect destRect, double angle, float z, Uint8 alpha, SDL_Color color)
{
    GoSDL::DrawingQueueOperation op;
    op.mTexture = texture;
    op.mDstRect = destRect;
    op.mAngle = angle;          /* (rotation is not used by the game) */
    op.mAlpha = alpha;
    op.mColor = color;
    mDrawingQueue.draw(z, op);
}

void GoSDL::Window::openGameController(Sint32) { }
void GoSDL::Window::closeDisconnectedGameControllers() { }
void GoSDL::Window::closeAllGameControllers() { }

void GoSDL::Window::setFullscreen(bool value)
{
    fg_icon_set_fullscreen(value);       /* (the icon's FULLSCREEN tool type follows) */
    if (mWindow && mWindow->fullscreen == value) return;

    const char *err = NULL;
    fg_display_close(mWindow);
    mWindow = fg_display_open(mRenderer->w, mRenderer->h, value, "Freegemas", &err);
    if (!mWindow) {
        const char *err2 = NULL;
        mWindow = fg_display_open(mRenderer->w, mRenderer->h, !value, "Freegemas", &err2);
        if (err) fg_message("Freegemas", err);
        if (!mWindow) {
            close();
            return;
        }
    }
    mRenderer->fullDirty = true;
    mMouseActive = false;
}
