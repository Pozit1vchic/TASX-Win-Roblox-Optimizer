#include "jobs.h"

#include "config.h"
#include "ntsys.h"
#include "log.h"
#include "lograte.h"
#include "stats.h"

#include <mutex>
#include <string>
#include <unordered_map>

#ifndef JOB_OBJECT_MSG_EXIT_PROCESS
#define JOB_OBJECT_MSG_EXIT_PROCESS 7
#endif
#ifndef JOB_OBJECT_MSG_NEW_PROCESS
#define JOB_OBJECT_MSG_NEW_PROCESS 6
#endif

namespace {

std::mutex g_mtx;
HANDLE g_jobBackground = nullptr;
HANDLE g_iocp = nullptr;
HANDLE g_iocpThread = nullptr;
DWORD  g_iocpThreadId = 0;
/* pid -> 0 in cgroup with background settings, 2 in cgroup with the
   focus profile applied, -1 sticky per-process fallback (foreign job /
   denied, never retried). */
std::unordered_map<DWORD, int> g_pidJob;

/* Background policy cache for JobsRefreshDynamic */
DWORD_PTR g_dynamicBgMask = 0;
bool      g_bgRateCap = false;

/* JobAssignMode values: auto (default, try assign, sticky fallback on foreign Job),
   diagnose (same + log InJob info for broken foreign-job diagnostics),
   off (never assign, pure per-process profile). Deprecated alias: force. */
int GetJobAssignMode()
{
    const char* v = config_get_str("TASX", "JobAssignMode", "auto");
    if (!v || !*v) v = config_get_str("FastFlags", "JobAssignMode", "auto");
    char low[16] = {};
    size_t n = 0;
    for (; v[n] && n + 1 < sizeof(low); ++n) {
        char c = v[n];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        low[n] = c;
    }
    low[n] = '\0';
    if (low[0] == 'o' && (low[1] == 'f' || low[1] == 'f')) return 2; // off
    if ((low[0] == 'd' && low[1] == 'i') || (low[0] == 'f' && low[1] == 'o')) {
        // diagnose or deprecated force
        return 1;
    }
    return 0; // auto
}

bool ShouldLogPerPid(const char* prefix, DWORD pid, int intervalSec)
{
    char tag[64];
    snprintf(tag, sizeof(tag), "%s-%lu", prefix, (unsigned long)pid);
    return LogRateLimit(tag, intervalSec);
}

DWORD_PTR GetBgMaskForCurrentPolicy(int anyFocused)
{
    unsigned long long eMask = tasx_get_ecore_mask();
    unsigned long long allMask = tasx_get_all_mask();

    if (!config_get_bool("TASX", "DynamicAffinity", 1))
        return (DWORD_PTR)eMask;
    if (anyFocused)
        return (DWORD_PTR)eMask;
    /* farm: nothing focused — all cores, optionally rate-capped */
    return (DWORD_PTR)allMask;
}

/* Single readable CPU-cap path: BackgroundCpuCapPercent, legacy fallback
   JobCpuCapPercent, default 20 in farm mode, 0 when something is focused. */
int BgCpuCapPercent(int anyFocused)
{
    int cap = config_get_int("TASX", "BackgroundCpuCapPercent", -1);
    if (cap < 0)
        cap = config_get_int("TASX", "JobCpuCapPercent", 0);
    if (cap < 0) {
        const char* v = config_get_str("TASX", "BackgroundCpuCapPercent", nullptr);
        if (!v)
            v = config_get_str("TASX", "JobCpuCapPercent", nullptr);
        cap = (!v && !anyFocused) ? 20 : 0;
    }
    if (cap < 0)
        cap = 0;
    if (cap > 100)
        cap = 100;
    return cap;
}

bool ApplyLimitsToJob(HANDLE job, DWORD_PTR bgMask, int cpuCap)
{
    if (!job)
        return false;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext{};
    ext.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PRIORITY_CLASS | JOB_OBJECT_LIMIT_AFFINITY;
    ext.BasicLimitInformation.PriorityClass = IDLE_PRIORITY_CLASS;
    ext.BasicLimitInformation.Affinity = bgMask ? bgMask : (DWORD_PTR)-1;

    int memCap = config_get_int("TASX", "JobMemoryCapMB", 0);
    if (memCap > 0) {
        ext.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        ext.ProcessMemoryLimit = (SIZE_T)(unsigned)memCap << 20;
    }

    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &ext, sizeof(ext))) {
        return false;
    }

    if (cpuCap > 0 && cpuCap < 100) {
        if (!tasx_job_set_cpu_rate(job, (unsigned long)cpuCap)) {
            JOBOBJECT_CPU_RATE_CONTROL_INFORMATION crc{};
            crc.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE
                             | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
            crc.CpuRate = (DWORD)cpuCap * 100;
            SetInformationJobObject(job, JobObjectCpuRateControlInformation,
                                    &crc, sizeof(crc));
        }
    } else {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION crc{};
        crc.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE;
        crc.CpuRate = 10000; /* 100% — no cap */
        SetInformationJobObject(job, JobObjectCpuRateControlInformation,
                                &crc, sizeof(crc));
    }
    /* Job-level I/O priority is not portable: per-process VeryLow is set
       once in JobHookProcess for each newly assigned PID. */

    if (config_get_bool("ETW", "DisableTelemetry", 1) ||
        config_get_bool("TASX", "DisableTelemetry", 0)) {
        tasx_etw_disable_provider(L"Microsoft-Windows-Diagnostics-Performance");
        tasx_etw_disable_provider(L"Microsoft-Windows-Kernel-Processor-Power");
    }
    return true;
}

bool NameHas(const std::wstring& full, const wchar_t* needle)
{
    if (full.empty() || !needle)
        return false;
    std::wstring hay = full;
    for (auto& c : hay)
        c = (wchar_t)towlower(c);
    std::wstring ndl = needle;
    for (auto& c : ndl)
        c = (wchar_t)towlower(c);
    return hay.find(ndl) != std::wstring::npos;
}

DWORD WINAPI IocpLoop(LPVOID)
{
    for (;;) {
        DWORD code = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = nullptr;
        (void)key;
        BOOL ok = GetQueuedCompletionStatus(g_iocp, &code, &key, &ov, INFINITE);
        if (!ok) {
            if (!g_iocp)
                break;
            DWORD err = GetLastError();
            if (err == ERROR_ABANDONED_WAIT_0 || err == ERROR_INVALID_HANDLE)
                break;
            continue;
        }
        DWORD pid = (DWORD)(ULONG_PTR)ov;
        if (code == JOB_OBJECT_MSG_EXIT_PROCESS) {
            TasxNotifyJobEvent(1, pid);
        } else if (code == JOB_OBJECT_MSG_NEW_PROCESS) {
            /* A job child spawned. Only the crash handler is of interest —
               never forward arbitrary children (would cause kill storms). */
            std::wstring name = QueryProcessNameByPid(pid);
            if (NameHas(name, L"robloxcrashhandler.exe"))
                TasxNotifyJobEvent(2, pid);
        }
    }
    return 0;
}

bool CreateBackgroundJob()
{
    int killOnClose = config_get_bool("TASX", "KillOnAgentExit", 1);
    g_jobBackground = tasx_job_create_ex(killOnClose ? 1 : 0);
    if (!g_jobBackground)
        return false;

    if (g_iocp) {
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT acp{};
        acp.CompletionPort = g_iocp;
        acp.CompletionKey = nullptr;
        SetInformationJobObject(g_jobBackground,
                                JobObjectAssociateCompletionPortInformation,
                                &acp, sizeof(acp));
    }

    DWORD_PTR bgMask = GetBgMaskForCurrentPolicy(0);
    int cap = BgCpuCapPercent(0);
    bool ok = ApplyLimitsToJob(g_jobBackground, bgMask, cap);
    g_dynamicBgMask = bgMask;
    g_bgRateCap = cap > 0;
    if (!ok && LogRateLimit("jobs-applylimits", 60))
        LOGW("[Jobs] Warning: background cgroup limits not fully applied");
    return true;
}

} // namespace

bool JobsInit()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_jobBackground)
        return true;

    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!g_iocp) {
        LOGE("[Jobs] Failed to create completion port | Code: %lu",
             (unsigned long)GetLastError());
        return false;
    }
    g_iocpThread = CreateThread(nullptr, 0, IocpLoop, nullptr, 0, &g_iocpThreadId);
    if (!g_iocpThread) {
        LOGE("[Jobs] Failed to create IOCP thread");
        CloseHandle(g_iocp);
        g_iocp = nullptr;
        return false;
    }

    if (!CreateBackgroundJob()) {
        LOGE("[Jobs] Failed to create background job");
        CloseHandle(g_iocp);
        g_iocp = nullptr;
        if (g_iocpThread) {
            TerminateThread(g_iocpThread, 0);
            CloseHandle(g_iocpThread);
            g_iocpThread = nullptr;
        }
        return false;
    }

    LOGI("[Jobs] Background cgroup ready (single job + per-process focus profiles)");
    return true;
}

bool JobHookProcess(DWORD pid, HANDLE hProc)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_jobBackground)
        return false;
    auto known = g_pidJob.find(pid);
    if (known != g_pidJob.end())
        return known->second != -1; // already tracked: job or sticky fallback

    int mode = GetJobAssignMode();
    if (mode == 2) { // off
        g_pidJob[pid] = -1;
        if (ShouldLogPerPid("job-off", pid, 300))
            LOGW("[Jobs] PID %lu JobAssignMode=off -> per-process fallback (no Job assign)", pid);
        return false;
    }

    /* Assign-once: a process can live in exactly one Job. Retry is
       pointless for both failure modes, so the fallback is sticky. */
    if (!tasx_job_assign(g_jobBackground, hProc)) {
        DWORD err = GetLastError();
        g_pidJob[pid] = -1;
        if (mode == 1) {
            BOOL inJob = FALSE;
            IsProcessInJob(hProc, nullptr, &inJob);
            if (ShouldLogPerPid("job-force", pid, 300))
                LOGW("[Jobs] PID %lu assign denied (err %lu, inJob=%d, mode=force) -> per-process fallback (foreign Job without BREAKAWAY_OK)", pid, (unsigned long)err, (int)inJob);
        } else if (err == ERROR_ALREADY_ASSIGNED) {
            // foreign job without BREAKAWAY_OK — external launcher/manager owns it
            if (ShouldLogPerPid("job-foreign", pid, 300))
                LOGW("[Jobs] PID %lu already in foreign job (no BREAKAWAY_OK) -> per-process fallback, no retry", pid);
        } else if (err == ERROR_ACCESS_DENIED) {
            // err 5: process already in another Job, sticky fallback
            if (ShouldLogPerPid("job-denied", pid, 300))
                LOGW("[Jobs] PID %lu assign denied (err 5, foreign Job) -> per-process fallback, sticky no retry", pid);
        } else {
            if (ShouldLogPerPid("job-assign-fail", pid, 60))
                LOGW("[Jobs] PID %lu assign to background failed | Code: %lu -> per-process fallback", pid,
                     (unsigned long)err);
        }
        return false;
    }

    tasx_process_power_throttling(hProc, 1);
    tasx_set_io_priority(hProc, 0);
    tasx_set_memory_priority(hProc, 1);

    g_pidJob[pid] = 0;
    LOGI("[TASX] PID %lu -> background cgroup", pid);
    return true;
}

bool JobApplyProfile(DWORD pid, int focused)
{
    /* State cache for cgroup-tracked processes. The shared cgroup carries
       only farm-wide CPU/MEM caps; priority/affinity/EcoQoS per focus state
       are applied per-process by the caller (CpuApplyFocusProfile).
       Returns true when the process already runs the requested state
       (caller skips all syscalls), false when the caller must apply the
       per-process profile — and after a cross move there is deliberately
       NO AssignProcessToJobObject: moving between sibling jobs always
       fails with ERROR_ACCESS_DENIED. */
    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_pidJob.find(pid);
    if (it == g_pidJob.end() || it->second == -1)
        return false;
    int want = focused ? 2 : 0;
    if (it->second == want)
        return true;
    it->second = want;
    return false;
}

void JobsRefreshDynamic(int anyFocused)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_jobBackground)
        return;

    DWORD_PTR target = GetBgMaskForCurrentPolicy(anyFocused);
    int cap = BgCpuCapPercent(anyFocused);
    bool rateCap = cap > 0;

    if (target == g_dynamicBgMask && rateCap == g_bgRateCap)
        return;

    g_dynamicBgMask = target;
    g_bgRateCap = rateCap;

    ApplyLimitsToJob(g_jobBackground, target, cap);

    unsigned long long all = tasx_get_all_mask();
    LOGI("[Jobs] Background profile -> %s rewritten",
         target == (DWORD_PTR)all
             ? (rateCap ? "all cores (capped)" : "all cores")
             : "E-cores / low half");
}

void JobRelease(DWORD pid)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_pidJob.erase(pid);
}

void JobShutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_pidJob.clear();
    }
    if (g_iocp) {
        CloseHandle(g_iocp);
        g_iocp = nullptr;
    }
    if (g_iocpThread) {
        WaitForSingleObject(g_iocpThread, 2000);
        CloseHandle(g_iocpThread);
        g_iocpThread = nullptr;
    }
    if (g_jobBackground) {
        CloseHandle(g_jobBackground);
        g_jobBackground = nullptr;
    }
}
