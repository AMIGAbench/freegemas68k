/* Audio of the AmigaOS port.
 *
 * A software mixer (8 voices, 8-bit signed mono, 22050 Hz) runs in its own
 * process and feeds one of two outputs:
 *   - AHI:   ahi.device, double-buffered CMD_WRITEs linked with ahir_Link
 *   - Paula: audio.device, a left and a right channel from chip RAM buffers
 * The samples (.snd) are raw 8-bit signed mono PCM made by
 * tools/convert_assets.py from the original OGG files.
 * GoSDL::Sound and GoSDL::Music are thin wrappers around the voices. */

#include "amiga_internal.h"
#include "go_sound.h"
#include "go_music.h"
#include "Util.h"

#include <exec/exec.h>
#include <exec/semaphores.h>
#include <dos/dostags.h>
#include <devices/ahi.h>
#include <devices/audio.h>
#include <graphics/gfxbase.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#define MIX_RATE    22050
#define MIX_VOICES  8
#define BUF_SAMPLES 1024          /* 46 ms per buffer */
#define PAULA_BUFS  3

extern "C" {
void fg_k_mix_add(int16_t *acc, const int8_t *src, int n, int vol);
void fg_k_mix_out(int8_t *out, const int16_t *acc, int n);
}

struct MIX_Audio {
    int8_t  *data;
    uint32_t len;
};

struct MIX_Track {
    int voice;
};

struct Voice {
    const MIX_Audio *audio;
    uint32_t pos;
    int      vol;          /* 0..256, current */
    int      target;       /* 0..256 */
    int      step;         /* change per buffer */
    bool     loop;
    bool     stopAtZero;   /* fade-out: stop when the volume reaches 0 */
    bool     active;
};

static struct SignalSemaphore sMixLock;
static Voice sVoices[MIX_VOICES];
static bool  sVoiceUsed[MIX_VOICES];
static int   sMode = FG_AUDIO_OFF;      /* the output that runs */

static struct Process *sProc = NULL;
static struct Task *sParent = NULL;
static volatile int sProcResult = 0;   /* 1 running, -1 failed */
static volatile bool sQuit = false;
static volatile int sAudioErr = 0;      /* set by the audio process, logged by the main task */
static int sWantMode = FG_AUDIO_OFF;
static int sAhiUnit = 0;

/* ================================================================ mixer */

static void mix(int8_t *out, int n)
{
    static int16_t acc[BUF_SAMPLES];
    memset(acc, 0, n * sizeof(int16_t));

    ObtainSemaphore(&sMixLock);
    for (int v = 0; v < MIX_VOICES; v++) {
        Voice &vo = sVoices[v];
        if (!vo.active) continue;

        /* volume ramp: one step per buffer */
        if (vo.vol != vo.target) {
            vo.vol += vo.step;
            if ((vo.step > 0 && vo.vol > vo.target) || (vo.step < 0 && vo.vol < vo.target) || vo.step == 0)
                vo.vol = vo.target;
        }
        int vol = vo.vol;
        const int8_t *src = vo.audio->data;
        uint32_t len = vo.audio->len, pos = vo.pos;
        int i = 0;
        while (i < n) {
            if (pos >= len) {
                if (!vo.loop) { vo.active = false; break; }
                pos = 0;
            }
            uint32_t chunk = len - pos;
            if (chunk > (uint32_t) (n - i)) chunk = n - i;
            const int8_t *s = src + pos;
            int16_t *a = acc + i;
            if (g_kMix) {
                fg_k_mix_add(a, s, (int) chunk, vol);                    /* (kern68k.s) */
            } else if (vol == 256) {
                for (uint32_t k = 0; k < chunk; k++) a[k] += s[k];
            } else {
                for (uint32_t k = 0; k < chunk; k++) a[k] += (int16_t) ((s[k] * vol) >> 8);
            }
            i += chunk;
            pos += chunk;
        }
        vo.pos = pos;
        if (vo.stopAtZero && vo.vol == 0) vo.active = false;
    }
    ReleaseSemaphore(&sMixLock);

    if (g_kMix) {
        fg_k_mix_out(out, acc, n);
        return;
    }
    for (int i = 0; i < n; i++) {
        int s = acc[i];
        out[i] = (int8_t) (s > 127 ? 127 : (s < -128 ? -128 : s));
    }
}

/* the mix as interleaved stereo (left = right) */
static void mix_stereo(int8_t *out, int n)
{
    static int8_t mono[BUF_SAMPLES];
    mix(mono, n);
    for (int i = n - 1; i >= 0; i--) out[2 * i] = out[2 * i + 1] = mono[i];
}

/* ================================================================ AHI output */

static bool run_ahi()
{
    struct MsgPort *port = CreateMsgPort();
    struct AHIRequest *req[2] = { NULL, NULL };
    int8_t *buf[2] = { NULL, NULL };
    bool pending[2] = { false, false };
    bool opened = false, ok = false;

    if (!port) goto out;
    req[0] = (struct AHIRequest *) CreateIORequest(port, sizeof(struct AHIRequest));
    if (!req[0]) goto out;
    req[0]->ahir_Version = 4;
    if (OpenDevice((CONST_STRPTR) AHINAME, sAhiUnit, (struct IORequest *) req[0], 0)) goto out;
    opened = true;
    req[1] = (struct AHIRequest *) AllocMem(sizeof(struct AHIRequest), MEMF_PUBLIC);
    /* stereo (the same mix left and right): a centred mono sound gets only half
       the amplitude per channel from AHI */
    buf[0] = (int8_t *) AllocMem(BUF_SAMPLES * 2, MEMF_PUBLIC);
    buf[1] = (int8_t *) AllocMem(BUF_SAMPLES * 2, MEMF_PUBLIC);
    if (!req[1] || !buf[0] || !buf[1]) goto out;
    CopyMem(req[0], req[1], sizeof(struct AHIRequest));

    /* a test write of silence: a unit without a working mode fails at once */
    memset(buf[0], 0, BUF_SAMPLES * 2);
    req[0]->ahir_Std.io_Command = CMD_WRITE;
    req[0]->ahir_Std.io_Data = buf[0];
    req[0]->ahir_Std.io_Length = 256;
    req[0]->ahir_Std.io_Offset = 0;
    req[0]->ahir_Frequency = MIX_RATE;
    req[0]->ahir_Type = AHIST_S8S;
    req[0]->ahir_Volume = 0x10000;
    req[0]->ahir_Position = 0x8000;
    req[0]->ahir_Link = NULL;
    if (DoIO((struct IORequest *) req[0])) {
        sAudioErr = 1;                                 /* (logged by the main task) */
        goto out;
    }

    sProcResult = 1;
    Signal(sParent, SIGBREAKF_CTRL_F);
    ok = true;
    {
        struct AHIRequest *link = NULL;
        int cur = 0;
        while (!sQuit) {
            mix_stereo(buf[cur], BUF_SAMPLES);
            struct AHIRequest *r = req[cur];
            r->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
            r->ahir_Std.io_Command = CMD_WRITE;
            r->ahir_Std.io_Data = buf[cur];
            r->ahir_Std.io_Length = BUF_SAMPLES * 2;
            r->ahir_Std.io_Offset = 0;
            r->ahir_Frequency = MIX_RATE;
            r->ahir_Type = AHIST_S8S;
            r->ahir_Volume = 0x10000;
            r->ahir_Position = 0x8000;
            r->ahir_Link = link;
            SendIO((struct IORequest *) r);
            pending[cur] = true;
            if (link) {
                /* the previous buffer has to finish before it is filled again */
                while (!CheckIO((struct IORequest *) link)) {
                    ULONG sig = Wait((1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C);
                    if (sig & SIGBREAKF_CTRL_C) sQuit = true;
                    if (sQuit) break;
                }
                if (sQuit) break;
                WaitIO((struct IORequest *) link);
                pending[cur ^ 1] = false;
                if (link->ahir_Std.io_Error) {
                    /* never spin at priority 20: the game would starve */
                    sAudioErr = 2;
                    break;
                }
            }
            link = r;
            cur ^= 1;
        }
        for (int i = 0; i < 2; i++) {
            if (!pending[i]) continue;
            if (!CheckIO((struct IORequest *) req[i])) AbortIO((struct IORequest *) req[i]);
            WaitIO((struct IORequest *) req[i]);
        }
    }
out:
    if (opened) CloseDevice((struct IORequest *) req[0]);
    if (req[1]) FreeMem(req[1], sizeof(struct AHIRequest));
    if (req[0]) DeleteIORequest((struct IORequest *) req[0]);
    for (int i = 0; i < 2; i++) if (buf[i]) FreeMem(buf[i], BUF_SAMPLES * 2);
    if (port) DeleteMsgPort(port);
    return ok;
}

/* ================================================================ Paula output */

static bool run_paula()
{
    static UBYTE allocMap[] = { 3, 5, 10, 12 };      /* a left and a right channel */
    struct MsgPort *port = CreateMsgPort();
    struct IOAudio *alloc = NULL;
    struct IOAudio *io[2][PAULA_BUFS];
    int8_t *buf[PAULA_BUFS];
    bool pending[PAULA_BUFS];
    bool opened = false, ok = false;
    memset(io, 0, sizeof(io));
    memset(pending, 0, sizeof(pending));
    memset(buf, 0, sizeof(buf));

    if (!port) goto out;
    alloc = (struct IOAudio *) CreateIORequest(port, sizeof(struct IOAudio));
    if (!alloc) goto out;
    alloc->ioa_Request.io_Message.mn_Node.ln_Pri = 0;
    alloc->ioa_Request.io_Command = ADCMD_ALLOCATE;
    alloc->ioa_Request.io_Flags = ADIOF_NOWAIT;
    alloc->ioa_Data = allocMap;
    alloc->ioa_Length = sizeof(allocMap);
    if (OpenDevice((CONST_STRPTR) AUDIONAME, 0, (struct IORequest *) alloc, 0)) goto out;
    opened = true;
    {
        ULONG mask = (ULONG) alloc->ioa_Request.io_Unit;
        ULONG left = mask & 9, right = mask & 6;           /* channels 0/3 left, 1/2 right */
        if (!left || !right) goto out;

        extern struct GfxBase *GfxBase;
        ULONG clock = (GfxBase && (GfxBase->DisplayFlags & PAL)) ? 3546895UL : 3579545UL;
        UWORD period = (UWORD) ((clock + MIX_RATE / 2) / MIX_RATE);

        for (int b = 0; b < PAULA_BUFS; b++) {
            buf[b] = (int8_t *) AllocMem(BUF_SAMPLES, MEMF_CHIP | MEMF_CLEAR);
            if (!buf[b]) goto out;
            for (int c = 0; c < 2; c++) {
                io[c][b] = (struct IOAudio *) AllocMem(sizeof(struct IOAudio), MEMF_PUBLIC | MEMF_CLEAR);
                if (!io[c][b]) goto out;
                CopyMem(alloc, io[c][b], sizeof(struct IOAudio));
                io[c][b]->ioa_Request.io_Unit = (struct Unit *) (c ? right : left);
                io[c][b]->ioa_Request.io_Command = CMD_WRITE;
                io[c][b]->ioa_Request.io_Flags = ADIOF_PERVOL;
                io[c][b]->ioa_Data = (UBYTE *) buf[b];
                io[c][b]->ioa_Length = BUF_SAMPLES;
                io[c][b]->ioa_Period = period;
                io[c][b]->ioa_Volume = 64;
                io[c][b]->ioa_Cycles = 1;
            }
        }

        sProcResult = 1;
        Signal(sParent, SIGBREAKF_CTRL_F);
        ok = true;

        /* fill and queue all buffers, then refill each one as it comes back */
        for (int b = 0; b < PAULA_BUFS; b++) {
            mix(buf[b], BUF_SAMPLES);
            Forbid();
            BeginIO((struct IORequest *) io[0][b]);
            BeginIO((struct IORequest *) io[1][b]);
            Permit();
            pending[b] = true;
        }
        int b = 0;
        while (!sQuit) {
            while (!CheckIO((struct IORequest *) io[0][b]) || !CheckIO((struct IORequest *) io[1][b])) {
                ULONG sig = Wait((1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C);
                if (sig & SIGBREAKF_CTRL_C) sQuit = true;
                if (sQuit) break;
            }
            if (sQuit) break;
            WaitIO((struct IORequest *) io[0][b]);
            WaitIO((struct IORequest *) io[1][b]);
            pending[b] = false;
            if (io[0][b]->ioa_Request.io_Error || io[1][b]->ioa_Request.io_Error) {
                sAudioErr = 3;
                break;
            }
            mix(buf[b], BUF_SAMPLES);
            Forbid();
            BeginIO((struct IORequest *) io[0][b]);
            BeginIO((struct IORequest *) io[1][b]);
            Permit();
            pending[b] = true;
            b = (b + 1) % PAULA_BUFS;
        }
        for (int c = 0; c < 2; c++)
            for (int k = 0; k < PAULA_BUFS; k++) {
                if (!pending[k]) continue;
                if (!CheckIO((struct IORequest *) io[c][k])) AbortIO((struct IORequest *) io[c][k]);
                WaitIO((struct IORequest *) io[c][k]);
            }
    }
out:
    if (opened) CloseDevice((struct IORequest *) alloc);   /* frees the channels */
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < PAULA_BUFS; k++)
            if (io[c][k]) FreeMem(io[c][k], sizeof(struct IOAudio));
    for (int k = 0; k < PAULA_BUFS; k++) if (buf[k]) FreeMem(buf[k], BUF_SAMPLES);
    if (alloc) DeleteIORequest((struct IORequest *) alloc);
    if (port) DeleteMsgPort(port);
    return ok;
}

/* ================================================================ the process */

static void audio_proc()
{
    bool ok = sWantMode == FG_AUDIO_AHI ? run_ahi() : run_paula();
    Forbid();
    if (!ok) sProcResult = -1;
    sProc = NULL;
    Signal(sParent, SIGBREAKF_CTRL_F);
    /* (exits under Forbid: the parent cannot unload the code before) */
}

static bool start_output(int mode)
{
    sWantMode = mode;
    sQuit = false;
    sProcResult = 0;
    SetSignal(0, SIGBREAKF_CTRL_F);
    sProc = CreateNewProcTags(NP_Entry, (ULONG) audio_proc,
                              NP_Name, (ULONG) "Freegemas audio",
                              NP_Priority, 20,
                              NP_StackSize, 16384,
                              TAG_DONE);
    if (!sProc) return false;
    while (sProcResult == 0) Wait(SIGBREAKF_CTRL_F);
    if (sProcResult < 0) {
        while (sProc) Wait(SIGBREAKF_CTRL_F);   /* (already gone) */
        return false;
    }
    sMode = mode;
    return true;
}

bool fg_audio_open(int mode, int ahiUnit)
{
    static bool init = false;
    if (!init) {
        InitSemaphore(&sMixLock);
        memset(sVoices, 0, sizeof(sVoices));
        init = true;
    }
    sParent = FindTask(NULL);
    sAhiUnit = ahiUnit;
    if (mode == FG_AUDIO_OFF) return false;

    bool ok = false;
    if (mode == FG_AUDIO_AHI || mode == FG_AUDIO_AUTO) ok = start_output(FG_AUDIO_AHI);
    if (!ok && (mode == FG_AUDIO_PAULA || mode == FG_AUDIO_AUTO)) ok = start_output(FG_AUDIO_PAULA);
    if (sAudioErr == 1) fg_info("[audio] AHI unit %ld does not play (write error)\n", (long) sAhiUnit);
    sAudioErr = 0;
    fg_info("[audio] %s\n", !ok ? "off" : (sMode == FG_AUDIO_AHI ? "AHI" : "Paula"));
    return ok;
}

void fg_audio_close()
{
    if (sAudioErr) {
        fg_info("[audio] %s write error during play: sound was off\n", sAudioErr == 3 ? "audio.device" : "AHI");
        sAudioErr = 0;
    }
    if (!sProc) return;
    sQuit = true;
    Forbid();
    struct Process *p = sProc;
    if (p) Signal((struct Task *) p, SIGBREAKF_CTRL_C);
    Permit();
    while (sProc) Wait(SIGBREAKF_CTRL_F);
    sMode = FG_AUDIO_OFF;
}

/* ================================================================ voices */

/* all samples are loaded once (fg_audio_preload at start-up) and kept */
static std::map<std::string, MIX_Audio *> sAudioCache;

static std::string snd_path(const std::string &path)
{
    size_t dot = path.rfind('.');
    if (dot != std::string::npos && path.compare(dot, std::string::npos, ".ogg") == 0)
        return path.substr(0, dot) + ".snd";
    return path;
}

static MIX_Audio *load_audio(const std::string &path)
{
    std::string p = snd_path(path);
    std::map<std::string, MIX_Audio *>::iterator it = sAudioCache.find(p);
    if (it != sAudioCache.end()) return it->second;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) {
        FG_LOG("[audio] cannot open %s\n", p.c_str());
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    MIX_Audio *a = (MIX_Audio *) malloc(sizeof(MIX_Audio));
    if (a) a->data = len > 0 ? (int8_t *) malloc(len) : NULL;
    if (!a || !a->data || fread(a->data, 1, len, f) != (size_t) len) {
        if (a) free(a->data);
        free(a);
        a = NULL;
    } else {
        a->len = (uint32_t) len;
        sAudioCache[p] = a;
    }
    fclose(f);
    return a;
}

bool fg_audio_preload(const std::string &path)
{
    return load_audio(path) != NULL;
}

/* (cached: freed only at the end, after the output has stopped) */
static void free_audio(MIX_Audio *) { }

void fg_audio_free_cache()
{
    for (std::map<std::string, MIX_Audio *>::iterator it = sAudioCache.begin(); it != sAudioCache.end(); ++it) {
        free(it->second->data);
        free(it->second);
    }
    sAudioCache.clear();
}

static MIX_Track *new_track()
{
    for (int i = 0; i < MIX_VOICES; i++)
        if (!sVoiceUsed[i]) {
            sVoiceUsed[i] = true;
            MIX_Track *t = new MIX_Track;
            t->voice = i;
            return t;
        }
    return NULL;
}

static void stop_voice(int v)
{
    if (!sProc) { sVoices[v].active = false; return; }
    ObtainSemaphore(&sMixLock);
    sVoices[v].active = false;
    ReleaseSemaphore(&sMixLock);
}

static void free_track(MIX_Track *t)
{
    if (!t) return;
    stop_voice(t->voice);
    sVoiceUsed[t->voice] = false;
    delete t;
}

/* fadeBuffers: buffers (46 ms each) for the ramp from 0, 0 = at once */
static void start_voice(int v, const MIX_Audio *a, float gain, bool loop, int fadeBuffers)
{
    if (!sProc || !a) return;
    int target = (int) (gain * 256.0f + 0.5f);
    if (target > 256) target = 256;
    if (target < 0) target = 0;
    ObtainSemaphore(&sMixLock);
    Voice &vo = sVoices[v];
    vo.audio = a;
    vo.pos = 0;
    vo.loop = loop;
    vo.stopAtZero = false;
    vo.target = target;
    if (fadeBuffers > 0) {
        vo.vol = 0;
        vo.step = (target + fadeBuffers - 1) / fadeBuffers;
    } else {
        vo.vol = target;
        vo.step = 0;
    }
    vo.active = true;
    ReleaseSemaphore(&sMixLock);
}

/* ------------------------------------------------------------------ GoSDL::Sound */

GoSDL::Sound::~Sound() { unload(); }

void GoSDL::Sound::setWindow(Window *parentWindow) { mParentWindow = parentWindow; }

void GoSDL::Sound::setSample(std::string path)
{
    unload();
    mSample = load_audio(getBasePath() + path);
    if (mSample) mSample_track = new_track();
}

void GoSDL::Sound::unload()
{
    free_track(mSample_track);
    mSample_track = nullptr;
    free_audio(mSample);
    mSample = nullptr;
}

void GoSDL::Sound::play(float vol)
{
    if (mSample_track && mSample)
        start_voice(mSample_track->voice, mSample, vol, false, 0);
}

/* ------------------------------------------------------------------ GoSDL::Music */

GoSDL::Music::~Music()
{
    free_track(mSample_track);
    mSample_track = nullptr;
    free_audio(mSample);
    mSample = nullptr;
}

void GoSDL::Music::setWindow(Window *parentWindow) { mParentWindow = parentWindow; }

void GoSDL::Music::setSample(std::string path)
{
    free_track(mSample_track);
    mSample_track = nullptr;
    free_audio(mSample);
    mSample = load_audio(getBasePath() + path);
    if (mSample) mSample_track = new_track();
}

void GoSDL::Music::play(float vol)
{
    if (mSample && mSample_track)
        start_voice(mSample_track->voice, mSample, vol, true, 4);   /* 200 ms fade-in */
}

void GoSDL::Music::stop()
{
    if (!mSample_track || !sProc) return;
    ObtainSemaphore(&sMixLock);
    Voice &vo = sVoices[mSample_track->voice];
    if (vo.active) {
        vo.target = 0;
        vo.step = -((vo.vol + 3) / 4);    /* 200 ms fade-out */
        if (vo.step == 0) vo.step = -1;
        vo.stopAtZero = true;
    }
    ReleaseSemaphore(&sMixLock);
}

bool GoSDL::Music::isPlaying()
{
    if (!mSample_track) return false;
    return sVoices[mSample_track->voice].active;
}
