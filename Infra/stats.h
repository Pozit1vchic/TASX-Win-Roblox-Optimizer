#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct ProcStat {
    DWORD   pid          = 0;
    SIZE_T  workingSet   = 0;   /* bytes */
    SIZE_T  privateBytes = 0;   /* bytes */
    std::wstring name;          /* exe name, e.g. L"RobloxPlayerBeta.exe" */
};

/* Snapshot of every process in ONE syscall (NtQuerySystemInformation).
   Used for memory stats, discovery fallback and crash-handler sweeps at
   100+ clients, replacing per-process GetProcessMemoryInfo and full
   CreateToolhelp32Snapshot scans. */
bool QueryProcStats(std::vector<ProcStat>& out);

/* Full image path of one PID ("" when not accessible). Used by the job
   completion-port filter and the crash-handler sweep. */
std::wstring QueryProcessNameByPid(DWORD pid);
