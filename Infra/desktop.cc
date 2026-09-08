#include "desktop.h"

#include <chrono>
#include <cstdint>
#include <iostream>
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

} /* namespace */

void DesktopCheckClient(DWORD pid, HANDLE hProc)
{
    if (!hProc || hProc == INVALID_HANDLE_VALUE) return;

    DWORD gdi = GetGuiResources(hProc, GR_GDIOBJECTS);
    DWORD usr = GetGuiResources(hProc, GR_USEROBJECTS);

    /* Default per-process quotas: GDI 10 000, USER 10 000. Warn at 80%. */
    if (gdi >= 8000 || usr >= 8000)
    {
        std::int64_t now = NowMs();
        auto it = g_lastWarnMs.find(pid);
        if (it != g_lastWarnMs.end() && now - it->second < 600000)
            return; /* rate limit: one warning per 10 min per client */
        g_lastWarnMs[pid] = now;

        std::cout << "[Desktop] WARNING: PID " << pid
                  << " near handle quota (GDI=" << gdi << ", USER=" << usr
                  << "). Raise GDIProcessHandleQuota/USERProcessHandleQuota"
                     " (TASX one-shot tweak) and reboot." << std::endl;
    }
}
