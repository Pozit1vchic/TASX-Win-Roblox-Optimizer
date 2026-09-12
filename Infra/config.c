/* TASX.ini reader - plain C, no CRT-specific helpers so it builds as C
   with both MSVC and MinGW. */
#include "config.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define CFG_MAX_ENTRIES 512
#define CFG_SECTION_LEN 32
#define CFG_KEY_LEN     64
#define CFG_VALUE_LEN   192

typedef struct {
    char section[CFG_SECTION_LEN];
    char key[CFG_KEY_LEN];
    char value[CFG_VALUE_LEN];
} CfgEntry;

static CfgEntry g_entries[CFG_MAX_ENTRIES];
/* Staging table for hot-reload: the fresh parse lands here first and is
   memcpy'd over g_entries while the lock is still held, so readers can never
   observe an empty table mid-reload. */
static CfgEntry g_scratch[CFG_MAX_ENTRIES];
static int      g_count     = 0;
static int      g_wasLoaded = 0;
static int      g_truncated = 0;
static CRITICAL_SECTION g_cfgCs;
static int      g_cfgCsInit = 0;

static void ensure_cfg_cs(void)
{
    if (!g_cfgCsInit) {
        InitializeCriticalSection(&g_cfgCs);
        g_cfgCsInit = 1;
    }
}

static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static int ascii_ieq(const char* a, const char* b)
{
    while (*a && *b) {
        if (ascii_lower(*a) != ascii_lower(*b)) return 0;
        ++a; ++b;
    }
    return *a == *b;
}

static void trim_inplace(char* s)
{
    char* start = s;
    size_t len;
    char* end;

    while (*start == ' ' || *start == '\t') ++start;
    if (start != s) {
        size_t n = strlen(start);
        memmove(s, start, n + 1);
    }

    len = strlen(s);
    end = s + len;
    while (end > s && (end[-1] == ' '  || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        --end;
    *end = '\0';
}

static int find_entry(const char* section, const char* key)
{
    int i;
    ensure_cfg_cs();
    EnterCriticalSection(&g_cfgCs);
    for (i = 0; i < g_count; ++i) {
        if (ascii_ieq(g_entries[i].section, section) &&
            ascii_ieq(g_entries[i].key, key)) {
            LeaveCriticalSection(&g_cfgCs);
            return i;
        }
    }
    LeaveCriticalSection(&g_cfgCs);
    return -1;
}

/* Parses iniPath into dst (CFG_MAX_ENTRIES slots) without locking - the
   caller holds g_cfgCs for the whole call. *dstCount receives the number of
   accepted entries, *dstTruncated becomes 1 when keys had to be dropped
   because the table was full. */
static void parse_ini(const char* iniPath, CfgEntry* dst, int* dstCount,
                      int* dstTruncated)
{
    FILE* fp;
    char line[256];
    char curSection[CFG_SECTION_LEN];
    int count = 0;
    int truncated = 0;

    *dstCount = 0;
    *dstTruncated = 0;

    fp = fopen(iniPath, "r");
    if (!fp) return;

    /* Skip a UTF-8 BOM if present, otherwise the first section header
       ("[TASX]" read as "\xEF\xBB\xBF[TASX]") never matches. */
    {
        unsigned char bom[3] = { 0, 0, 0 };
        size_t n = fread(bom, 1, 3, fp);
        long start = 0;
        if (n == 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)
            start = 3;
        fseek(fp, start, SEEK_SET);
    }

    curSection[0] = '\0';

    while (fgets(line, sizeof(line), fp)) {
        char* p = line;

        trim_inplace(p);
        if (p[0] == '\0' || p[0] == ';' || p[0] == '#') continue;

        if (p[0] == '[') {
            char* close = strchr(p, ']');
            size_t len;
            if (!close) continue;
            len = (size_t)(close - p) - 1;
            if (len >= CFG_SECTION_LEN) len = CFG_SECTION_LEN - 1;
            memcpy(curSection, p + 1, len);
            curSection[len] = '\0';
            trim_inplace(curSection);
            continue;
        }
        else {
            char* eq = strchr(p, '=');
            char* key;
            char* val;
            size_t keyLen;

            if (!eq) continue;
            *eq = '\0';
            key = p;
            val = eq + 1;
            trim_inplace(key);
            trim_inplace(val);

            // Inline comment strip (quote-aware, not inside quoted value):
            // '#' always starts a comment; ';' only after whitespace (so
            // list values like CacheLinks=D:\A;E:\B survive intact).
            char* comment = NULL;
            for (char* c = val; *c; ++c) {
                if (*c == '"' || *c == '\'') {
                    // skip quoted segment
                    char quote = *c;
                    ++c;
                    while (*c && *c != quote) {
                        if (*c == '\\' && *(c+1)) c += 2;
                        else ++c;
                    }
                    continue;
                }
                if (*c == '#' || (*c == ';' && (c == val || (c > val && (*(c-1) == ' ' || *(c-1) == '\t'))))) {
                    comment = c;
                    break;
                }
            }
            if (comment) {
                *comment = '\0';
                trim_inplace(val);
            }

            if (key[0] == '\0' || curSection[0] == '\0') continue;

            if (count >= CFG_MAX_ENTRIES) { truncated = 1; break; }

            keyLen = strlen(key);
            if (keyLen >= CFG_KEY_LEN) keyLen = CFG_KEY_LEN - 1;
            memcpy(dst[count].key, key, keyLen);
            dst[count].key[keyLen] = '\0';

            keyLen = strlen(curSection);
            if (keyLen >= CFG_SECTION_LEN) keyLen = CFG_SECTION_LEN - 1;
            memcpy(dst[count].section, curSection, keyLen);
            dst[count].section[keyLen] = '\0';

            keyLen = strlen(val);
            if (keyLen >= CFG_VALUE_LEN) keyLen = CFG_VALUE_LEN - 1;
            memcpy(dst[count].value, val, keyLen);
            dst[count].value[keyLen] = '\0';

            ++count;
        }
    }

    fclose(fp);
    *dstCount = count;
    *dstTruncated = truncated;
}

void config_load(const char* iniPath)
{
    ensure_cfg_cs();
    EnterCriticalSection(&g_cfgCs);
    if (g_wasLoaded) { LeaveCriticalSection(&g_cfgCs); return; }
    g_wasLoaded = 1;
    /* Hold the lock for the whole parse so concurrent readers (trimmer/job
       threads) never see a half-written entry. INI is tiny (< 5 KB). */
    parse_ini(iniPath, g_entries, &g_count, &g_truncated);
    LeaveCriticalSection(&g_cfgCs);
}

void config_default_path(char* out, int outLen)
{
    char exePath[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    DWORD i;

    if (n == 0 || n >= MAX_PATH) {
        snprintf(out, (size_t)outLen, "TASX.ini");
        return;
    }

    for (i = n; i > 0; --i) {
        if (exePath[i - 1] == '\\' || exePath[i - 1] == '/') {
            exePath[i] = '\0';
            break;
        }
    }
    if (i == 0) exePath[0] = '\0';

    snprintf(out, (size_t)outLen, "%sTASX.ini", exePath);
}

int config_loaded(void)
{
    return g_wasLoaded && g_count > 0;
}

const char* config_get_str(const char* section, const char* key, const char* defVal)
{
    int idx = find_entry(section, key);
    if (idx < 0) return defVal;
    return g_entries[idx].value;
}

int config_get_int(const char* section, const char* key, int defVal)
{
    const char* v = config_get_str(section, key, NULL);
    int result = 0;
    int sign = 1;

    if (!v || v[0] == '\0') return defVal;

    if (*v == '-') { sign = -1; ++v; }
    else if (*v == '+') { ++v; }

    while (*v >= '0' && *v <= '9') {
        result = result * 10 + (*v - '0');
        ++v;
    }
    return sign * result;
}

int config_get_bool(const char* section, const char* key, int defVal)
{
    const char* v = config_get_str(section, key, NULL);
    char c;

    if (!v || v[0] == '\0') return defVal;

    c = ascii_lower(v[0]);
    if (c == '0' || c == 'f' || c == 'n' || c == 'o') {
        /* 0, false, no, off */
        if (ascii_ieq(v, "off")) return 0;
        if (c == 'o') return 1; /* "on" */
        return 0;
    }
    if (c == '1' || c == 't' || c == 'y') return 1;
    return defVal;
}

void config_reload(const char* iniPath)
{
    ensure_cfg_cs();
    /* Parse into the scratch table and swap it in under a single lock hold.
       The old version reset g_count first and re-parsed in place, so every
       key (trimmer interval, FFlags preset, job caps...) collapsed to its
       built-in default for the whole parse window - a hot-reload could flip
       live thresholds for a few milliseconds. */
    EnterCriticalSection(&g_cfgCs);
    {
        int count = 0, truncated = 0;
        parse_ini(iniPath, g_scratch, &count, &truncated);
        memcpy(g_entries, g_scratch, sizeof(g_entries));
        g_count = count;
        g_truncated = truncated;
        g_wasLoaded = 1;
    }
    LeaveCriticalSection(&g_cfgCs);
}

int config_truncated(void)
{
    return g_truncated;
}

int config_max_entries(void)
{
    return CFG_MAX_ENTRIES;
}

int config_get_entry_count(void)
{
    ensure_cfg_cs();
    EnterCriticalSection(&g_cfgCs);
    int c = g_count;
    LeaveCriticalSection(&g_cfgCs);
    return c;
}

int config_get_entry(int index, char* secOut, int secLen, char* keyOut, int keyLen,
                     char* valOut, int valLen)
{
    ensure_cfg_cs();
    EnterCriticalSection(&g_cfgCs);
    if (index < 0 || index >= g_count) { LeaveCriticalSection(&g_cfgCs); return 0; }
    if (secOut && secLen > 0) {
        strncpy(secOut, g_entries[index].section, (size_t)secLen - 1);
        secOut[secLen - 1] = '\0';
    }
    if (keyOut && keyLen > 0) {
        strncpy(keyOut, g_entries[index].key, (size_t)keyLen - 1);
        keyOut[keyLen - 1] = '\0';
    }
    if (valOut && valLen > 0) {
        strncpy(valOut, g_entries[index].value, (size_t)valLen - 1);
        valOut[valLen - 1] = '\0';
    }
    LeaveCriticalSection(&g_cfgCs);
    return 1;
}

int config_create_default(const char* iniPath)
{
    FILE* probe = fopen(iniPath, "r");
    if (probe) { fclose(probe); return 0; }

    FILE* fp = fopen(iniPath, "w");
    if (!fp) return 0;

    const char* tpl =
        "; TASX configuration - farm preset for 25-30 clients, FPS cap 20\n"
        "; Every key is optional; missing keys use built-in defaults.\n"
        "; Booleans: 1/0 (true/false/on/off also accepted). Hot-reload: ~10s.\n"
        "; Inline comments after values (\"key=val ; comment\") are supported.\n"
        "\n"
        "[TASX]\n"
        "; Trimmer: UNFOCUSED farm clients are WORKING, not idle (focused never trimmed).\n"
        "; FarmKeepHot=1: periodic pass is soft-only; hard trim only after\n"
        "; HardTrimAfterSec of continuous unfocus (or commit-critical >=90%).\n"
        "TrimUnfocused=1\n"
        "TrimIntervalSec=10\n"
        "AdaptiveTrim=1\n"
        "TrimSkipBelowMB=250\n"
        "FarmKeepHot=1\n"
        "HardTrimAfterSec=1800\n"
        "; Background memory priority 1=VeryLow..5=Normal (default 2=Low on farm)\n"
        "BackgroundMemPriority=2\n"
        "\n"
        "; Job Object cgroup policy\n"
        "; JobAssignMode: auto = try assign, sticky fallback on foreign job (default)\n"
        ";              diagnose = same + InJob diagnostics (deprecated alias: force)\n"
        ";              off   = never assign to Job, per-process fallback only\n"
        "JobAssignMode=auto\n"
        "BackgroundCpuCapPercent=0\n"
        "JobCpuCapPercent=0 ; legacy alias for BackgroundCpuCapPercent\n"
        "JobMemoryCapMB=8192 ; per-process cap: 0=off, 8192 for farm\n"
        "KillOnAgentExit=1\n"
        "CommitBlockThreshold=85\n"
        "MuteBackground=1\n"
        "PinBackgroundToECores=0\n"
        "DynamicAffinity=0\n"
        "\n"
        "; Focus stability (anti-flap)\n"
        "; FocusDwellMs: min time a focus state must persist before TASX reacts.\n"
        "; Flickers < dwell are ignored and profile rewrites coalesced.\n"
        "FocusDwellMs=1800\n"
        "FocusHysteresisMs=500 ; legacy alias, ignored when FocusDwellMs is set\n"
        "\n"
        "; Pagefile / disk free warning threshold (GB on pagefile volume)\n"
        "PagefileWarnFreeGB=8\n"
        "\n"
        "; System cleaner (split: standby purge is safe, global empty-WS evicts farms)\n"
        "LowMemReactor=1\n"
        "LowMemCooldownSec=30\n"
        "SystemCleaner=1\n"
        "SystemCleanStandby=1\n"
        "SystemCleanEmptyWS=0 ; 1=also empty ALL working sets (NOT for keep-hot farms)\n"
        "SystemCleanMinIntervalSec=60\n"
        "PurgeStandbyOnLaunch=1 ; debounced to 1 per 60s for pack spawns\n"
        "\n"
        "; Watchdog\n"
        "SelfCpuWatchdogPercent=5\n"
        "\n"
        "; FarmBoost hotkey (Ctrl+Alt+B): page-in clients from pagefile (always P+E).\n"
        "; Format: Modifiers+Key (Ctrl/Alt/Shift/Win + A-Z/0-9/F1-F24), off=disabled.\n"
        "BoostHotkey=Ctrl+Alt+B\n"
        "PageInIntervalSec=300\n"
        "FarmBoostDefault=0 ; 1=start with the whole farm hot\n"
        "\n"
        "; Farm auto-respawn (opt-in for 24/7 farms): a crashed client is\n"
        "; relaunched with the exact same command line. Event-driven: the new\n"
        "; process is discovered by WMI and hooked like any other spawn.\n"
        "RespawnOnCrash=0\n"
        "RespawnPerHourMax=6 ; per-origin-client respawns per hour (0=unlimited)\n"
        "RespawnGlobalHourCap=60 ; hard GLOBAL crash-loop breaker per hour (0=unlimited)\n"
        "\n"
        "; Hung-client watchdog (default OFF - log only; farm clients can\n"
        "; legitimately block during asset loads so auto-kill may misfire).\n"
        "HungClientWatch=0 ; 1=detect via IsHungAppWindow + log\n"
        "HungKill=0 ; 1=kill hung client (exit feeds auto-respawn if RespawnOnCrash=1)\n"
        "HungKillAfterSecMin=60 ; minutes of CONTINUOUS hang before HungKill fires\n"
        "\n"
        "; Shared-cache LRU trim ([Net] SharedCacheRoot only - client installs\n"
        "; are never touched, reparse points are never followed).\n"
        "CacheMaxGB=20 ; prune oldest cache files over this size (GB), 0=off\n"
        "\n"
        "; Crash handler + timer + power + tweaks\n"
        "; TimerResolution 0.5ms is auto-disabled on farm presets without focus.\n"
        "KillCrashHandler=1\n"
        "TimerResolution=1\n"
        "PowerPlan=1\n"
        "ApplyTweaks=1\n"
        "DisableCpuBoost=0\n"
        "WarmClientFiles=1\n"
        "WarmMaxMB=256\n"
        "DesktopHeapExpand=0\n"
        "\n"
        "; Injector coexistence\n"
        "InjectorOwnsGraphics=0 ; 0=farm owns graphics (recommended), 1=injector owns\n"
        "ForceGraphicsFlags=0 ; when 1, TASX writes graphics flags even if InjectorOwnsGraphics=1\n"
        "\n"
        "[Roblox]\n"
        "; Legacy graphics section - kept for compat, overridden by [FastFlags] Preset.\n"
        "; For farm: UncapFps=0 + TargetFps=20 caps ALL clients strictly to 20 FPS.\n"
        "UncapFps=0\n"
        "TargetFps=20\n"
        "Renderer=D3D10 ; Auto|Vulkan|D3D11|D3D10|OpenGL (Vulkan not recommended for weak PCs)\n"
        "Lighting=Voxel ; Auto|Voxel|ShadowMap|Future (Voxel=fastest)\n"
        "TextureQuality=0 ; Auto|0|1|2|3 (0=lowest)\n"
        "DisableTelemetry=1\n"
        "ExtraVersionsDirs=\n"
        "\n"
        "[FastFlags]\n"
        "; Preset: farm20 | farm30 | weak | balanced | off (off = only manual keys + [Roblox] section)\n"
        "; farm15 = deprecated alias of farm20.\n"
        "; Alive potato (allowlist): Tex0, FRM0, grass 0, CSG-low, PauseVoxelizer,\n"
        "; SkyGray, MSAA1, NoDPIScale, D3D11. FPS placeholder written zero-cost.\n"
        "Preset=farm20\n"
        "FFlagsPruneDead=1\n"
        "; Manual overrides on top of preset (any FFlag/DFInt/DFFlag key = value).\n"
        "; Example: FFlagRenderGpuTextureCompressor=True\n"
        "; Graphics flags are skipped when InjectorOwnsGraphics=1 unless ForceGraphicsFlags=1.\n"
        "\n"
        "[ETW]\n"
        "DisableTelemetry=1\n"
        "\n"
        "[Log]\n"
        "LogLevel=info\n"
        "LogFile=\n"
        "\n"
        "[Net]\n"
        "SharedCacheRoot=\n"
        "CacheLinks=\n";
    fputs(tpl, fp);
    fclose(fp);
    return 1;
}
