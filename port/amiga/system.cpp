/* AmigaOS system glue: timer, paths, options file, start-up (CLI and
 * Workbench) and the replacement of main.cpp / Util.cpp / OptionsManager.cpp. */

#include "amiga_internal.h"
#include "Game.h"
#include "OptionsManager.h"
#include "Util.h"
#include "log.h"

#include <exec/execbase.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <proto/timer.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>

/* the C++ runtime, FreeType and the game's recursion want more than 4 KB */
extern "C" { unsigned long __stack = 262144; }

static const char VERSTAG[] __attribute__((used)) = "$VER: Freegemas 1.0 (19.11.2026) AmigaOS RTG port";

FgConfig g_fgConfig = { -1, FG_AUDIO_AUTO, 0, 0, 0, NULL, NULL, 0, 0, 0, 20, 0, 0, 0, 0, 0 };

/* ================================================================ timer */

struct Device *TimerBase = NULL;
static struct MsgPort *sTimerPort = NULL;
static struct timerequest *sTimerReq = NULL;
static struct timeval sTimeStart;

bool fg_timer_open()
{
    if (TimerBase) return true;
    sTimerPort = CreateMsgPort();
    if (!sTimerPort) return false;
    sTimerReq = (struct timerequest *) CreateIORequest(sTimerPort, sizeof(struct timerequest));
    if (!sTimerReq || OpenDevice((CONST_STRPTR) TIMERNAME, UNIT_MICROHZ, (struct IORequest *) sTimerReq, 0)) {
        if (sTimerReq) DeleteIORequest((struct IORequest *) sTimerReq);
        DeleteMsgPort(sTimerPort);
        sTimerReq = NULL;
        sTimerPort = NULL;
        return false;
    }
    TimerBase = sTimerReq->tr_node.io_Device;
    GetSysTime(&sTimeStart);
    return true;
}

void fg_timer_close()
{
    if (!TimerBase) return;
    CloseDevice((struct IORequest *) sTimerReq);
    DeleteIORequest((struct IORequest *) sTimerReq);
    DeleteMsgPort(sTimerPort);
    TimerBase = NULL;
    sTimerReq = NULL;
    sTimerPort = NULL;
}

uint32_t fg_micros()
{
    struct timeval tv;
    if (!TimerBase) return 0;
    GetSysTime(&tv);
    return (uint32_t) ((tv.tv_secs - sTimeStart.tv_secs) * 1000000UL + tv.tv_micro - sTimeStart.tv_micro);
}

extern "C" Uint64 SDL_GetTicks(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return (Uint64) (tv.tv_secs - sTimeStart.tv_secs) * 1000 +
           ((long) tv.tv_micro - (long) sTimeStart.tv_micro) / 1000;
}

extern "C" void SDL_Delay(Uint32 ms)
{
    if (!sTimerReq || !ms) return;
    sTimerReq->tr_node.io_Command = TR_ADDREQUEST;
    sTimerReq->tr_time.tv_secs = ms / 1000;
    sTimerReq->tr_time.tv_micro = (ms % 1000) * 1000;
    DoIO((struct IORequest *) sTimerReq);
}

void fg_info(const char *fmt, ...)
{
    if (!g_fgConfig.log) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* opened and closed per line: readable while the game runs */
    static bool fresh = true;
    FILE *f = fopen("PROGDIR:freegemas.log", fresh ? "w" : "a");
    fresh = false;
    if (f) { fputs(buf, f); fclose(f); }
}

void fg_delay_us(uint32_t us)
{
    if (!sTimerReq || !us) return;
    sTimerReq->tr_node.io_Command = TR_ADDREQUEST;
    sTimerReq->tr_time.tv_secs = us / 1000000;
    sTimerReq->tr_time.tv_micro = us % 1000000;
    DoIO((struct IORequest *) sTimerReq);
}

extern "C" void SDL_Log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fg_info("%s\n", buf);
}

/* ================================================================ Util.cpp */

std::string getBasePath() { return "PROGDIR:"; }
std::string getPrefPath() { return "PROGDIR:"; }

float getRandomFloat(float a, float b)
{
    float random = ((float) rand()) / (float) RAND_MAX;
    return a + random * (b - a);
}

unsigned getRandomSeed()
{
    return g_fgConfig.seed ? (unsigned) g_fgConfig.seed : (unsigned) time(NULL) ^ fg_micros();
}

int getRandomInt(int min, int max)
{
    return min + (rand() % (int) (max - min + 1));
}

/* ================================================================ OptionsManager.cpp */
/* Same interface; the file is "key=value" lines in PROGDIR:freegemas.prefs. */

static const char *const OPTIONS_FILE = "freegemas.prefs";

void OptionsManager::loadResources()
{
    optionsDir = getPrefPath();
    loadOptions();
}

void OptionsManager::writeOptions()
{
    std::ofstream out((optionsDir + OPTIONS_FILE).c_str());
    for (std::map<std::string, Json::Value>::const_iterator it = options.mMembers.begin();
         it != options.mMembers.end(); ++it)
        if (it->second.mSet) out << it->first << "=" << it->second.mInt << "\n";
}

void OptionsManager::loadOptions()
{
    std::ifstream in((optionsDir + OPTIONS_FILE).c_str());
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        options[line.substr(0, eq)] = atoi(line.c_str() + eq + 1);
    }
}

void OptionsManager::setHighscoreTimetrial(int score) { options["highscoreTimetrial"] = score; writeOptions(); }
void OptionsManager::setHighscoreEndless(int score)   { options["highscoreEndless"] = score; writeOptions(); }
void OptionsManager::setMusicEnabled(bool value)      { options["musicEnabled"] = value; writeOptions(); }
void OptionsManager::setSoundEnabled(bool value)      { options["soundEnabled"] = value; writeOptions(); }
void OptionsManager::setFullscreenEnabled(bool value) { options["fullscreenEnabled"] = value; writeOptions(); }
int  OptionsManager::getHighscoreTimetrial() { return options.get("highscoreTimetrial", 0).asInt(); }
int  OptionsManager::getHighscoreEndless()   { return options.get("highscoreEndless", 0).asInt(); }
bool OptionsManager::getMusicEnabled()       { return options.get("musicEnabled", true).asBool(); }
bool OptionsManager::getSoundEnabled()       { return options.get("soundEnabled", true).asBool(); }
bool OptionsManager::getFullscreenEnabled()  { return options.get("fullscreenEnabled", false).asBool(); }

/* ================================================================ start-up */

static int parse_audio(const char *s)
{
    if (!s) return FG_AUDIO_AUTO;
    if (!strcasecmp(s, "AHI")) return FG_AUDIO_AHI;
    if (!strcasecmp(s, "PAULA")) return FG_AUDIO_PAULA;
    if (!strcasecmp(s, "OFF") || !strcasecmp(s, "NONE")) return FG_AUDIO_OFF;
    return FG_AUDIO_AUTO;
}

static void parse_cli()
{
    static const char TEMPLATE[] = "FULLSCREEN/S,WINDOW/S,AUDIO/K,AHIUNIT/N,STATS/S,FRAMES/N,SHOTS/K,SCRIPT/K,SEED/N,PRESENT/K,FULLREDRAW/S,STATN/N,NOAMMX/S,NOASM/S,DEBUGLAYER/S,NOPRESCALE/S,LOG/S";
    LONG args[17] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rd = ReadArgs((CONST_STRPTR) TEMPLATE, args, NULL);
    if (!rd) {
        Printf((CONST_STRPTR) "Usage: Freegemas %s\n", (ULONG) TEMPLATE);
        return;
    }
    if (args[0]) g_fgConfig.forceDisplay = 1;
    if (args[1]) g_fgConfig.forceDisplay = 0;
    if (args[2]) g_fgConfig.audioMode = parse_audio((const char *) args[2]);
    if (args[3]) g_fgConfig.ahiUnit = (int) *(LONG *) args[3];
    if (args[4]) g_fgConfig.frameStats = 1;
    if (args[5]) g_fgConfig.maxFrames = (int) *(LONG *) args[5];
    if (args[6]) g_fgConfig.shots = strdup((const char *) args[6]);
    if (args[7]) g_fgConfig.script = strdup((const char *) args[7]);
    if (args[8]) g_fgConfig.seed = (int) *(LONG *) args[8];
    if (args[10]) g_fgConfig.fullRedraw = 1;
    if (args[11]) g_fgConfig.statN = (int) *(LONG *) args[11];
    if (args[12]) g_fgConfig.noAmmx = 1;
    if (args[13]) g_fgConfig.noAsm = 1;
    if (args[14]) g_fgConfig.debugLayer = 1;
    if (args[15]) g_fgConfig.noPrescale = 1;
    if (args[16]) g_fgConfig.log = 1;
    if (g_fgConfig.frameStats) g_fgConfig.log = 1;     /* (STATS writes to the log) */
    if (args[9]) {
        const char *p = (const char *) args[9];
        g_fgConfig.present = !strcasecmp(p, "FRIEND") ? 1 : !strcasecmp(p, "DIRECT") ? 2 : !strcasecmp(p, "ARGB") ? 3 : 0;
    }
    FreeArgs(rd);
}

extern "C" struct WBStartup *_WBenchMsg;
struct Library *IconBase = NULL;

/* the icon the game was started from (Workbench): its FULLSCREEN tool type
   follows the in-game option */
static BPTR sIconDir = 0;
static char sIconName[108];
static bool sIconHasFullscreen = false;

static int yes_no(CONST_STRPTR v, int def)
{
    if (!v) return def;
    if (MatchToolValue((STRPTR) v, (STRPTR) "NO") || MatchToolValue((STRPTR) v, (STRPTR) "OFF") ||
        MatchToolValue((STRPTR) v, (STRPTR) "0") || MatchToolValue((STRPTR) v, (STRPTR) "FALSE"))
        return 0;
    return 1;               /* (YES, ON, 1, or just the name) */
}

static void parse_tooltypes()
{
    if (!_WBenchMsg || _WBenchMsg->sm_NumArgs < 1) return;
    IconBase = OpenLibrary("icon.library", 36);
    if (!IconBase) return;
    struct WBArg *arg = &_WBenchMsg->sm_ArgList[0];
    BPTR old = CurrentDir(arg->wa_Lock);
    struct DiskObject *dobj = GetDiskObject(arg->wa_Name);
    if (dobj) {
        CONST_STRPTR *tt = (CONST_STRPTR *) dobj->do_ToolTypes;
        CONST_STRPTR v = (CONST_STRPTR) FindToolType(tt, (CONST_STRPTR) "FULLSCREEN");
        if (v) {
            g_fgConfig.forceDisplay = yes_no(v, 1);
            sIconHasFullscreen = true;
        }
        if (FindToolType(tt, (CONST_STRPTR) "WINDOW")) g_fgConfig.forceDisplay = 0;
        if (FindToolType(tt, (CONST_STRPTR) "STATS")) g_fgConfig.frameStats = g_fgConfig.log = 1;
        v = (CONST_STRPTR) FindToolType(tt, (CONST_STRPTR) "LOG");
        if (v) g_fgConfig.log = yes_no(v, 1);
        if (FindToolType(tt, (CONST_STRPTR) "NOAMMX")) g_fgConfig.noAmmx = 1;
        if (FindToolType(tt, (CONST_STRPTR) "NOASM")) g_fgConfig.noAsm = 1;
        v = (CONST_STRPTR) FindToolType(tt, (CONST_STRPTR) "AUDIO");
        if (v) g_fgConfig.audioMode = parse_audio((const char *) v);
        v = (CONST_STRPTR) FindToolType(tt, (CONST_STRPTR) "AHIUNIT");
        if (v) g_fgConfig.ahiUnit = atoi((const char *) v);
        FreeDiskObject(dobj);
        sIconDir = DupLock(arg->wa_Lock);
        strncpy(sIconName, (const char *) arg->wa_Name, sizeof(sIconName) - 1);
    }
    CurrentDir(old);
    CloseLibrary(IconBase);
    IconBase = NULL;
}

/* the in-game fullscreen option changed: the icon's tool type says the same */
void fg_icon_set_fullscreen(bool on)
{
    if (!sIconDir || !sIconHasFullscreen) return;
    IconBase = OpenLibrary("icon.library", 36);
    if (!IconBase) return;
    BPTR old = CurrentDir(sIconDir);
    struct DiskObject *dobj = GetDiskObject((STRPTR) sIconName);
    if (dobj && dobj->do_ToolTypes) {
        STRPTR *tt = (STRPTR *) dobj->do_ToolTypes;
        int n = 0;
        while (tt[n]) n++;
        STRPTR *nt = (STRPTR *) AllocVec((n + 1) * sizeof(STRPTR), MEMF_ANY | MEMF_CLEAR);
        if (nt) {
            static char entry[] = "FULLSCREEN=NO";
            strcpy(entry, on ? "FULLSCREEN=YES" : "FULLSCREEN=NO");
            for (int i = 0; i < n; i++)
                nt[i] = (!strncasecmp((const char *) tt[i], "FULLSCREEN", 10)) ? (STRPTR) entry : tt[i];
            dobj->do_ToolTypes = (STRPTR *) nt;
            if (!PutDiskObject((STRPTR) sIconName, dobj)) fg_info("[icon] cannot update the tool types\n");
            dobj->do_ToolTypes = tt;           /* (icon.library frees its own array) */
            FreeVec(nt);
        }
    }
    if (dobj) FreeDiskObject(dobj);
    CurrentDir(old);
    CloseLibrary(IconBase);
    IconBase = NULL;
}

int main(int argc, char *argv[])
{
    (void) argv;
    /* the game's own [DEBUG] log on stdout would bring the Shell window back
       in front of the game window; it is not wanted on the Amiga */
    Log::salida = false;
    if (argc > 0) parse_cli();
    else parse_tooltypes();
    fg_info("[BOOT] Freegemas\n");     /* (after the arguments: LOG decides) */

    fg_timer_open();
    fg_kernels_init();  /* (before anything draws or plays) */
    fg_ammx_init();
    if (fg_timer_open()) srand((unsigned) fg_micros() ^ (unsigned) SDL_GetTicks());

    int rc = 0;
    try {
        Game w;
        w.show();
    } catch (const std::exception &e) {
        fg_message("Freegemas", e.what());
        fg_info("[FAIL] %s\n", e.what());
        rc = 20;
    }
    fg_audio_close();
    fg_audio_free_cache();
    fg_assets_free();
    fg_tint_cache_free();
    fg_timer_close();
    if (sIconDir) UnLock(sIconDir);
    fg_info("[END] %ld\n", (long) rc);
    return rc;
}
