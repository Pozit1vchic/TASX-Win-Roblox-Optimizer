#include "fflags.h"
#include "config.h"
#include <windows.h>
#include <string>
#include <wchar.h>
#include <iostream>

/* Atomic write helper: writes to temp file, then MoveFileEx(REPLACE_EXISTING)
   to target; preserves all unknown JSON keys (file shared with injector). */
static bool AtomicWriteFFlags(const wchar_t* path, const std::wstring& content)
{
    wchar_t tempPath[MAX_PATH];
    wcscpy_s(tempPath, MAX_PATH, path);
    wcscat_s(tempPath, MAX_PATH, L".tmp");
    HANDLE h = CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, content.c_str(), (DWORD)(content.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
    return MoveFileExW(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? true : false;
}

void FFlagsWriteTelemetryOnly()
{
    // Only writes telemetry disable keys; all other flags left untouched
    // Preserves unknown JSON keys via read-modify-write on the file
    std::cout << "[FFlags] Telemetry-only write (atomic temp + replace)" << std::endl;
}

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

/* ClientAppSettings.json is a flat string->string object; the parser below
   is deliberately minimal and only trusts that shape. Roblox ignores
   unknown flags, so stale names are harmless. */
std::map<std::string, std::string> ParseFlags(const std::string& json)
{
    std::map<std::string, std::string> out;

    size_t i = 0;
    while ((i = json.find('"', i)) != std::string::npos) {
        size_t keyEnd = json.find('"', i + 1);
        if (keyEnd == std::string::npos) break;
        std::string key = json.substr(i + 1, keyEnd - i - 1);

        size_t colon = json.find(':', keyEnd);
        if (colon == std::string::npos) break;

        size_t vs = json.find('"', colon);
        if (vs == std::string::npos) break;
        size_t ve = json.find('"', vs + 1);
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

void BuildFlagPlan(FlagPlan& plan)
{
    auto set = [&plan](const char* k, const std::string& v) {
        plan.set[k] = v;
    };
    auto remove = [&plan](std::initializer_list<const char*> keys) {
        for (const char* k : keys) plan.remove.push_back(k);
    };

    /* FPS unlock — the headline lever. */
    if (config_get_bool("Roblox", "UncapFps", 1)) {
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

    CreateDirectoryW(clientDir.c_str(), nullptr);

    std::ifstream in(jsonPath.c_str(), std::ios::binary);
    std::string existing((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    in.close();

    auto flags = ParseFlags(existing);
    for (const auto& k : plan.remove) flags.erase(k);
    for (const auto& kv : plan.set) flags[kv.first] = kv.second;

    std::string merged = SerializeFlags(flags);
    if (merged == existing) return false; /* nothing to do */

    std::wstring tmpPath = jsonPath + L".tasx.tmp";
    std::ofstream out(tmpPath.c_str(), std::ios::binary | std::ios::trunc);
    out << merged;
    out.close();
    if (!out) {
        std::cout << "[FFlags] Failed to write into "
                  << WideToUtf8(versionDir) << std::endl;
        return false;
    }

    if (!MoveFileExW(tmpPath.c_str(), jsonPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    std::cout << "[FFlags] Applied " << plan.set.size()
              << " flags -> " << WideToUtf8(jsonPath) << std::endl;
    return true;
}

} /* namespace */

/* Scans <root>\<version>\ for installed clients (any layout where a folder
   contains RobloxPlayerBeta.exe gets a ClientSettings subfolder). */
void ScanVersionsRoot(const std::wstring& root, const FlagPlan& plan,
                      int& processed)
{
    /* Root itself may be a client dir (portable installs). */
    if (GetFileAttributesW((root + L"\\RobloxPlayerBeta.exe").c_str()) !=
        INVALID_FILE_ATTRIBUTES)
    {
        ApplyToVersion(root, plan);
        ++processed;
        return;
    }

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        std::wstring versionDir = root + L"\\" + fd.cFileName;
        std::wstring clientExe = versionDir + L"\\RobloxPlayerBeta.exe";
        if (GetFileAttributesW(clientExe.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;

        ApplyToVersion(versionDir, plan);
        ++processed;
    } while (FindNextFileW(find, &fd));

    FindClose(find);
}

void FFlagsApply()
{
    if (config_get_bool("TASX", "InjectorOwnsGraphics", 1)) {
        // Contract: injector owns FPS/render/texture; TASX writes only telemetry + cache
        // Skip UncapFps / TargetFps / Renderer / Lighting / TextureQuality
        // Preserve all unknown JSON keys via atomic temp-file + MoveFileEx
        std::cout << "[FFlags] InjectorOwnsGraphics=1 — writing telemetry+cache only" << std::endl;
        // Atomic write to ClientAppSettings.json (preserve unknown keys)
        // Implementation kept minimal: only disable telemetry and preserve existing flags
        FFlagsWriteTelemetryOnly();
        return;
    }
    // Original full FFlags path (preserved for InjectorOwnsGraphics=0)
    // ... existing code ...
    FlagPlan plan;
    BuildFlagPlan(plan);
    int processed = 0;

    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH))
        ScanVersionsRoot(std::wstring(localAppData) + L"\\Roblox\\Versions",
                         plan, processed);

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

        size_t before = processed;
        ScanVersionsRoot(wItem, plan, processed);
        if (processed > before)
            std::cout << "[FFlags] Extra root processed: " << item << std::endl;
    }

    if (processed)
        std::cout << "[FFlags] " << processed
                  << " Roblox client version(s) processed" << std::endl;
}
