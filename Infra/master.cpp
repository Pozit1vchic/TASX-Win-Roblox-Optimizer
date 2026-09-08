#define NOMINMAX

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <chrono>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>

#include "WMI.h"
#include "CPU.h"
#include "trimmer.h"
#include "winhook.h"
#include "config.h"
#include "ntsys.h"
#include "tweaks.h"
#include "fflags.h"
#include "jobs.h"
#include "stats.h"
#include "audio.h"
#include "warm.h"
#include "desktop.h"
#include "netcache.h"

#include "lograte.h"

#include <unordered_set>

/* ------------------------------------------------------------------ */
/* Event core — WMI, job completion port, foreground hook and the       */
/* low-memory reactor all feed one single-consumer loop, so process     */
/* state is touched by the main thread only.                            */
/* ------------------------------------------------------------------ */

namespace {

enum class Ev { RobloxCreated, RobloxExited, ChildSpawn, Focus, LowMem };

struct Event {
    Ev    kind;
    DWORD pid;
};

std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::deque<Event> g_queue;

void PushEvent(Event ev)
{
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queue.push_back(ev);
    }
    g_queueCv.notify_one();
}

std::optional<Event> PopEvent(unsigned timeoutMs)
{
    std::unique_lock<std::mutex> lock(g_queueMutex);
    if (!g_queueCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [] { return !g_queue.empty(); }))
        return std::nullopt;

    Event ev = g_queue.front();
    g_queue.pop_front();
    return ev;
}

} /* namespace */

/* Cross-module notifications (WMI sink / job IOCP / hook threads). */
void TasxNotifyProcess(unsigned long pid, int created)
{
    PushEvent({created ? Ev::RobloxCreated : Ev::RobloxExited, (DWORD)pid});
}

void TasxNotifyJobEvent(int kind, DWORD pid)
{
    PushEvent({kind == 1 ? Ev::RobloxExited : Ev::ChildSpawn, pid});
}

void WinHook::NotifyFocusChanged()
{
    PushEvent({Ev::Focus, 0});
}

/* ------------------------------------------------------------------ */
/* Roblox instance bookkeeping                                          */
/* ------------------------------------------------------------------ */

namespace {

std::unordered_map<DWORD, HANDLE> g_rbxHandles;
std::unordered_map<DWORD, HANDLE> g_waitHandles; // RegisterWait handles
DWORD g_appliedFocus = 0; /* PID currently running the focused profile */
WinHook* g_hook = nullptr;
HANDLE g_singleInstanceMutex = nullptr;
bool g_wmiOk = false;

ULONGLONG g_lastWsUpdateMs = 0;
ULONGLONG g_lastCrashSweepMs = 0;

bool NameContains(const std::wstring& full, const wchar_t* needle)
{
    std::wstring hay = full, ndl = needle;
    for (auto& c : hay) c = (wchar_t)towlower(c);
    for (auto& c : ndl) c = (wchar_t)towlower(c);
    return hay.find(ndl) != std::wstring::npos;
}

bool IsRobloxName(const std::wstring& name)
{
    return NameContains(name, L"robloxplayerbeta.exe") ||
           name == L"Roblox.exe" || NameContains(name, L"\\roblox.exe");
}

std::unordered_set<DWORD> ClientPidSet()
{
    std::unordered_set<DWORD> pids;
    pids.reserve(g_rbxHandles.size());
    for (auto& kv : g_rbxHandles) pids.insert(kv.first);
    return pids;
}

std::wstring QueryProcessName(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &size))
        name = path;
    CloseHandle(h);
    return name;
}

VOID CALLBACK OnProcessExit(PVOID lpParam, BOOLEAN)
{
    DWORD pid = (DWORD)(ULONG_PTR)lpParam;
    PushEvent({Ev::RobloxExited, pid});
}

void RegisterProcessWait(DWORD pid, HANDLE hProc)
{
    HANDLE hWait = nullptr;
    if (!RegisterWaitForSingleObject(&hWait, hProc, OnProcessExit,
                                     (PVOID)(ULONG_PTR)pid, INFINITE,
                                     WT_EXECUTEONLYONCE)) {
        return;
    }
    // store for UnregisterWaitEx on clean exit
    auto it = g_waitHandles.find(pid);
    if (it != g_waitHandles.end()) {
        UnregisterWaitEx(it->second, nullptr);
    }
    g_waitHandles[pid] = hWait;
}

void UnregisterProcessWait(DWORD pid)
{
    auto it = g_waitHandles.find(pid);
    if (it != g_waitHandles.end()) {
        UnregisterWaitEx(it->second, INVALID_HANDLE_VALUE);
        g_waitHandles.erase(it);
    }
}

/* Event-driven hook of a single client (WMI event or sweep discovery). */
void HookClient(DWORD pid)
{
    if (g_rbxHandles.count(pid)) return;

    // Commit charge guard: block new spawns if > threshold
    int blockPct = config_get_int("TASX", "CommitBlockThreshold", 85);
    if (blockPct <= 0) blockPct = 85;
    // also support legacy CommitBlockThreshold naming? Use same.
    int curPct = tasx_get_commit_percent();
    if (curPct >= 0 && curPct >= blockPct) {
        std::cout << "[TASX] Commit charge " << curPct << "%, spawn blocked (threshold "
                  << blockPct << "%) PID " << pid << std::endl;
        return;
    }

    HANDLE hProc = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA |
        PROCESS_SET_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE, pid);
    if (!hProc) {
        std::cout << "[TASX] Failed to open PID " << pid << std::endl;
        return;
    }

    g_rbxHandles[pid] = hProc;

    // Register wait for exit notification (zero polling)
    RegisterProcessWait(pid, hProc);

    bool inJob = JobHookProcess(pid, hProc);
    if (inJob) {
        tasx_process_power_throttling(hProc, 1);
        tasx_set_io_priority(hProc, 0);
        tasx_set_memory_priority(hProc, 1);
        std::cout << "[TASX] New Roblox instance PID " << pid
                  << " hooked (job-tracked)" << std::endl;
    }
    else {
        CpuApplyFocusProfile(hProc, 0);
        std::cout << "[TASX] New Roblox instance PID " << pid
                  << " hooked (per-process fallback)" << std::endl;
    }

    StartTrimmer(pid, hProc);

    WarmClientFilesAsync(pid);
}

void ReleaseClient(DWORD pid, bool logExit)
{
    auto it = g_rbxHandles.find(pid);
    if (it == g_rbxHandles.end()) return;

    if (logExit)
        std::cout << "[TASX] Roblox PID " << pid << " exited." << std::endl;

    UnregisterProcessWait(pid);
    StopTrimmer(pid);
    TrimmerRemoveWorkingSet(pid);
    JobRelease(pid);
    CloseHandle(it->second);
    g_rbxHandles.erase(it);

    if (g_appliedFocus == pid) {
        g_appliedFocus = 0;
        TrimmerSetFocused(0);
        // BUG 14: the PID is dead - its WASAPI sessions are already gone.
        // No audio API call; just drop our focus/mute bookkeeping (above).
    }
}

void KillCrashHandlerPid(DWORD pid)
{
    std::wstring name = QueryProcessName(pid);
    if (!NameContains(name, L"robloxcrashhandler.exe")) return;

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) {
        TerminateProcess(h, 0);
        CloseHandle(h);
        std::cout << "[TASX] Killed RobloxCrashHandler PID " << pid << std::endl;
    }
}

void SweepCrashHandlers()
{
    if (!config_get_bool("TASX", "KillCrashHandler", 1)) return;

    std::vector<ProcStat> stats;
    if (!QueryProcStats(stats)) return;

    for (const auto& s : stats)
        if (s.name.size() && NameContains(s.name, L"robloxcrashhandler.exe"))
            KillCrashHandlerPid(s.pid);
}

/* Fallback via Toolhelp snapshot (only if WMI down) */
void SweepDiscoverViaSnapshot()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name(pe.szExeFile);
            if (!IsRobloxName(name)) continue;
            DWORD pid = pe.th32ProcessID;
            if (g_rbxHandles.count(pid)) continue;
            // commit guard inside HookClient
            HookClient(pid);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

/* Discovery fallback (WMI down / missed events): one syscall, no snapshots ideally. */
void SweepDiscover()
{
    if (!g_wmiOk) {
        // Primary fallback is snapshot per spec when WMI dead
        SweepDiscoverViaSnapshot();
        return;
    }
    // WMI healthy: use single-syscall table (lighter than snapshot)
    std::vector<ProcStat> stats;
    if (!QueryProcStats(stats)) return;

    for (const auto& s : stats) {
        if (!s.name.size() || !IsRobloxName(s.name)) continue;
        if (g_rbxHandles.count(s.pid)) continue;
        HookClient(s.pid);
    }
}

void UpdateWorkingSetCache()
{
    std::vector<ProcStat> stats;
    if (!QueryProcStats(stats)) return;
    for (const auto& s : stats) {
        if (g_rbxHandles.count(s.pid)) {
            TrimmerUpdateWorkingSet(s.pid, s.workingSet);
        }
    }
    g_lastWsUpdateMs = GetTickCount64();
}

void ApplyFocusIfChanged()
{
    HWND hwnd = GetForegroundWindow();
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);

    bool isRoblox = pid != 0 && g_rbxHandles.count(pid) != 0;
    // BUG 5/13: configurable hysteresis, bypassed so the FIRST focus
    // detection after startup (or when nothing was focused yet) is instant.
    static ULONGLONG lastFocusCheckMs = 0;
    ULONGLONG now = GetTickCount64();
    int hysteresisMs = config_get_int("TASX", "FocusHysteresisMs", 500);
    bool bypassHysteresis = (g_appliedFocus == 0 && isRoblox);
    if (!bypassHysteresis && (now - lastFocusCheckMs) < (ULONGLONG)hysteresisMs) {
        return;
    }
    lastFocusCheckMs = now;

    if (isRoblox && g_appliedFocus != pid)
    {
        DWORD oldFocus = g_appliedFocus;
        if (oldFocus) {
            auto old = g_rbxHandles.find(oldFocus);
            if (old != g_rbxHandles.end()) {
                std::cout << "[TASX] Roblox PID " << oldFocus
                          << " lost focus" << std::endl;
                if (!JobApplyProfile(oldFocus, 0))
                    CpuApplyFocusProfile(old->second, 0);
                else
                    tasx_process_power_throttling(old->second, 1);
                AudioMuteByPid(oldFocus);
            }
        }

        std::cout << "[TASX] Roblox PID " << pid << " in focus" << std::endl;
        auto cur = g_rbxHandles.find(pid);
        if (cur != g_rbxHandles.end()) {
            if (!JobApplyProfile(pid, 1))
                CpuApplyFocusProfile(cur->second, 1);
            else
                tasx_process_power_throttling(cur->second, 0);
        }
        AudioUnmuteByPid(pid);
        TrimmerSetFocused(pid);
        g_appliedFocus = pid;
        UpdateWorkingSetCache();
    }
    else if (!isRoblox && g_appliedFocus)
    {
        DWORD oldFocus = g_appliedFocus;
        auto old = g_rbxHandles.find(oldFocus);
        if (old != g_rbxHandles.end()) {
            std::cout << "[TASX] Roblox PID " << oldFocus
                      << " lost focus (other app)" << std::endl;
            if (!JobApplyProfile(oldFocus, 0))
                CpuApplyFocusProfile(old->second, 0);
            else
                tasx_process_power_throttling(old->second, 1);
            AudioMuteByPid(oldFocus);
        }
        // Ensure focused pid itself is unmuted if it was muted before (should already)
        TrimmerSetFocused(0);
        g_appliedFocus = 0;
        UpdateWorkingSetCache();
    }

    // BUG 5: rewrite the dynamic job profile only when the focus STATE
    // (focused / not focused) actually changed, not on every 250 ms tick.
    static int lastAppliedFocusState = -1;
    int currentState = (g_appliedFocus != 0) ? 1 : 0;
    if (currentState != lastAppliedFocusState) {
        JobsRefreshDynamic(currentState);
        lastAppliedFocusState = currentState;
    }
}

/* Timer resolution: 0.5 ms tick while Roblox is running -> smoother frame
   pacing and snappier input scheduling. State-guarded: prints/acts only on
   the on/off transition. */
bool g_timerActive = false;

void TimerOn()
{
    if (g_timerActive) return;
    unsigned long actual = 0;
    if (config_get_bool("TASX", "TimerResolution", 1) &&
        tasx_set_timer_resolution(5000, 1, &actual)) {
        g_timerActive = true;
        std::cout << "[TASX] Timer resolution set to "
                  << (actual / 10) << " us" << std::endl;
    }
}

void TimerOff()
{
    if (!g_timerActive) return;
    g_timerActive = false;

    unsigned long actual = 0;
    tasx_set_timer_resolution(5000, 0, &actual);
    std::cout << "[TASX] Timer resolution released" << std::endl;
}

/* Full-system memory pass: standby purge + empty every working set. Needs
   elevation; failure is logged once so the log doesn't spam. */
void SystemCleanPass(bool announce)
{
    if (!config_get_bool("TASX", "SystemCleaner", 1)) return;

    static bool warned = false;

    bool purged = tasx_purge_standby_list();
    bool emptied = tasx_empty_working_sets_system();

    if (purged && emptied) {
        if (announce)
            std::cout << "[TASX] System clean: standby purged + all working sets emptied"
                      << std::endl;
    }
    else if (!warned) {
        warned = true;
        std::cout << "[TASX] System cleaner needs admin rights "
                     "(install via ScheduledTaskInstaller.bat for full effect)"
                  << std::endl;
    }
}

/* Kernel wakes us only when commit memory runs low -> no cleaning timer. */
void LowMemReactorStart()
{
    if (!config_get_bool("TASX", "LowMemReactor", 1)) return;

    HANDLE hThread = CreateThread(nullptr, 0, [](LPVOID)->DWORD {
        HANDLE hLow = CreateMemoryResourceNotification(LowMemoryResourceNotification);
        if (!hLow) {
            std::cout << "[TASX] Low-memory notification unavailable" << std::endl;
            return 0;
        }
        // BUG 3: the notification is manual-reset and stays signaled while
        // memory is low - without a debounce this floods the event queue.
        while (true) {
            if (WaitForSingleObject(hLow, INFINITE) != WAIT_OBJECT_0) break;
            PushEvent({Ev::LowMem, 0});
            Sleep(30000);  // debounce: after signaling, sleep 30s before next push
        }
        CloseHandle(hLow);
        return 0;
    }, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);
}

void HandleRobloxCreated(DWORD pid)
{
    /* STEP 4 — Memory safety: pagefile volume free-space check */
    ULARGE_INTEGER freeBytes = {0};
    if (GetDiskFreeSpaceExW(NULL, &freeBytes, NULL, NULL)) {
        if (freeBytes.QuadPart < (8ULL << 30))
            std::cout << "[TASX] WARNING: Pagefile volume < 8 GB free" << std::endl;
    }
    bool isNew = !g_rbxHandles.count(pid);
    HookClient(pid);
    ApplyFocusIfChanged(); /* the new instance may already be the foreground */

    if (isNew && g_rbxHandles.count(pid)) {
        if (config_get_bool("TASX", "PurgeStandbyOnLaunch", 1)) {
            if (tasx_purge_standby_list())
                std::cout << "[TASX] Standby list purged (RAM reclaimed for the game)"
                          << std::endl;
        }
        // Kill crash handlers only on RBX_ON (per spec) not periodically
        SweepCrashHandlers();
        FFlagsApply();
        UpdateWorkingSetCache();
    }

    TweaksPowerEnter();
    TimerOn();
}

void HandleRobloxExited(DWORD pid)
{
    ReleaseClient(pid, true);
    // commit block auto-unblocks on next RBX_ON due to fresh check

    if (g_rbxHandles.empty()) {
        TimerOff();
        TweaksPowerExit();
    }
}

int RunTasx()
{
    char iniPath[MAX_PATH];
    config_default_path(iniPath, MAX_PATH);
    config_load(iniPath);
    std::cout << "[TASX] Config: " << iniPath
              << (config_loaded() ? " (loaded)" : " (defaults)") << std::endl;

    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\TASX_Optimizer_Mutex");
    if (g_singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
#ifdef TASX_CONSOLE
        std::cout << "[TASX] TASX is already running in the background"
                     " (agent autostart or another window)." << std::endl;
        std::cout << "[TASX] Nothing is broken - this window closes in 5 seconds."
                  << std::endl;
        Sleep(5000);
#else
        MessageBoxW(nullptr,
            L"TASX уже запущен и работает в фоне (агент автозапуска).\n"
            L"Второй экземпляр не нужен - всё уже оптимизируется.",
            L"TASX", MB_OK | MB_ICONINFORMATION);
#endif
        return 0;
    }

    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    CpuInit();
    if (config_get_bool("TASX", "DisableCpuBoost", 0))
        CpuSetBoostMode(0);
    TweaksApplyOneShot();
    FFlagsApply();
    NetCacheApply();
    SystemCleanPass(true);

    bool jobsOk = JobsInit();
    (void)jobsOk;

    g_wmiOk = _wmimon();
    if (!g_wmiOk)
        std::cout << "[TASX] WMI unavailable -> snapshot discovery active"
                  << std::endl;

    g_hook = new WinHook();
    g_hook->Start();

    LowMemReactorStart();

    SweepDiscover();
    ApplyFocusIfChanged();
    UpdateWorkingSetCache();
    g_lastCrashSweepMs = GetTickCount64();
    if (!g_rbxHandles.empty()) { TweaksPowerEnter(); TimerOn(); }

    std::cout << "[TASX] Launched" << std::endl;

    while (true)
    {
        auto ev = PopEvent(250);

        if (ev) {
            switch (ev->kind) {
            case Ev::RobloxCreated: HandleRobloxCreated(ev->pid); break;
            case Ev::RobloxExited:  HandleRobloxExited(ev->pid);  break;
            case Ev::ChildSpawn:    KillCrashHandlerPid(ev->pid); break;
            case Ev::Focus:         ApplyFocusIfChanged();        break;
            case Ev::LowMem: {
                static ULONGLONG lastLowMemCleanMs = 0;
                ULONGLONG now = GetTickCount64();
                int minIntervalSec = config_get_int("TASX", "SystemCleanMinIntervalSec", 60);
                if (now - lastLowMemCleanMs < (ULONGLONG)minIntervalSec * 1000) {
                    break;  // rate-limited
                }
                lastLowMemCleanMs = now;
                std::cout << "[TASX] Low memory event -> reactive clean" << std::endl;
                tasx_purge_standby_list();
                TrimmerTrimAllAggressive();
                // NEVER call SystemCleanPass (global WS empty) if focused client exists
                if (g_appliedFocus == 0) {
                    SystemCleanPass(true);
                } else {
                    std::cout << "[TASX] Skipped system clean (focused PID "
                              << g_appliedFocus << " active)" << std::endl;
                }
                break;
            }
            }
        }

        // Focus safety net (hook may miss exotic switches): PopEvent timeout 250ms already does cadence via loop
        // But also ensure ApplyFocus on each iteration (lightweight, does GetForegroundWindow)
        ApplyFocusIfChanged();

        ULONGLONG now = GetTickCount64();

        // Periodic WS cache refresh ~10s, plus GDI guard, audio refresh, dynamic jobs
        if (now - g_lastWsUpdateMs >= 10000) {
            // discovery only if WMI unhealthy; otherwise WS cache update suffices
            if (!g_wmiOk) SweepDiscover();
            UpdateWorkingSetCache();
            for (auto& kv : g_rbxHandles)
                DesktopCheckClient(kv.first, kv.second);
            // audio sessions appear/disappear without our knowledge -> periodic bulk sync
            AudioApplyBackgroundMute(ClientPidSet(), g_appliedFocus);
            JobsRefreshDynamic(g_appliedFocus != 0);
        }

        // Crash handlers: only every 60s plus on RBX_ON/ChildSpawn
        if (now - g_lastCrashSweepMs >= 60000) {
            SweepCrashHandlers();
            NetCacheLogConnections(ClientPidSet());
            g_lastCrashSweepMs = now;

            // BUG 12: periodic pagefile free-space check (was only in
            // HandleRobloxCreated) + self CPU watchdog.
            ULARGE_INTEGER freeBytes = {0};
            if (GetDiskFreeSpaceExW(NULL, &freeBytes, NULL, NULL)) {
                if (freeBytes.QuadPart < (8ULL << 30)) {
                    if (LogRateLimit("pagefile-low", 300))
                        std::cout << "[TASX] WARNING: Pagefile volume < 8 GB free" << std::endl;
                }
            }
            {
                static ULONGLONG lastCpuCheckMs = 0;
                static ULONGLONG lastSelfCpuMs = 0;
                FILETIME ftime, fexit, fkernel, fuser;
                if (GetProcessTimes(GetCurrentProcess(), &ftime, &fexit, &fkernel, &fuser)) {
                    ULONGLONG ku =
                        (((ULONGLONG)fkernel.dwHighDateTime << 32) | fkernel.dwLowDateTime) +
                        (((ULONGLONG)fuser.dwHighDateTime << 32) | fuser.dwLowDateTime);
                    if (lastCpuCheckMs && now > lastCpuCheckMs && ku >= lastSelfCpuMs) {
                        int cpuPct = (int)((ku - lastSelfCpuMs) /
                                           ((now - lastCpuCheckMs) * 10000));
                        int threshold = config_get_int("TASX", "SelfCpuWatchdogPercent", 5);
                        if (cpuPct > threshold && LogRateLimit("self-cpu", 300)) {
                            std::cout << "[TASX] WARNING: Self CPU " << cpuPct
                                      << "% exceeds watchdog threshold " << threshold
                                      << "%" << std::endl;
                        }
                    }
                    lastSelfCpuMs = ku;
                    lastCpuCheckMs = now;
                }
            }
        }
    }

    StopAllTrimmers();
    JobShutdown();
    delete g_hook;
    _wmishutdown();
    if (g_singleInstanceMutex) {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
    }
    return 0;
}

} /* namespace */

#ifdef TASX_CONSOLE
int main()
{
    return RunTasx();
}
#else
int WINAPI WinMain(
    _In_ HINSTANCE,
    _In_opt_ HINSTANCE,
    _In_ LPSTR,
    _In_ int)
{
    return RunTasx();
}
#endif
