#include "tweaks.h"

#include "config.h"
#include "ntsys.h"
#include "log.h"
#include "lograte.h"

#include <windows.h>
#include <string>

namespace {

void SetRegValueDWORD(HKEY root, const wchar_t* path, const wchar_t* name,
                      DWORD value, const char* label)
{
    HKEY key = nullptr;
    DWORD cur = 0, type = 0, size = sizeof(cur);

    LONG qr = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (qr == ERROR_FILE_NOT_FOUND)
        qr = RegCreateKeyExW(root, path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (qr != ERROR_SUCCESS) {
        LOGW("[TASX] Tweak '%s' skipped (registry access denied)", label);
        return;
    }

    qr = RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)&cur, &size);
    if (qr == ERROR_SUCCESS && type == REG_DWORD && cur == value) {
        RegCloseKey(key);
        return; /* already correct — keep the log quiet */
    }

    if (RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value,
                       sizeof(value)) == ERROR_SUCCESS)
        LOGI("[TASX] Tweak applied: %s", label);
    else
        LOGW("[TASX] Tweak '%s' write failed", label);

    RegCloseKey(key);
}

void SetRegValueString(HKEY root, const wchar_t* path, const wchar_t* name,
                       const wchar_t* value, const char* label)
{
    HKEY key = nullptr;
    wchar_t cur[256] = {};
    DWORD type = 0, size = sizeof(cur) - sizeof(wchar_t);

    LONG qr = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (qr == ERROR_FILE_NOT_FOUND)
        qr = RegCreateKeyExW(root, path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (qr != ERROR_SUCCESS) {
        LOGW("[TASX] Tweak '%s' skipped (registry access denied)", label);
        return;
    }

    qr = RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)cur, &size);
    if (qr == ERROR_SUCCESS && type == REG_SZ && wcscmp(cur, value) == 0) {
        RegCloseKey(key);
        return;
    }

    DWORD bytes = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
    if (RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value, bytes) == ERROR_SUCCESS)
        LOGI("[TASX] Tweak applied: %s", label);
    else
        LOGW("[TASX] Tweak '%s' write failed", label);

    RegCloseKey(key);
}

/* Dynamic PowrProf bindings (header availability differs across SDKs). */
typedef unsigned long (WINAPI *PowerGetActiveSchemeFn)(unsigned char*, GUID**);
typedef unsigned long (WINAPI *PowerSetActiveSchemeFn)(unsigned char*, const GUID*);

/* Desktop heap expansion for 100+ windowed clients. Touches the csrss
   boot parameters (SharedSection=a,b,c -> b is the interactive desktop
   heap in KB). OPT-IN ONLY: a malformed write here can break booting.
   The edit is purely numeric, everything else stays byte-identical. */
void ExpandDesktopHeap()
{
    if (!config_get_bool("TASX", "DesktopHeapExpand", 0)) return;

    const wchar_t* subkey =
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\SubSystems";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;

    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, L"Windows", nullptr, &type, nullptr, &size) !=
            ERROR_SUCCESS ||
        type != REG_EXPAND_SZ || size < 8 || size > 4096)
    {
        RegCloseKey(key);
        return;
    }

    std::wstring val(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, L"Windows", nullptr, &type, (LPBYTE)&val[0],
                         &size) != ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return;
    }
    if (!val.empty() && val.back() == L'\0') val.pop_back();

    size_t ss = val.find(L"SharedSection=");
    if (ss == std::wstring::npos) { RegCloseKey(key); return; }
    size_t p = ss + 14;

    /* first number (a) */
    while (p < val.size() && iswdigit(val[p])) ++p;
    if (p >= val.size() || val[p] != L',') { RegCloseKey(key); return; }
    size_t bStart = ++p;
    while (p < val.size() && iswdigit(val[p])) ++p;
    if (p == bStart) { RegCloseKey(key); return; }
    int heapKB = _wtoi(val.substr(bStart, p - bStart).c_str());

    const int targetKB = 30720; /* 30 MB interactive desktop heap */
    if (heapKB >= targetKB) { RegCloseKey(key); return; }

    val = val.substr(0, bStart) + std::to_wstring(targetKB) + val.substr(p);
    DWORD bytes = (DWORD)((val.size() + 1) * sizeof(wchar_t));

    if (RegSetValueExW(key, L"Windows", 0, REG_EXPAND_SZ,
                       (const BYTE*)val.c_str(), bytes) == ERROR_SUCCESS)
        LOGI("[TASX] Desktop heap expanded to %d KB (reboot required)", targetKB);

    RegCloseKey(key);
}

PowerGetActiveSchemeFn GetPowerGetActiveScheme(HMODULE powr)
{
    return TasxProcFn<PowerGetActiveSchemeFn>(powr, "PowerGetActiveScheme");
}

PowerSetActiveSchemeFn GetPowerSetActiveScheme(HMODULE powr)
{
    return TasxProcFn<PowerSetActiveSchemeFn>(powr, "PowerSetActiveScheme");
}

const GUID kUltimatePerfGuid =
    { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };
const GUID kHighPerfGuid =
    { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };

GUID g_prevScheme = {};
bool  g_prevSaved = false;

/* Full path of an installed RobloxPlayerBeta.exe for UserGpuPreferences
   (the key works best with a full path). First match under
   %LOCALAPPDATA%\Roblox\Versions; falls back to the bare exe name. */
std::wstring FindRobloxPlayerExe()
{
    wchar_t la[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH))
        return L"RobloxPlayerBeta.exe";

    std::wstring versions = std::wstring(la) + L"\\Roblox\\Versions";
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((versions + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
        return L"RobloxPlayerBeta.exe";

    std::wstring hit;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        std::wstring exe = versions + L"\\" + fd.cFileName +
                           L"\\RobloxPlayerBeta.exe";
        if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            hit = exe;
            break;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    return hit.empty() ? L"RobloxPlayerBeta.exe" : hit;
}

} /* namespace */

void TweaksApplyOneShot()
{
    if (!config_get_bool("TASX", "ApplyTweaks", 1)) return;

    bool elevated = tasx_is_elevated() != 0;
    if (!elevated && LogRateLimit("tweaks-hklm-skip", 3600))
        LOGW("[TASX] HKLM tweaks skipped (needs admin) — HKCU tweaks still applied");

    /* Game DVR off — its background capture pipeline costs FPS. */
    SetRegValueDWORD(HKEY_CURRENT_USER,
        L"System\\GameConfigStore", L"GameDVR_Enabled", 0, "GameDVR_Enabled=0");
    SetRegValueDWORD(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
        L"AppCaptureEnabled", 0, "AppCaptureEnabled=0");
    if (elevated)
        SetRegValueDWORD(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR",
            L"AllowGameDVR", 0, "AllowGameDVR policy=0");

    /* Route Roblox to the high-performance GPU (full exe path when an
       install is found, bare name otherwise). */
    SetRegValueString(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
        FindRobloxPlayerExe().c_str(), L"GpuPreference=2;",
        "Roblox GPU preference = High performance");

    /* Game Mode on — Windows itself deprioritizes background work in game. */
    SetRegValueDWORD(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\GameBar",
        L"AutoGameModeEnabled", 1, "Game Mode enabled");
    SetRegValueDWORD(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\GameBar",
        L"AllowAutoGameMode", 1, "AllowAutoGameMode=1");

    /* MMCSS: no network throttling, system tuned for foreground work. */
    if (elevated) {
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
        L"NetworkThrottlingIndex", 0xFFFFFFFFu, "NetworkThrottlingIndex=off");
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
        L"SystemResponsiveness", 0, "SystemResponsiveness=0");

    /* MMCSS 'Games' task profile: max GPU/CPU scheduling priority. */
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
        L"GPU Priority", 8, "Games: GPU Priority=8");
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
        L"Priority", 6, "Games: Priority=6");
    SetRegValueString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
        L"Scheduling Category", L"High", "Games: Scheduling Category=High");
    SetRegValueString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
        L"SFIO Priority", L"High", "Games: SFIO Priority=High");

    /* Windowed-farm limits: per-process GDI/USER quotas (default 10 000
       gets exhausted around ~50 windowed clients). Reboot required. */
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
        L"GDIProcessHandleQuota", 65536, "GDIProcessHandleQuota=65536");
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
        L"USERProcessHandleQuota", 18000, "USERProcessHandleQuota=18000");

    ExpandDesktopHeap();
    }

    LOGI("[TASX] One-shot tweaks done%s",
         elevated ? " (HKCU+HKLM)" : " (HKCU only, HKLM needs admin)");
}

void TweaksPowerEnter()
{
    if (!config_get_bool("TASX", "PowerPlan", 1)) return;
    if (g_prevSaved) return;

    HMODULE powr = LoadLibraryW(L"PowrProf.dll");
    if (!powr) return;

    auto get = GetPowerGetActiveScheme(powr);
    auto set = GetPowerSetActiveScheme(powr);
    if (get && set) {
        GUID* cur = nullptr;
        if (get(0, &cur) == 0 && cur) {
            g_prevScheme = *cur;
            LocalFree(cur);
            g_prevSaved = true;
        }

        if (set(0, &kUltimatePerfGuid) != 0)
            set(0, &kHighPerfGuid);

        LOGI("[TASX] Power scheme -> Ultimate/High performance");
    }

    FreeLibrary(powr);
}

void TweaksPowerExit()
{
    if (!g_prevSaved) return;
    g_prevSaved = false;

    HMODULE powr = LoadLibraryW(L"PowrProf.dll");
    if (!powr) return;

    auto set = GetPowerSetActiveScheme(powr);
    if (set && set(0, &g_prevScheme) == 0)
        LOGI("[TASX] Power scheme restored");

    FreeLibrary(powr);
}
