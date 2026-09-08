#include "jobs.h"

#include "config.h"
#include "ntsys.h"
#include "lograte.h"

#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef JOB_OBJECT_MSG_EXIT_PROCESS
#define JOB_OBJECT_MSG_EXIT_PROCESS 7
#endif
#ifndef JOB_OBJECT_MSG_NEW_PROCESS
#define JOB_OBJECT_MSG_NEW_PROCESS 6
#endif

namespace {

std::mutex g_mtx;
HANDLE g_jobFocus = nullptr;
HANDLE g_jobBackground = nullptr;
HANDLE g_iocp = nullptr;
HANDLE g_iocpThread = nullptr;
DWORD  g_iocpThreadId = 0;
std::unordered_map<DWORD, int> g_pidJob; // pid -> 0 bg, 1 focus
std::unordered_set<DWORD> g_knownPids;

/* Background policy cache for JobsRefreshDynamic */
DWORD_PTR g_dynamicBgMask = 0;
bool      g_bgRateCap = false;

DWORD_PTR GetBgMaskForCurrentPolicy(int anyFocused)
{
    unsigned long long pMask = tasx_get_pcore_mask();
    unsigned long long eMask = tasx_get_ecore_mask();
    unsigned long long allMask = tasx_get_all_mask();

    if (!config_get_bool("TASX", "DynamicAffinity", 1)) {
        // static: E-cores if hybrid else low half
        if (eMask && eMask != allMask) return (DWORD_PTR)eMask;
        // fallback low half computed in ntsys via eMask already
        return (DWORD_PTR)(eMask ? eMask : allMask);
    }
    if (anyFocused) {
        if (eMask && eMask != allMask) return (DWORD_PTR)eMask;
        return (DWORD_PTR)(eMask ? eMask : allMask);
    } else {
        // farm: all cores, optionally capped
        return (DWORD_PTR)allMask;
    }
}

bool ApplyLimitsToJob(HANDLE job, bool focused, DWORD_PTR bgMaskForBg)
{
    if (!job) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext{};
    ext.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PRIORITY_CLASS | JOB_OBJECT_LIMIT_AFFINITY;

    if (focused) {
        ext.BasicLimitInformation.PriorityClass = HIGH_PRIORITY_CLASS;
        unsigned long long allMask = tasx_get_all_mask();
        ext.BasicLimitInformation.Affinity = allMask ? (DWORD_PTR)allMask : (DWORD_PTR)-1;
    } else {
        ext.BasicLimitInformation.PriorityClass = IDLE_PRIORITY_CLASS;
        DWORD_PTR bg = bgMaskForBg ? bgMaskForBg : (DWORD_PTR)tasx_get_ecore_mask();
        if (!bg) bg = (DWORD_PTR)tasx_get_all_mask();
        ext.BasicLimitInformation.Affinity = bg ? bg : (DWORD_PTR)-1;

        // Group memory cap (whole job). Legacy key JobMemoryCapMB + new handling.
        int memCap = config_get_int("TASX", "JobMemoryCapMB", 0);
        if (memCap > 0) {
            ext.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            ext.ProcessMemoryLimit = (SIZE_T)memCap << 20;
        }
    }

    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &ext, sizeof(ext))) {
        return false;
    }

    if (!focused) {
        // CPU cap for background job: BackgroundCpuCapPercent (new) else JobCpuCapPercent (legacy)
        int cap = config_get_int("TASX", "BackgroundCpuCapPercent", -1);
        if (cap < 0) cap = config_get_int("TASX", "JobCpuCapPercent", 0);
        if (cap <= 0) cap = config_get_int("TASX", "BackgroundCpuCapPercent", 0);
        // default 20 if not set and anyFocused==0? spec says default 20%
        if (cap <= 0 && cap != 0) {
            // if key missing, use 20 as default for background job
            const char* v = config_get_str("TASX", "BackgroundCpuCapPercent", nullptr);
            if (!v) v = config_get_str("TASX", "JobCpuCapPercent", nullptr);
            if (!v) cap = 20;
        }
        if (cap > 0 && cap < 100) {
            // try via tasx_job_set_cpu_rate (uses HARD_CAP)
            if (!tasx_job_set_cpu_rate(job, (unsigned long)cap)) {
                // fallback direct
                JOBOBJECT_CPU_RATE_CONTROL_INFORMATION crc{};
                crc.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE
                                 | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
                crc.CpuRate = (DWORD)cap * 100;
                SetInformationJobObject(job, JobObjectCpuRateControlInformation,
                                        &crc, sizeof(crc));
            }
        }
        // IO priority group. BUG 10: tasx_job_set_io_priority returns 0
        // ("not applied, use per-process"), so enforce VeryLow on every
        // assigned process ourselves.
        if (tasx_job_set_io_priority(job, 0) == 0) {
            for (const auto& kv : g_pidJob) {
                HANDLE hp = OpenProcess(PROCESS_SET_INFORMATION, FALSE, kv.first);
                if (!hp) continue;
                tasx_set_io_priority(hp, 0); /* VeryLow for background */
                CloseHandle(hp);
            }
        }

        // ETW suppression for background
        if (config_get_bool("ETW", "DisableTelemetry", 1) ||
            config_get_bool("TASX", "DisableTelemetry", 0)) {
            tasx_etw_disable_provider(L"Microsoft-Windows-Diagnostics-Performance");
            tasx_etw_disable_provider(L"Microsoft-Windows-Kernel-Processor-Power");
        }
    } else {
        // Focus: ensure no cap
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION crc{};
        crc.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE;
        crc.CpuRate = 10000; // 100%
        SetInformationJobObject(job, JobObjectCpuRateControlInformation, &crc, sizeof(crc));
    }
    return true;
}

DWORD WINAPI IocpLoop(LPVOID)
{
    for (;;) {
        DWORD code = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = nullptr;
        BOOL ok = GetQueuedCompletionStatus(g_iocp, &code, &key, &ov, INFINITE);
        if (!ok) {
            if (GetLastError() == WAIT_TIMEOUT) continue;
            if (!g_iocp) break;
            // port closed -> exit
            if (GetLastError() == ERROR_ABANDONED_WAIT_0 || GetLastError() == ERROR_INVALID_HANDLE)
                break;
            continue;
        }
        // With 2 global jobs, key is 0=bg/1=focus, pid is in ov
        DWORD pid = (DWORD)(ULONG_PTR)ov;
        if (code == JOB_OBJECT_MSG_EXIT_PROCESS) {
            TasxNotifyJobEvent(1, pid);
        } else if (code == JOB_OBJECT_MSG_NEW_PROCESS) {
            DWORD child = pid;
            // key+code distinction not needed; child != parent check still
            // For global jobs we don't have parent pid in key, so treat any NEW_PROCESS as crash handler candidate
            TasxNotifyJobEvent(2, child);
        }
    }
    return 0;
}

bool CreateGlobalJobs()
{
    g_jobFocus = tasx_job_create();
    g_jobBackground = tasx_job_create();
    if (!g_jobFocus || !g_jobBackground) {
        if (g_jobFocus) { CloseHandle(g_jobFocus); g_jobFocus = nullptr; }
        if (g_jobBackground) { CloseHandle(g_jobBackground); g_jobBackground = nullptr; }
        return false;
    }

    // Associate both with same IOCP
    if (g_iocp) {
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT acp{};
        acp.CompletionPort = g_iocp;
        acp.CompletionKey = (PVOID)(ULONG_PTR)0; // bg
        SetInformationJobObject(g_jobBackground, JobObjectAssociateCompletionPortInformation,
                                &acp, sizeof(acp));
        acp.CompletionKey = (PVOID)(ULONG_PTR)1; // focus
        SetInformationJobObject(g_jobFocus, JobObjectAssociateCompletionPortInformation,
                                &acp, sizeof(acp));
    }

    // Apply initial limits: focus = all cores HIGH, bg = E-cores IDLE + cap
    DWORD_PTR bgMask = GetBgMaskForCurrentPolicy(0);
    bool ok1 = ApplyLimitsToJob(g_jobFocus, true, 0);
    bool ok2 = ApplyLimitsToJob(g_jobBackground, false, bgMask);
    g_dynamicBgMask = bgMask;
    g_bgRateCap = config_get_int("TASX", "BackgroundCpuCapPercent", -1) >= 0 ?
                  config_get_int("TASX", "BackgroundCpuCapPercent", 0) > 0 :
                  config_get_int("TASX", "JobCpuCapPercent", 0) > 0;
    if (!ok1 || !ok2) {
        if (LogRateLimit("jobs-applylimits", 10))
            std::cout << "[Jobs] Warning: ApplyLimits failed focus=" << ok1
                      << " bg=" << ok2 << std::endl;
    }
    return true;
}

} // namespace

bool JobsInit()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_jobFocus && g_jobBackground) return true;

    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!g_iocp) {
        std::cout << "[Jobs] Failed to create completion port | Code: " << GetLastError() << std::endl;
        return false;
    }
    g_iocpThread = CreateThread(nullptr, 0, IocpLoop, nullptr, 0, &g_iocpThreadId);
    if (!g_iocpThread) {
        std::cout << "[Jobs] Failed to create IOCP thread" << std::endl;
        CloseHandle(g_iocp); g_iocp = nullptr;
        return false;
    }

    if (!CreateGlobalJobs()) {
        std::cout << "[Jobs] Failed to create global jobs" << std::endl;
        CloseHandle(g_iocp); g_iocp = nullptr;
        if (g_iocpThread) { TerminateThread(g_iocpThread, 0); CloseHandle(g_iocpThread); g_iocpThread = nullptr; }
        return false;
    }

    std::cout << "[Jobs] 2-job layer ready (focus + background cgroup)" << std::endl;
    return true;
}

bool JobHookProcess(DWORD pid, HANDLE hProc)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_jobBackground) return false;
    if (g_pidJob.find(pid) != g_pidJob.end()) return true; // already tracked

    // Try assign to background job. One syscall.
    if (!tasx_job_assign(g_jobBackground, hProc)) {
        // Fallback: if already in a job (ERROR_ALREADY_ASSIGNED), try direct AssignProcessToJobObject
        // and if still fails, return false for per-process fallback
        DWORD err = GetLastError();
        if (err == ERROR_ALREADY_ASSIGNED || err == ERROR_ACCESS_DENIED) {
            // Process already in a job (maybe from previous TASX run or parent) - try to update limits instead
            // BUG 4: per-PID rate limit - a stuck PID would otherwise spam
            // this line on every discovery/refresh pass.
            static std::unordered_set<DWORD> warnedErr5;
            if (warnedErr5.find(pid) == warnedErr5.end()) {
                warnedErr5.insert(pid);
                std::cout << "[Jobs] PID " << pid << " already in job (err " << err << ") -> fallback to limit rewrite" << std::endl;
            }
            // Still track as background logically
            g_pidJob[pid] = 0;
            g_knownPids.insert(pid);
            return false; // signal caller to use CpuApply fallback? But spec wants return true even if assign fails? Let's return false to trigger fallback.
        }
        std::cout << "[Jobs] PID " << pid << " assign to background failed | Code: " << err << std::endl;
        return false;
    }

    // Success: set per-process one-shot properties that jobs can't express
    tasx_process_power_throttling(hProc, 1);
    tasx_set_io_priority(hProc, 0);
    tasx_set_memory_priority(hProc, 1);

    g_pidJob[pid] = 0;
    g_knownPids.insert(pid);
    std::cout << "[TASX] PID " << pid << " -> background cgroup" << std::endl;
    return true;
}

bool JobApplyProfile(DWORD pid, int focused)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_pidJob.find(pid);
    if (it == g_pidJob.end()) return false; // not job-tracked, caller will use CpuApply

    HANDLE hTarget = focused ? g_jobFocus : g_jobBackground;
    HANDLE hCurrent = (it->second == 1) ? g_jobFocus : g_jobBackground;
    if (hTarget == hCurrent) return true; // already in correct job

    // Need handle to process for assignment
    HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_SET_INFORMATION,
                               FALSE, pid);
    if (!hProc) {
        // process may have exited
        return false;
    }

    bool ok = tasx_job_assign(hTarget, hProc) != 0;
    DWORD err = GetLastError();
    CloseHandle(hProc);

    if (ok) {
        it->second = focused ? 1 : 0;
        // Update power throttling one-shot
        HANDLE h2 = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
        if (h2) {
            tasx_process_power_throttling(h2, focused ? 0 : 1);
            CloseHandle(h2);
        }
        std::cout << "[TASX] PID " << pid << (focused ? " -> FOCUS cgroup" : " -> background cgroup") << std::endl;
        return true;
    } else {
        // Assign failed (already in job). Fallback: rewrite limits of the current job?
        // For sibling jobs, process already in one job cannot be moved to another.
        // Fallback is to rewrite the global job limits? But that would affect all.
        // Instead fallback to per-process CpuApply via return false so master does CpuApplyFocusProfile.
        // Log and keep logical state flipped so next call doesn't retry assign storm.
        // BUG 4: same per-PID rate limit as JobHookProcess.
        static std::unordered_set<DWORD> warnedMoveErr5;
        if (warnedMoveErr5.find(pid) == warnedMoveErr5.end()) {
            warnedMoveErr5.insert(pid);
            std::cout << "[Jobs] PID " << pid << " move to " << (focused?"focus":"bg")
                      << " failed (err " << err << ") -> per-process fallback" << std::endl;
        }
        // Do not update it->second, keep as is, caller will handle via Cpu path
        return false;
    }
}

void JobsRefreshDynamic(int anyFocused)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_jobFocus || !g_jobBackground) return;

    DWORD_PTR target = GetBgMaskForCurrentPolicy(anyFocused);
    bool rateCap = false;
    int cap = config_get_int("TASX", "BackgroundCpuCapPercent", -1);
    if (cap < 0) cap = config_get_int("TASX", "JobCpuCapPercent", 0);
    if (cap < 0) {
        const char* v = config_get_str("TASX", "BackgroundCpuCapPercent", nullptr);
        if (!v) v = config_get_str("TASX", "JobCpuCapPercent", nullptr);
        if (!v) cap = anyFocused ? 0 : 20; // default 20 in farm mode
    }
    rateCap = cap > 0;

    if (target == g_dynamicBgMask && rateCap == g_bgRateCap)
        return;

    g_dynamicBgMask = target;
    g_bgRateCap = rateCap;

    // Rewrite background job limits with new mask/cap. Focus job stays all cores.
    ApplyLimitsToJob(g_jobBackground, false, target);
    // Focus job affinity stays all cores, but re-apply in case topology changed
    ApplyLimitsToJob(g_jobFocus, true, 0);

    std::cout << "[Jobs] Background profile -> "
              << (target == (DWORD_PTR)tasx_get_all_mask()
                  ? (rateCap ? "all cores (capped)" : "all cores")
                  : "E-cores / low half")
              << " rewritten" << std::endl;
}

void JobRelease(DWORD pid)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_pidJob.erase(pid);
    g_knownPids.erase(pid);
    // Do NOT close global jobs here; they are shared.
}

void JobShutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_pidJob.clear();
        g_knownPids.clear();
    }
    if (g_iocp) {
        CloseHandle(g_iocp);
        g_iocp = nullptr;
    }
    if (g_iocpThread) {
        // IOCP loop will exit on port close; wait briefly
        WaitForSingleObject(g_iocpThread, 2000);
        CloseHandle(g_iocpThread);
        g_iocpThread = nullptr;
    }
    if (g_jobFocus) { CloseHandle(g_jobFocus); g_jobFocus = nullptr; }
    if (g_jobBackground) { CloseHandle(g_jobBackground); g_jobBackground = nullptr; }
}
