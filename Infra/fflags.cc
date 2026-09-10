#include "fflags.h"
#include "config.h"
#include "log.h"
#include "lograte.h"
#include <windows.h>
#include <string>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

static bool IsGraphicsFlag(const std::string& k)
{
    // FPS / renderer / lighting / texture / quality flags are "graphics".
    // NOTE: most pre-allowlist graphics flags are dead client-side (ignored),
    // but the ownership gate still applies to them so TASX never fights the
    // injector over keys it doesn't own.
    if (k == "DFIntTaskSchedulerTargetFps") return true;
    if (k == "FFlagTaskSchedulerLimitTargetFpsTo2402") return true;
    if (k.rfind("FFlagDebugGraphics", 0) == 0) return true;
    if (k.rfind("DFFlagDebugRenderForceTechnologyVoxel", 0) == 0) return true;
    if (k.rfind("FFlagDebugForceFutureIsBright", 0) == 0) return true;
    if (k == "DFFlagTextureQualityOverrideEnabled") return true;
    if (k == "DFIntTextureQualityOverride") return true;
    if (k == "DFIntDebugFRMQualityLevelOverride") return true;
    if (k == "FFlagRenderGpuTextureCompressor") return true;
    if (k == "DFIntUseLevelOfDetail") return true;
    if (k == "FFlagDebugUseLevelOfDetail") return true;
    if (k == "FIntFRMMaxGrassDistance") return true;
    if (k == "FIntFRMMinGrassDistance") return true;
    if (k == "FIntFRMMaxGrass23") return true;
    if (k == "DFIntCSGLevelOfDetail") return true;
    if (k.rfind("DFIntCSGLevelOfDetailSwitchingDistance", 0) == 0) return true;
    if (k == "DFFlagDebugPauseVoxelizer") return true;
    if (k == "FFlagDebugSkyGray") return true;
    if (k == "FIntDebugForceMSAASamples") return true;
    if (k == "DFFlagDisableDPIScale") return true;
    if (k == "FIntGrassMovementReducedMotionFactor") return true;
    return false;
}

/* Official Roblox local allowlist (announcement 29.09.2025, re-checked
   01.08.2026): 18 keys - 4 geometry, 13 rendering, 1 UI. Everything else in
   ClientAppSettings.json is silently ignored by the client. The list is
   signed per channel and can change without notice; unknown keys get a
   one-time LOGW (see SuggestReplacement). */
static bool IsFlagAllowed(const std::string& k)
{
    static const std::unordered_map<std::string,int> allow = {
        // Geometry (4)
        {"DFIntCSGLevelOfDetailSwitchingDistance",1},
        {"DFIntCSGLevelOfDetailSwitchingDistanceL12",1},
        {"DFIntCSGLevelOfDetailSwitchingDistanceL23",1},
        {"DFIntCSGLevelOfDetailSwitchingDistanceL34",1},
        // Rendering (13)
        {"FFlagHandleAltEnterFullscreenManually",1},
        {"DFFlagTextureQualityOverrideEnabled",1},
        {"DFIntTextureQualityOverride",1},
        {"FIntDebugForceMSAASamples",1},
        {"DFFlagDisableDPIScale",1},
        {"FFlagDebugGraphicsPreferD3D11",1},
        {"FFlagDebugSkyGray",1},
        {"DFFlagDebugPauseVoxelizer",1},
        {"DFIntDebugFRMQualityLevelOverride",1},
        {"FIntFRMMaxGrassDistance",1},
        {"FIntFRMMinGrassDistance",1},
        {"FFlagDebugGraphicsPreferVulkan",1},
        {"FFlagDebugGraphicsPreferOpenGL",1},
        // UI (1)
        {"FIntGrassMovementReducedMotionFactor",1},
    };
    if (allow.find(k) != allow.end()) return true;
    return false;
}

/* FPS-intent placeholder: DFIntTaskSchedulerTargetFps is NOT on the official
   allowlist (sources conflict; Bloxstrap guides claim it works, the
   announcement list omits it). Writing it costs nothing when ignored and
   applies automatically if re-allowed - so the farm presets keep it as a
   zero-cost placeholder, but it is NOT counted as an alive flag. */
static bool IsFpsPlaceholder(const std::string& k)
{
    return k == "DFIntTaskSchedulerTargetFps" ||
           k == "FFlagTaskSchedulerLimitTargetFpsTo2402";
}

static const char* SuggestReplacement(const std::string& k)
{
    if (k == "DFIntTaskSchedulerTargetFps" || k == "FFlagTaskSchedulerLimitTargetFpsTo2402")
        return "FPS cap now lives in GlobalBasicSettings_13.xml FramerateCap (client reads it, flags don't)";
    if (k == "FFlagDebugGraphicsPreferD3D11FL10") return "D3D10 override is not allowlisted; use FFlagDebugGraphicsPreferD3D11=True";
    if (k == "DFFlagDebugRenderForceTechnologyVoxel") return "Voxel forcing is not allowlisted; use DFFlagDebugPauseVoxelizer=True + FFlagDebugSkyGray=True";
    if (k.find("ShadowMap") != std::string::npos || k.find("Future") != std::string::npos)
        return "Future/ShadowMap are not allowlisted; use DFFlagDebugPauseVoxelizer=True + DFIntTextureQualityOverride=0";
    if (k.find("StreamingEnable") != std::string::npos) return "Streaming toggles are not allowlisted; no live replacement";
    if (k.find("Telemetry") != std::string::npos) return "Telemetry flags are blocked; ETW suppression (TASX [ETW] section) still works OS-side";
    if (k.find("PhysicsReplication") != std::string::npos) return "Physics follows frame rate engine-side; no flag needed";
    if (k == "DFIntCSGLevelOfDetail") return "Use DFIntCSGLevelOfDetailSwitchingDistance*=75-150 instead";
    return "not on Roblox allowlist (18 keys); ignored by client - remove flag";
}

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
   ignored (treated as "unknown - must read"), never as errors. */
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
    auto setSafe = [&plan](const char* k, const std::string& v) {
        if (!IsFlagAllowed(k) && !IsFpsPlaceholder(k)) {
            char tag[128]; snprintf(tag,sizeof(tag),"fflags-unknown-%s",k);
            if (LogRateLimit(tag, 3600))
                LOGW("[FFlags] Unknown/dead flag '%s' skipped (%s)", k, SuggestReplacement(k));
            return;
        }
        // graphics ownership check inside setSafe via caller context - handled externally for preset,
        // but for manual overrides we also check
        plan.set[k] = v;
    };
    auto remove = [&plan](std::initializer_list<const char*> keys) {
        for (const char* k : keys) plan.remove.push_back(k);
    };

    // Preset first: injector default depends on it for backward compat.
    // Legacy (no preset): default 1 (injector owns graphics, old behavior).
    // TASX-owned presets (farm15/20/30/weak/balanced): default 0 (TASX owns graphics).
    // Explicit key in either section always wins.
    const char* presetRawPre = config_get_str("FastFlags", "Preset", nullptr);
    if (!presetRawPre) presetRawPre = config_get_str("TASX", "Preset", nullptr);
    bool hasPresetPre = false;
    if (presetRawPre) {
        char pl[16] = {};
        size_t pn = 0;
        for (; presetRawPre[pn] && pn + 1 < sizeof(pl); ++pn)
            pl[pn] = (char)tolower((unsigned char)presetRawPre[pn]);
        pl[pn] = '\0';
        hasPresetPre = (strcmp(pl, "farm15") == 0 || strcmp(pl, "farm20") == 0 || strcmp(pl, "farm30") == 0 || strcmp(pl, "weak") == 0 ||
                        strcmp(pl, "balanced") == 0);
    }
    const char* injStr1 = config_get_str("TASX", "InjectorOwnsGraphics", nullptr);
    const char* injStr2 = config_get_str("FastFlags", "InjectorOwnsGraphics", nullptr);
    bool injectorOwnsGraphics;
    if (!injStr1 && !injStr2) {
        injectorOwnsGraphics = hasPresetPre ? false : true;
    } else {
        injectorOwnsGraphics = config_get_bool("TASX", "InjectorOwnsGraphics", 1) ||
                               config_get_bool("FastFlags", "InjectorOwnsGraphics", 0);
        // NOTE: when key exists in only one section, the other section's
        // default must not force true. Re-evaluate strictly:
        if (injStr1 && !injStr2)
            injectorOwnsGraphics = config_get_bool("TASX", "InjectorOwnsGraphics", 1) != 0;
        else if (!injStr1 && injStr2)
            injectorOwnsGraphics = config_get_bool("FastFlags", "InjectorOwnsGraphics", 1) != 0;
    }
    bool forceGraphics = config_get_bool("TASX", "ForceGraphicsFlags", 0) ||
                         config_get_bool("FastFlags", "ForceGraphicsFlags", 0);

    auto canWriteGraphics = [&]() -> bool {
        if (!injectorOwnsGraphics) return true;
        return forceGraphics;
    };

    // Preset handling: [FastFlags] Preset=farm20|farm30|weak|balanced|off
    // farm15 is a deprecated alias of farm20 (kept for old configs).
    const char* presetRaw = config_get_str("FastFlags", "Preset", nullptr);
    if (!presetRaw) presetRaw = config_get_str("TASX", "Preset", nullptr);
    std::string preset;
    if (presetRaw) {
        preset = presetRaw;
        for (auto &c: preset) c = (char)tolower((unsigned char)c);
    }

    bool isFarm = (preset == "farm15" || preset == "farm20" || preset == "farm30");
    bool isWeak = (preset == "weak");
    bool isBalanced = (preset == "balanced");
    // FPS budget follows the preset name; physics replication is pinned to
    // the same value (user directive: physics fixed to FPS).
    int farmFps = 20;
    if (preset == "farm15") farmFps = 15;
    else if (preset == "farm30") farmFps = 30;
    // default to farm20 when FastFlags section exists but preset missing? No - stay legacy for compat.
    // If user created new TASX.ini via config_create_default, preset will be farm20.

    if (isFarm) {
        // FPS intent placeholder: zero-cost if the client ignores it
        // (allowlist), auto-applies if re-allowed. UncapFps=0 logic stays.
        if (canWriteGraphics()) {
            setSafe("DFIntTaskSchedulerTargetFps", std::to_string(farmFps));
            setSafe("FFlagTaskSchedulerLimitTargetFpsTo2402", "False");
            // Renderer D3D11 - the only potato backend on the allowlist
            // (D3D10/FL10 override is dead; Vulkan kept off the farm)
            remove({ "FFlagDebugGraphicsPreferVulkan","FFlagDebugGraphicsPreferD3D11","FFlagDebugGraphicsPreferOpenGL","FFlagDebugGraphicsPreferD3D11FL10","FFlagDebugGraphicsDisableDirect3D11" });
            setSafe("FFlagDebugGraphicsPreferD3D11", "True");
            // Lighting: voxel forcing is dead - pause voxelizer + gray sky
            remove({ "DFFlagDebugRenderForceTechnologyVoxel","FFlagDebugForceFutureIsBrightPhase2","FFlagDebugForceFutureIsBrightPhase3" });
            setSafe("DFFlagDebugPauseVoxelizer", "True");
            setSafe("FFlagDebugSkyGray", "True");
            // Texture 0 - full potato (alive)
            remove({ "DFFlagTextureQualityOverrideEnabled","DFIntTextureQualityOverride","DFIntDebugFRMQualityLevelOverride" });
            setSafe("DFFlagTextureQualityOverrideEnabled", "True");
            setSafe("DFIntTextureQualityOverride", "0");
            setSafe("DFIntDebugFRMQualityLevelOverride", "0");
            // MSAA minimum + no DPI upscale
            setSafe("FIntDebugForceMSAASamples", "1");
            setSafe("DFFlagDisableDPIScale", "True");
            // Grass: none, still air
            setSafe("FIntFRMMaxGrassDistance", "0");
            setSafe("FIntFRMMinGrassDistance", "0");
            setSafe("FIntGrassMovementReducedMotionFactor", "0");
            // Geometry detail collapses early (alive CSG distances, low-end values)
            setSafe("DFIntCSGLevelOfDetailSwitchingDistance", "100");
            setSafe("DFIntCSGLevelOfDetailSwitchingDistanceL12", "75");
            setSafe("DFIntCSGLevelOfDetailSwitchingDistanceL23", "100");
            setSafe("DFIntCSGLevelOfDetailSwitchingDistanceL34", "150");
        }
        // NOTE: physics follows frame rate engine-side (replication flag is
        // dead); streaming/LOD/telemetry toggles are dead (allowlist) and are
        // NOT written. ETW suppression still applies OS-side ([ETW] section).
        // anti-flags: ensure high quality never enabled
        remove({ "FFlagDebugForceFutureIsBrightPhase2","FFlagDebugForceFutureIsBrightPhase3" });

    } else if (isWeak) {
        // Lighter potato: FPS intent + D3D11 + Tex1 + reduced grass/geometry.
        // Dead toggles (voxel forcing, compressor, LOD, streaming) not written.
        if (canWriteGraphics()) {
            setSafe("DFIntTaskSchedulerTargetFps", "30");
            setSafe("FFlagTaskSchedulerLimitTargetFpsTo2402", "False");
            remove({ "FFlagDebugGraphicsPreferVulkan","FFlagDebugGraphicsPreferD3D11","FFlagDebugGraphicsPreferD3D11FL10","FFlagDebugGraphicsPreferOpenGL","FFlagDebugGraphicsDisableDirect3D11" });
            setSafe("FFlagDebugGraphicsPreferD3D11", "True");
            remove({ "DFFlagDebugRenderForceTechnologyVoxel","FFlagDebugForceFutureIsBrightPhase2","FFlagDebugForceFutureIsBrightPhase3" });
            setSafe("DFFlagDebugPauseVoxelizer", "True");
            remove({ "DFFlagTextureQualityOverrideEnabled","DFIntTextureQualityOverride","DFIntDebugFRMQualityLevelOverride" });
            setSafe("DFFlagTextureQualityOverrideEnabled", "True");
            setSafe("DFIntTextureQualityOverride", "1");
            setSafe("DFIntDebugFRMQualityLevelOverride", "1");
            setSafe("FIntFRMMaxGrassDistance", "0");
            setSafe("FIntFRMMinGrassDistance", "0");
            setSafe("FIntDebugForceMSAASamples", "1");
        }

    } else if (isBalanced) {
        if (canWriteGraphics()) {
            setSafe("DFIntTaskSchedulerTargetFps", "60");
            setSafe("FFlagTaskSchedulerLimitTargetFpsTo2402", "False");
            // balanced keeps Auto renderer; voxel forcing is dead
            remove({ "DFFlagDebugRenderForceTechnologyVoxel","FFlagDebugForceFutureIsBrightPhase2","FFlagDebugForceFutureIsBrightPhase3" });
            remove({ "DFFlagTextureQualityOverrideEnabled","DFIntTextureQualityOverride" });
            setSafe("DFFlagTextureQualityOverrideEnabled", "True");
            setSafe("DFIntTextureQualityOverride", "2");
        }

    } else {
        // No preset (legacy path) - respect [Roblox] section for backward compat.
        // Only allowlisted flags are written; dead values get a replacement hint.
        if (canWriteGraphics()) {
            if (config_get_bool("Roblox", "UncapFps", 1)) {
                setSafe("DFIntTaskSchedulerTargetFps", "999");
                setSafe("FFlagTaskSchedulerLimitTargetFpsTo2402", "False");
            } else {
                int fps = config_get_int("Roblox", "TargetFps", 999);
                if (fps < 1) fps = 1;
                if (fps > 999) fps = 999;
                setSafe("DFIntTaskSchedulerTargetFps", std::to_string(fps));
                setSafe("FFlagTaskSchedulerLimitTargetFpsTo2402", "False");
            }
            std::string renderer = config_get_str("Roblox", "Renderer", "Auto");
            if (renderer != "Auto") {
                remove({ "FFlagDebugGraphicsPreferVulkan","FFlagDebugGraphicsPreferD3D11","FFlagDebugGraphicsPreferD3D11FL10","FFlagDebugGraphicsPreferOpenGL","FFlagDebugGraphicsDisableDirect3D11" });
                if (renderer == "Vulkan") setSafe("FFlagDebugGraphicsPreferVulkan", "True");
                else if (renderer == "D3D11") setSafe("FFlagDebugGraphicsPreferD3D11", "True");
                else if (renderer == "D3D10") {
                    if (LogRateLimit("fflags-d3d10-dead", 3600))
                        LOGW("[FFlags] Renderer=D3D10 is not allowlisted by Roblox - falling back to D3D11 (alive)");
                    setSafe("FFlagDebugGraphicsPreferD3D11", "True");
                }
                else if (renderer == "OpenGL") setSafe("FFlagDebugGraphicsPreferOpenGL", "True");
            }
            std::string lighting = config_get_str("Roblox", "Lighting", "Auto");
            if (lighting != "Auto") {
                remove({ "DFFlagDebugRenderForceTechnologyVoxel","FFlagDebugForceFutureIsBrightPhase2","FFlagDebugForceFutureIsBrightPhase3" });
                if (lighting == "Voxel") {
                    if (LogRateLimit("fflags-voxel-dead", 3600))
                        LOGW("[FFlags] Lighting=Voxel forcing is not allowlisted - using PauseVoxelizer+SkyGray instead");
                    setSafe("DFFlagDebugPauseVoxelizer", "True");
                    setSafe("FFlagDebugSkyGray", "True");
                }
                else if (lighting == "ShadowMap" || lighting == "Future") {
                    if (LogRateLimit("fflags-hiqual-dead", 3600))
                        LOGW("[FFlags] Lighting=%s is not allowlisted and anti-potato - ignored", lighting.c_str());
                }
            }
            std::string tex = config_get_str("Roblox", "TextureQuality", "Auto");
            int texLvl = tex != "Auto" && !tex.empty() ? config_get_int("Roblox", "TextureQuality", -1) : -1;
            if (texLvl >= 0 && texLvl <= 3) {
                remove({ "DFFlagTextureQualityOverrideEnabled","DFIntTextureQualityOverride" });
                setSafe("DFFlagTextureQualityOverrideEnabled", "True");
                setSafe("DFIntTextureQualityOverride", std::to_string(texLvl));
            }
            // anti-flag for legacy: cap debug quality
            if (texLvl == 0) setSafe("DFIntDebugFRMQualityLevelOverride", "0");
        }
    }

    // Manual overrides from [FastFlags] (key=value on top of preset) - respects allowlist & graphics gate
    {
        int cnt = config_get_entry_count();
        for (int i = 0; i < cnt; ++i) {
            char sec[32] = {}, key[64] = {}, val[192] = {};
            if (!config_get_entry(i, sec, sizeof(sec), key, sizeof(key), val, sizeof(val))) continue;
            char lowSec[32]={};
            for (size_t k=0;k<sizeof(lowSec)-1 && sec[k];++k) lowSec[k]=(char)tolower((unsigned char)sec[k]);
            bool isFF = (strcmp(lowSec,"fastflags")==0);
            if (!isFF) continue;
            // skip Preset key
            char klow[64]={};
            for (size_t k=0;k<sizeof(klow)-1 && key[k];++k) klow[k]=(char)tolower((unsigned char)key[k]);
            if (strcmp(klow,"preset")==0) continue;
            if (strcmp(klow,"injectorownsgraphics")==0) continue;
            if (strcmp(klow,"forcegraphicsflags")==0) continue;
            if (strcmp(klow,"fflagsprunedead")==0) continue;
            // graphics gate
            if (IsGraphicsFlag(key) && !canWriteGraphics()) {
                if (LogRateLimit("fflags-graphics-skip", 60))
                    LOGI("[FFlags] Skip graphics flag '%s' (InjectorOwnsGraphics=1, ForceGraphicsFlags=0)", key);
                continue;
            }
            if (!IsFlagAllowed(key) && !IsFpsPlaceholder(key)) {
                char tag[128]; snprintf(tag,sizeof(tag),"fflags-unknown-%s",key);
                if (LogRateLimit(tag, 3600))
                    LOGW("[FFlags] Unknown flag '%s' in [FastFlags] skipped (%s)", key, SuggestReplacement(key));
                continue;
            }
            plan.set[key] = val;
        }
    }

    // Anti-flags enforcement: never leave high-quality residues
    if (isFarm && canWriteGraphics()) {
        // ensure Future/ShadowMap removed even if manual tried to set them
        plan.remove.push_back("FFlagDebugForceFutureIsBrightPhase2");
        plan.remove.push_back("FFlagDebugForceFutureIsBrightPhase3");
        // enforce TextureQuality 0 even if manual set 1-3 (spec: 1-3 never)
        auto it = plan.set.find("DFIntTextureQualityOverride");
        if (it != plan.set.end() && it->second != "0") {
            if (LogRateLimit("fflags-antitex", 60))
                LOGW("[FFlags] Anti-flag: DFIntTextureQualityOverride=%s overrides to 0 (farm)", it->second.c_str());
            it->second = "0";
        }
        auto it2 = plan.set.find("DFIntDebugFRMQualityLevelOverride");
        if (it2 != plan.set.end() && it2->second != "0") {
            if (LogRateLimit("fflags-antifrm", 60))
                LOGW("[FFlags] Anti-flag: DFIntDebugFRMQualityLevelOverride forced to 0");
            it2->second = "0";
        }
        // MSAA above minimum and grass above zero are quality-raising - force down.
        auto it3 = plan.set.find("FIntDebugForceMSAASamples");
        if (it3 != plan.set.end() && it3->second != "0" && it3->second != "1") {
            if (LogRateLimit("fflags-antimsaa", 60))
                LOGW("[FFlags] Anti-flag: FIntDebugForceMSAASamples=%s forced to 1 (farm)", it3->second.c_str());
            it3->second = "1";
        }
        auto it4 = plan.set.find("FIntFRMMaxGrassDistance");
        if (it4 != plan.set.end() && it4->second != "0") {
            if (LogRateLimit("fflags-antigrass", 60))
                LOGW("[FFlags] Anti-flag: FIntFRMMaxGrassDistance forced to 0 (farm)");
            it4->second = "0";
        }
    }

    /* Telemetry JSON flags are BLOCKED by the Roblox allowlist (ignored by
       the client) - not written. Telemetry suppression stays OS-side via the
       [ETW] section (tasx_etw_disable_provider). Nothing to do here. */

    // Dead-key cleanup (user approved): erase pre-allowlist leftovers so the
    // JSON doesn't rot with ignored entries. Runs ONLY when TASX owns
    // graphics - never fight the injector (it would re-add its keys and the
    // two writers would churn the file back and forth).
    // Default ON for strict farm presets, OFF otherwise.
    {
        bool isStrictFarm = isFarm; // farm15/20/30
        const char* pv = config_get_str("TASX", "FFlagsPruneDead", nullptr);
        if (!pv) pv = config_get_str("FastFlags", "FFlagsPruneDead", nullptr);
        bool prune = pv ? (config_get_bool("TASX", "FFlagsPruneDead", 0) ||
                           config_get_bool("FastFlags", "FFlagsPruneDead", 0))
                        : isStrictFarm;
        // NOTE: when the key exists in only one section the other section's
        // default must not force it on - re-evaluate strictly.
        const char* p1 = config_get_str("TASX", "FFlagsPruneDead", nullptr);
        const char* p2 = config_get_str("FastFlags", "FFlagsPruneDead", nullptr);
        if (p1 && !p2) prune = config_get_bool("TASX", "FFlagsPruneDead", 0) != 0;
        else if (!p1 && p2) prune = config_get_bool("FastFlags", "FFlagsPruneDead", 0) != 0;
        if (prune && canWriteGraphics()) {
            static const char* deadKeys[] = {
                "DFFlagDebugDisableTelemetryEphemeralCounter",
                "DFFlagDebugDisableTelemetryEphemeralEvent",
                "DFFlagDebugDisableTelemetryV2Counter",
                "DFFlagDebugDisableTelemetryV2Event",
                "DFFlagDebugDisableTelemetryV2Stat",
                "FFlagStreamingEnableAssetStreaming",
                "FFlagStreamingEnableInstanceStreaming",
                "FFlagStreamingEnableOcclusionCulling",
                "FFlagStreamingEnableHarmony",
                "DFIntUseLevelOfDetail", "FFlagDebugUseLevelOfDetail",
                "FFlagPhysicsReplicationRate", "DFIntPhysicsReplicationRate",
                "FFlagOptimizeServerTickRate",
                "DFFlagDebugRenderForceTechnologyVoxel",
                "FFlagDebugGraphicsPreferD3D11FL10",
                "FFlagDebugGraphicsDisableDirect3D11",
                "FFlagRenderGpuTextureCompressor",
                "DFIntCSGLevelOfDetail", "FIntFRMMaxGrass23",
                "FFlagFastCluster", "DFIntAnimationLodFacs",
                "FFlagFixGraphicsQuality",
            };
            int n = 0;
            for (size_t i = 0; i < sizeof(deadKeys) / sizeof(deadKeys[0]); ++i) {
                plan.remove.push_back(deadKeys[i]);
                ++n;
            }
            if (LogRateLimit("fflags-prune", 3600))
                LOGI("[FFlags] PruneDead ON: tracking %d dead keys for removal (preset %s)",
                     n, preset.empty() ? "legacy" : preset.c_str());
        }
    }
}

bool ApplyToVersion(const std::wstring& versionDir, const FlagPlan& plan)
{
    std::wstring clientDir = versionDir + L"\\ClientSettings";
    std::wstring jsonPath = clientDir + L"\\ClientAppSettings.json";

    // mtime+size pre-check: skip the read entirely when neither the file
    // nor our plan changed since the last pass (farm spawns 100 clients -
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
        // Deleted/locked version dir during Roblox update - not fatal.
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
    if (find == INVALID_HANDLE_VALUE) return; // removed/unavailable root - not fatal

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

/* File-level dirty tracking (P0 debounce) - outside namespace */
static ULONGLONG s_lastFFlagsMs = 0;
static int s_fflagsDirty = 0;

void FFlagsMarkDirty()
{
    s_fflagsDirty = 1;
}

void FFlagsApply()
{
    ULONGLONG now = GetTickCount64();
    int minIntervalMs = 5000;
    bool dirty = (s_fflagsDirty != 0);
    s_fflagsDirty = 0;
    if (!dirty && (now - s_lastFFlagsMs) < (ULONGLONG)minIntervalMs)
        return; // debounced: no dirty/new spawn since last pass
    s_lastFFlagsMs = now;

    // Same compat rule as BuildFlagPlan: missing key -> 1 legacy, 0 with farm preset.
    const char* pPre = config_get_str("FastFlags", "Preset", nullptr);
    if (!pPre) pPre = config_get_str("TASX", "Preset", nullptr);
    bool hasPre = false;
    if (pPre) {
        char pl[16] = {};
        size_t pn = 0;
        for (; pPre[pn] && pn + 1 < sizeof(pl); ++pn)
            pl[pn] = (char)tolower((unsigned char)pPre[pn]);
        hasPre = (strcmp(pl, "farm15") == 0 || strcmp(pl, "farm20") == 0 ||
                  strcmp(pl, "farm30") == 0 || strcmp(pl, "weak") == 0 ||
                  strcmp(pl, "balanced") == 0);
    }
    const char* i1 = config_get_str("TASX", "InjectorOwnsGraphics", nullptr);
    const char* i2 = config_get_str("FastFlags", "InjectorOwnsGraphics", nullptr);
    bool inj;
    if (!i1 && !i2) inj = hasPre ? false : true;
    else if (i1 && !i2) inj = config_get_bool("TASX", "InjectorOwnsGraphics", 1) != 0;
    else if (!i1 && i2) inj = config_get_bool("FastFlags", "InjectorOwnsGraphics", 1) != 0;
    else inj = config_get_bool("TASX", "InjectorOwnsGraphics", 1) ||
               config_get_bool("FastFlags", "InjectorOwnsGraphics", 0);
    bool force = config_get_bool("TASX", "ForceGraphicsFlags", 0) ||
                 config_get_bool("FastFlags", "ForceGraphicsFlags", 0);
    if (inj && !force) {
        if (LogRateLimit("fflags-injector", 3600))
            LOGI("[FFlags] InjectorOwnsGraphics=1 (ForceGraphicsFlags=0) - graphics flags skipped, alive non-graphics still applied");
    } else if (inj && force) {
        if (LogRateLimit("fflags-force", 3600))
            LOGI("[FFlags] ForceGraphicsFlags=1 - injector graphics ownership overridden, all flags applied");
    }
    FlagPlan plan;
    BuildFlagPlan(plan);
    // Effective-set verification (hourly): what the client can actually see.
    if (LogRateLimit("fflags-effective", 3600)) {
        int alive = 0, ph = 0;
        for (auto& kv : plan.set) {
            if (IsFpsPlaceholder(kv.first)) ++ph;
            else ++alive;
        }
        LOGI("[FFlags] Effective set: %d alive (allowlist) + %d FPS-placeholder, preset %s",
             alive, ph, (pPre && *pPre) ? pPre : "legacy");
    }
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
