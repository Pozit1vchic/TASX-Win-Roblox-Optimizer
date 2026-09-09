#include "fflags.h"
#include "config.h"
#include "log.h"
#include "lograte.h"
#include <windows.h>
#include <string>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

/* ClientAppSettings.json is a flat string->string object; the parser below
   is deliberately minimal and only trusts that shape. Roblox ignores
   unknown flags, so stale names are harmless. */
size_t FindClosingQuote(const std::string& json, size_t from)
{
    /* Skips \" escapes so valid JSON with quotes inside values parses. */
    size_t i = from;
    while (i < json.size()) {
        if (json[i] == '\\' && i + 1 < json.size()) { i += 2; continue; }
        if (json[i] == '"') return i;
        ++i;
    }
    return std::string::npos;
}

std::map<std::string, std::string> ParseFlags(const std::string& json)
{
    std::map<std::string, std::string> out;

    size_t i = 0;
    while ((i = json.find('"', i)) != std::string::npos) {
        size_t keyEnd = FindClosingQuote(json, i + 1);
        if (keyEnd == std::string::npos) break;
        std::string key = json.substr(i + 1, keyEnd - i - 1);

        size_t colon = json.find(':', keyEnd);
        if (colon == std::string::npos) break;

        size_t vs = json.find('"', colon);
        if (vs == std::string::npos) break;
        size_t ve = FindClosingQuote(json, vs + 1);
        if (ve == std::string::npos) break;

        out[key] = json.substr(vs + 1, ve - vs - 1);
        i = ve + 1;
    }
    return out;
}

std::string SerializeFlags(const std::map<std::string, std::string>& flags)
{
    std::ostringstream os;
    os << "{\n";
    bool first = true;
    for (const auto& kv : flags) {
        if (!first) os << ",\n";
        first = false;
        os << "  \"" << kv.first << "\": \"" << kv.second << "\"";
    }
    os << "\n}\n";
    return os.str();
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &s[0], n, nullptr, nullptr);
    return s;
}

struct FlagPlan {
    std::map<std::string, std::string> set;   /* merged over user flags     */
    std::vector<std::string> remove;          /* stripped before the merge  */
};

/* Stable identity of (file state, plan): stat failures are uniformly
   ignored (treated as "unknown — must read"), never as errors. */
bool FileKey(const std::wstring& jsonPath, unsigned long long planHash,
             unsigned long long& outKey)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(jsonPath.c_str(), GetFileExInfoStandard, &fad))
        return false;
    outKey = ((((unsigned long long)fad.nFileSizeLow) |
               ((unsigned long long)fad.nFileSizeHigh << 32)) ^
              (((unsigned long long)fad.ftLastWriteTime.dwLowDateTime) |
               ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32)) ^
              planHash);
    return true;
}

void BuildFlagPlan(FlagPlan& plan)
{
    auto set = [&plan](const char* k, const std::string& v) {
        plan.set[k] = v;
    };
    auto remove = [&plan](std::initializer_list<const char*> keys) {
        for (const char* k : keys) plan.remove.push_back(k);
    };

    // BUG 7: graphics blocks (FPS unlock / Renderer / Lighting /
    // TextureQuality) belong to the external injector when
    // InjectorOwnsGraphics=1. The telemetry block below stays OUTSIDE and
    // ALWAYS applies (both owners disable telemetry: FFlags + ETW).
    bool injectorOwnsGraphics = config_get_bool("TASX", "InjectorOwnsGraphics", 1);
    if (!injectorOwnsGraphics) {

    /* FPS unlock — the headline lever.
       UncapFps=1 means "no cap": TargetFps is IGNORED (uncapped to 999).
       TargetFps applies ONLY when UncapFps=0 (e.g. capped background farm).
       The UncapFps/TargetFps conflict warning lives in master.cpp startup
       (single place) — not here, this plan builder runs per spawn. */
    if (config_get_bool("Roblox", "UncapFps", 1)) {
        set("DFIntTaskSchedulerTargetFps", "999");
        set("FFlagTaskSchedulerLimitTargetFpsTo2402", "False");
    } else {
        int fps = config_get_int("Roblox", "TargetFps", 999);
        if (fps < 30) fps = 30;
        set("DFIntTaskSchedulerTargetFps", std::to_string(fps));
        set("FFlagTaskSchedulerLimitTargetFpsTo2402", "False");
    }

    /* Preferred rendering backend (Vulkan usually nets the most FPS on
       modern GPUs; Auto leaves the client's own choice untouched). */
    std::string renderer = config_get_str("Roblox", "Renderer", "Auto");
    if (renderer != "Auto") {
        remove({ "FFlagDebugGraphicsPreferVulkan",
                 "FFlagDebugGraphicsPreferD3D11",
                 "FFlagDebugGraphicsPreferD3D11FL10",
                 "FFlagDebugGraphicsPreferOpenGL",
                 "FFlagDebugGraphicsDisableDirect3D11" });

        if (renderer == "Vulkan")
            set("FFlagDebugGraphicsPreferVulkan", "True");
        else if (renderer == "D3D11")
            set("FFlagDebugGraphicsPreferD3D11", "True");
        else if (renderer == "D3D10")
            set("FFlagDebugGraphicsPreferD3D11FL10", "True");
        else if (renderer == "OpenGL")
            set("FFlagDebugGraphicsPreferOpenGL", "True");
    }

    /* Lighting technology. */
    std::string lighting = config_get_str("Roblox", "Lighting", "Auto");
    if (lighting != "Auto") {
        remove({ "DFFlagDebugRenderForceTechnologyVoxel",
                 "FFlagDebugForceFutureIsBrightPhase2",
                 "FFlagDebugForceFutureIsBrightPhase3" });

        if (lighting == "Voxel")
            set("DFFlagDebugRenderForceTechnologyVoxel", "True");
        else if (lighting == "ShadowMap")
            set("FFlagDebugForceFutureIsBrightPhase2", "True");
        else if (lighting == "Future")
            set("FFlagDebugForceFutureIsBrightPhase3", "True");
    }

    /* Texture quality override. */
    std::string tex = config_get_str("Roblox", "TextureQuality", "Auto");
    int texLvl = tex != "Auto" && !tex.empty()
                     ? config_get_int("Roblox", "TextureQuality", -1) : -1;
    if (texLvl >= 0 && texLvl <= 3) {
        remove({ "DFFlagTextureQualityOverrideEnabled",
                 "DFIntTextureQualityOverride" });
        set("DFFlagTextureQualityOverrideEnabled", "True");
        set("DFIntTextureQualityOverride", std::to_string(texLvl));
    }

    } /* end !injectorOwnsGraphics */

    /* Cut the client's telemetry chatter. */
    if (config_get_bool("Roblox", "DisableTelemetry", 1)) {
        set("DFFlagDebugDisableTelemetryEphemeralCounter", "True");
        set("DFFlagDebugDisableTelemetryEphemeralEvent", "True");
        set("DFFlagDebugDisableTelemetryV2Counter", "True");
        set("DFFlagDebugDisableTelemetryV2Event", "True");
        set("DFFlagDebugDisableTelemetryV2Stat", "True");
    }
}

bool ApplyToVersion(const std::wstring& versionDir, const FlagPlan& plan)
{
    std::wstring clientDir = versionDir + L"\\ClientSettings";
    std::wstring jsonPath = clientDir + L"\\ClientAppSettings.json";

    // mtime+size pre-check: skip the read entirely when neither the file
    // nor our plan changed since the last pass (farm spawns 100 clients —
    // re-reading every JSON on every spawn is pure I/O waste).
    static std::unordered_map<std::wstring, unsigned long long> s_fileCache;
    unsigned long long planHash = 1469598103934665603ull; // FNV-1a
    auto mix = [&](const std::string& s) {
        for (unsigned char c : s) {
            planHash ^= c;
            planHash *= 1099511628211ull;
        }
        planHash ^= 0x9e3779b9u;
    };
    for (auto& kv : plan.set) { mix(kv.first); mix(kv.second); }
    for (auto& k : plan.remove) mix(k);
    {
        unsigned long long fkey = 0;
        if (FileKey(jsonPath, planHash, fkey)) {
            auto it = s_fileCache.find(jsonPath);
            if (it != s_fileCache.end() && it->second == fkey)
                return false; // already in desired state, verified last pass
        }
    }

    CreateDirectoryW(clientDir.c_str(), nullptr);

    std::ifstream in(jsonPath.c_str(), std::ios::binary);
    std::string existing((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    in.close();

    auto flags = ParseFlags(existing);
    for (const auto& k : plan.remove) flags.erase(k);
    for (const auto& kv : plan.set) flags[kv.first] = kv.second;

    std::string merged = SerializeFlags(flags);
    if (merged == existing) {
        // Record verified state so the next pass skips the read.
        // Stat failure is ignored (simply no cache entry).
        unsigned long long fkey = 0;
        if (FileKey(jsonPath, planHash, fkey))
            s_fileCache[jsonPath] = fkey;
        return false; /* nothing to do */
    }

    std::wstring tmpPath = jsonPath + L".tasx.tmp";
    std::ofstream out(tmpPath.c_str(), std::ios::binary | std::ios::trunc);
    out << merged;
    out.close();
    if (!out) {
        LOGW("[FFlags] Failed to write into %s", WideToUtf8(versionDir).c_str());
        return false;
    }

    if (!MoveFileExW(tmpPath.c_str(), jsonPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        DWORD err = GetLastError();
        DeleteFileW(tmpPath.c_str());
        // Deleted/locked version dir during Roblox update — not fatal.
        if (LogRateLimit("fflags-movefail", 60))
            LOGW("[FFlags] Skip %s (locked/removed, err %lu)",
                 WideToUtf8(versionDir).c_str(), (unsigned long)err);
        return false;
    }

    // Refresh cache to post-write state (stat failure ignored).
    {
        unsigned long long fkey = 0;
        if (FileKey(jsonPath, planHash, fkey))
            s_fileCache[jsonPath] = fkey;
    }

    if (LogRateLimit("fflags-applied", 10))
        LOGI("[FFlags] Applied %u flags -> %s", (unsigned)plan.set.size(),
             WideToUtf8(jsonPath).c_str());
    return true;
}

} /* namespace */

/* Scans <root>\<version>\ for installed clients (any layout where a folder
   contains RobloxPlayerBeta.exe gets a ClientSettings subfolder).
   Counts: scanned = version dirs found, applied = files actually rewritten. */
void ScanVersionsRoot(const std::wstring& root, const FlagPlan& plan,
                      int& scanned, int& applied)
{
    /* Root itself may be a client dir (portable installs). */
    if (GetFileAttributesW((root + L"\\RobloxPlayerBeta.exe").c_str()) !=
        INVALID_FILE_ATTRIBUTES)
    {
        ++scanned;
        if (ApplyToVersion(root, plan)) ++applied;
        return;
    }

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return; // removed/unavailable root — not fatal

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        std::wstring versionDir = root + L"\\" + fd.cFileName;
        std::wstring clientExe = versionDir + L"\\RobloxPlayerBeta.exe";
        if (GetFileAttributesW(clientExe.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;

        ++scanned;
        if (ApplyToVersion(versionDir, plan)) ++applied;
    } while (FindNextFileW(find, &fd));

    FindClose(find);
}

void FFlagsApply()
{
    if (config_get_bool("TASX", "InjectorOwnsGraphics", 1)) {
        // Contract handled inside BuildFlagPlan — plan contains only
        // telemetry keys, ApplyToVersion preserves injector-owned flags.
        // Once per hour max: was spammed on every process spawn.
        if (LogRateLimit("fflags-injector", 3600))
            LOGI("[FFlags] InjectorOwnsGraphics=1 — writing telemetry only");
    }
    FlagPlan plan;
    BuildFlagPlan(plan);
    int scanned = 0, applied = 0;

    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH))
        ScanVersionsRoot(std::wstring(localAppData) + L"\\Roblox\\Versions",
                         plan, scanned, applied);

    /* Extra roots for portable / multi-manager installs, e.g.
       ExtraVersionsDirs=D:\RobloxMultiManager\clients;E:\RBX */
    std::string extra = config_get_str("Roblox", "ExtraVersionsDirs", "");
    size_t pos = 0;
    while (pos < extra.size()) {
        size_t end = extra.find(';', pos);
        if (end == std::string::npos) end = extra.size();

        std::string item = extra.substr(pos, end - pos);
        pos = end + 1;

        /* trim */
        size_t b = item.find_first_not_of(" \t\"");
        size_t e = item.find_last_not_of(" \t\"");
        if (b == std::string::npos) continue;
        item = item.substr(b, e - b + 1);
        if (item.empty()) continue;

        int n = MultiByteToWideChar(CP_UTF8, 0, item.c_str(), -1, nullptr, 0);
        if (n <= 1) continue;
        std::wstring wItem(n - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, item.c_str(), -1, &wItem[0], n);

        int before = scanned;
        ScanVersionsRoot(wItem, plan, scanned, applied);
        if (scanned > before && LogRateLimit("fflags-extraroot", 60))
            LOGI("[FFlags] Extra root processed: %s", item.c_str());
    }

    // Log only real work: silent when everything already in desired state.
    if (applied)
        LOGI("[FFlags] %d file(s) rewritten, %d version(s) scanned",
             applied, scanned);
}
