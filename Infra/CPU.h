#pragma once

#include <windows.h>

/* Applies the full TASX scheduling profile to a Roblox process:
   focused   -> HIGH priority, all (P-)cores, throttling exempted,
                normal I/O & memory priority.
   background-> IDLE priority, E-cores (or the low half of the CPU),
                efficiency mode + VeryLow I/O/memory priority.
   Call again whenever focus changes; cheap when nothing changed. */
bool CpuApplyFocusProfile(HANDLE hProcess, int focused);

/* Page-in all committed private pages of a process (pull from pagefile). */
struct CpuPageInStats {
    unsigned long long attemptedPages = 0;
    unsigned long long touchedPages = 0;
    unsigned long long failedPages = 0;
    unsigned long long skippedPages = 0;
    unsigned long long bytesTouched = 0;
};
bool CpuPageInProcess(HANDLE hProcess, CpuPageInStats* stats);

/* Background memory priority 1..5: BackgroundMemPriority key, default
   Low(2) on farm presets, VeryLow(1) otherwise. Single source of truth
   for CPU.cc profiles and the trimmer. */
unsigned long CpuBackgroundMemPrio(void);

/* Global CPU boost toggle (system-wide power setting). */
void CpuSetBoostMode(int enable);

/* One-time topology discovery + console summary. */
void CpuInit(void);

/* Topology accessors for Job Object limits (init lazily). */
DWORD_PTR CpuAllMask(void);        /* every logical CPU (group 0)        */
DWORD_PTR CpuBackgroundMask(void); /* E-cores on hybrid, low half else   */
int       CpuIsHybrid(void);
DWORD     CpuLogicalCount(void);
