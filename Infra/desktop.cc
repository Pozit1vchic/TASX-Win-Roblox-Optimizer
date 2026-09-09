#include "desktop.h"

#include "log.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#ifndef GR_GDIOBJECTS
#define GR_GDIOBJECTS   0
#define GR_USEROBJECTS  1
#endif

namespace {

std::unordered_map<DWORD, std::int64_t> g_lastWarnMs;

std::int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* Real per-process GDI quota (HKLM value set by the TASX one-shot tweak,
   Windows default 10000). Read once; warn at 80% of the actual quota.
   USER quota is tracked separately (its default is also 10000). */
DWORD QuotaFromReg(const wchar_t* name, DWORD fallback)
{
    static std::unordered_map<std::wstring, DWORD> cache;
    auto it = cache.find(name);
    if (it != cache.end())
        return it->second;
    DWORD v = fallback;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD raw = 0, type = 0, size = sizeof(raw);
        if (RegQueryValueExW(key, name, nullptr, &type,
                             (LPBYTE)&raw, &size) == ERROR_SUCCESS &&
            type == REG_DWORD && raw > 0)
            v = raw;
        RegCloseKey(key);
    }
    cache[name] = v;
    return v;
}

DWORD GdiQuota()  { return QuotaFromReg(L"GDIProcessHandleQuota", 10000); }
DWORD UserQuota() { return QuotaFromReg(L"USERProcessHandleQuota", 10000); }

} /* namespace */

void DesktopCheckClient(DWORD pid, HANDLE hProc)
{
    if (!hProc || hProc == INVALID_HANDLE_VALUE) return;

    DWORD gdi = GetGuiResources(hProc, GR_GDIOBJECTS);
    DWORD usr = GetGuiResources(hProc, GR_USEROBJECTS);

    DWORD warnGdi  = GdiQuota() * 8 / 10;
    DWORD warnUser = UserQuota() * 8 / 10;
    if (gdi >= warnGdi || usr >= warnUser)
    {
        std::int64_t now = NowMs();
        auto it = g_lastWarnMs.find(pid);
        if (it != g_lastWarnMs.end() && now - it->second < 600000)
            return; /* rate limit: one warning per 10 min per client */
        g_lastWarnMs[pid] = now;

        LOGW("[Desktop] WARNING: PID %lu near handle quota (GDI=%lu/%lu, USER=%lu/%lu). Raise quotas (TASX one-shot tweak) and reboot.",
             pid, gdi, (unsigned long)GdiQuota(), usr, (unsigned long)UserQuota());
    }
}
