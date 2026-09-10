#define NOMINMAX

#include "CPU.h"

#include "config.h"
#include "ntsys.h"
#include "log.h"

#include <iostream>
#include <psapi.h>
#include <vector>

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

unsigned long CpuBackgroundMemPrio(void)
{
    const char* v = config_get_str("TASX", "BackgroundMemPriority", nullptr);
    if (v) {
        int p = config_get_int("TASX", "BackgroundMemPriority", 1);
        if (p < 1) p = 1;
        if (p > 5) p = 5;
        return (unsigned long)p;
    }
    const char* pPre = config_get_str("FastFlags", "Preset", nullptr);
    if (!pPre) pPre = config_get_str("TASX", "Preset", nullptr);
    if (pPre) {
        char pl[16] = {};
        size_t pn = 0;
        for (; pPre[pn] && pn + 1 < sizeof(pl); ++pn) {
            char c = pPre[pn];
            if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
            pl[pn] = c;
        }
        if (strcmp(pl, "farm15") == 0 || strcmp(pl, "farm20") == 0 ||
            strcmp(pl, "farm30") == 0)
            return 2ul;
    }
    return 1ul;
}

bool CpuPageInProcess(HANDLE hProcess, CpuPageInStats* stats)
{
    if (!stats || !hProcess || hProcess == INVALID_HANDLE_VALUE) return false;
    *stats = {};
    SYSTEM_INFO si{}; GetNativeSystemInfo(&si);
    const SIZE_T page = si.dwPageSize ? si.dwPageSize : 4096;
    std::vector<unsigned char> buffer(256 * 1024);
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t base = 0;
    while (true) {
        SIZE_T queried = VirtualQueryEx(hProcess, (LPCVOID)base, &mbi, sizeof(mbi));
        if (!queried) break;
        uintptr_t regionBase = (uintptr_t)mbi.BaseAddress;
        uintptr_t regionEnd = regionBase + (uintptr_t)mbi.RegionSize;
        if (regionEnd < regionBase || regionEnd <= regionBase) { base = regionEnd; continue; }
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
            DWORD p = mbi.Protect & 0xFF;
            bool readable = (p != PAGE_NOACCESS && p != PAGE_GUARD);
            if (readable) {
                uintptr_t start = (regionBase + page - 1) & ~(uintptr_t)(page - 1);
                if (start < regionEnd) {
                    SIZE_T totalPages = (SIZE_T)((regionEnd - start + page - 1) / page);
                    stats->attemptedPages += totalPages;
                    for (uintptr_t pos = start; pos < regionEnd; ) {
                        SIZE_T req = (SIZE_T)std::min<ULONGLONG>(256ULL * 1024, (ULONGLONG)(regionEnd - pos));
                        SIZE_T got = 0;
                        if (ReadProcessMemory(hProcess, (LPCVOID)pos, buffer.data(), req, &got)) {
                            SIZE_T pages = got / page;
                            stats->touchedPages += pages;
                            stats->bytesTouched += got;
                            if (got < req) {
                                uintptr_t fb = pos + pages * page;
                                for (; fb < pos + req; fb += page) {
                                    unsigned char b = 0; SIZE_T one = 0;
                                    if (ReadProcessMemory(hProcess, (LPCVOID)fb, &b, 1, &one) && one) {
                                        stats->touchedPages += 1; stats->bytesTouched += 1;
                                    } else {
                                        stats->failedPages += 1;
                                    }
                                }
                            }
                        } else {
                            SIZE_T pagesInReq = (SIZE_T)((req + page - 1) / page);
                            stats->failedPages += pagesInReq;
                        }
                        pos += req;
                    }
                }
            } else {
                uintptr_t start = (regionBase + page - 1) & ~(uintptr_t)(page - 1);
                if (start < regionEnd) {
                    stats->skippedPages += (SIZE_T)((regionEnd - start + page - 1) / page);
                }
            }
        }
        base = regionEnd;
    }
    return true;
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

        DWORD_PTR target = allMask;
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

    DWORD_PTR bgMask = allMask;
    if (bgMask) SetProcessAffinityMask(hProcess, bgMask);

    /* Always P+E cores, no E-core pinning; keep background scheduling but
       disable EcoQoS/throttling so all cores are usable. */
    tasx_process_power_throttling(hProcess, 0);

    tasx_set_io_priority(hProcess, 0 /* VeryLow */);
    tasx_set_memory_priority(hProcess, CpuBackgroundMemPrio());

    LOGI("[TASX] PID %lu -> BACKGROUND profile (IDLE, all cores, low I/O+mem)",
         pid);
    return true;
}
