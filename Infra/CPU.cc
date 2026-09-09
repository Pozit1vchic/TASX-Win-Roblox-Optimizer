#define NOMINMAX

#include "CPU.h"

#include "config.h"
#include "ntsys.h"
#include "log.h"

#include <iostream>
#include <psapi.h>

#ifdef _MSC_VER
#pragma comment(lib, "PowrProf.lib")
#endif

namespace {

/* Single source of truth for topology is ntsys.c (tasx_get_*_mask).
   This module keeps NO topology state of its own. */

bool g_summaryLogged = false;

unsigned PopCount(unsigned long long m)
{
    unsigned c = 0;
    while (m) { m &= m - 1; ++c; }
    return c;
}

void LogSummary()
{
    if (g_summaryLogged) return;
    g_summaryLogged = true;

    unsigned long long all =
        (unsigned long long)tasx_get_all_mask();
    unsigned long long pMask =
        (unsigned long long)tasx_get_pcore_mask();
    unsigned long long eMask =
        (unsigned long long)tasx_get_ecore_mask();

    if (!all) {
        LOGW("[TASX] CPU topology unavailable, scheduling profiles degraded");
        return;
    }

    unsigned logical = PopCount(all);
    int hybrid = (eMask && eMask != all) ? 1 : 0;

    if (hybrid)
        LOGI("[TASX] CPU: %u logical cores | hybrid: %u P-cores, %u E-cores",
             logical, PopCount(pMask), PopCount(eMask));
    else
        LOGI("[TASX] CPU: %u logical cores (non-hybrid, background -> low half)",
             logical);
    LOGI("[TASX] Focus profile -> all cores + HIGH priority; background -> %s + IDLE + efficiency mode",
         hybrid ? "E-cores" : "low half");
}

} /* namespace */

void CpuInit(void)
{
    LogSummary();
}

DWORD_PTR CpuAllMask(void)
{
    return (DWORD_PTR)tasx_get_all_mask();
}

int CpuIsHybrid(void)
{
    unsigned long long eMask = (unsigned long long)tasx_get_ecore_mask();
    unsigned long long all = (unsigned long long)tasx_get_all_mask();
    return (eMask && eMask != all) ? 1 : 0;
}

DWORD CpuLogicalCount(void)
{
    return PopCount((unsigned long long)tasx_get_all_mask());
}

DWORD_PTR CpuBackgroundMask(void)
{
    return (DWORD_PTR)tasx_get_ecore_mask(); /* alias: ntsys owns the mask */
}

void CpuSetBoostMode(int enable)
{
    HMODULE hPowrProf = LoadLibraryW(L"PowrProf.dll");
    if (!hPowrProf) return;

    using PowerSetInformationFn = LONG (WINAPI*)(HANDLE, int, PVOID, ULONG);
    auto fn = TasxProcFn<PowerSetInformationFn>(hPowrProf, "PowerSetInformation");
    if (fn) {
        DWORD boost = enable ? 1 : 0;
        if (fn(nullptr, 35 /* ProcessorPerformanceBoostMode */, &boost,
               sizeof(boost)) == 0)
            LOGI("[TASX] CPU boost mode %s", enable ? "enabled" : "disabled");
        else
            LOGW("[TASX] CPU boost mode change failed (needs admin)");
    }
    FreeLibrary(hPowrProf);
}

bool CpuApplyFocusProfile(HANDLE hProcess, int focused)
{
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) return false;

    LogSummary();

    DWORD pid = GetProcessId(hProcess);
    DWORD_PTR pMask = (DWORD_PTR)tasx_get_pcore_mask();
    DWORD_PTR eMask = (DWORD_PTR)tasx_get_ecore_mask();
    DWORD_PTR allMask = (DWORD_PTR)tasx_get_all_mask();

    if (focused) {
        if (!SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS))
            LOGW("[TASX] PID %lu HIGH priority failed | Code: %lu", pid,
                 (unsigned long)GetLastError());

        DWORD_PTR target = pMask ? pMask : allMask;
        if (target && !SetProcessAffinityMask(hProcess, target))
            LOGW("[TASX] PID %lu p-core affinity failed | Code: %lu", pid,
                 (unsigned long)GetLastError());

        /* Exempt from EcoQoS / power throttling — the focused game gets
           the turbo it pays for (process-level only, no thread enumeration). */
        tasx_process_power_throttling(hProcess, 0);
        tasx_set_memory_priority(hProcess, 5 /* Normal */);

        LOGI("[TASX] PID %lu -> FOCUSED profile (HIGH priority, p-cores, boost exempt)",
             pid);
        return true;
    }

    if (!SetPriorityClass(hProcess, IDLE_PRIORITY_CLASS))
        LOGW("[TASX] PID %lu IDLE priority failed | Code: %lu", pid,
             (unsigned long)GetLastError());

    if (config_get_bool("TASX", "PinBackgroundToECores", 1)) {
        DWORD_PTR bgMask = eMask ? eMask : allMask;
        if (bgMask) {
            if (SetProcessAffinityMask(hProcess, bgMask))
                LOGI("[TASX] PID %lu pinned to %u background (E-)cores", pid,
                     PopCount((unsigned long long)bgMask));
            else
                LOGW("[TASX] PID %lu affinity failed | Code: %lu", pid,
                     (unsigned long)GetLastError());
        }
    }

    /* Efficiency mode: process-level EcoQoS only (no per-thread enumeration). */
    tasx_process_power_throttling(hProcess, 1);

    tasx_set_io_priority(hProcess, 0 /* VeryLow */);
    tasx_set_memory_priority(hProcess, 1 /* VeryLow */);

    LOGI("[TASX] PID %lu -> BACKGROUND profile (IDLE, EcoQoS, E-cores, low I/O+mem)",
         pid);
    return true;
}
