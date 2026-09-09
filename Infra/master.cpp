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

#include "log.h"
#include "lograte.h"

#include <unordered_set>

/* ------------------------------------------------------------------ */
/* Event core — WMI, job completion port, foreground hook and the       */
/* low-memory reactor all feed one single-consumer loop, so process     */
/* state is touched by the main thread only.                            */
/* ------------------------------------------------------------------ */

namespace {

enum class Ev { RobloxCreated, RobloxExited, JobChild, Focus, LowMem, ConfigChanged };

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
    PushEvent({kind == 1 ? Ev::RobloxExited : Ev::JobChild, pid});
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
/* Commit-blocked spawns: pid -> next retry time (ms). The WMI creation
   event is already consumed when the block hits, so without this queue
   the client would be dropped forever. Retried on the main loop (no new
   threads); dropped silently if the process exits first. */
std::unordered_map<DWORD, ULONGLONG> g_retryBlocked;
DWORD g_appliedFocus = 0; /* PID currently running the focused profile */
WinHook* g_hook = nullptr;
HANDLE g_singleInstanceMutex = nullptr;
bool g_wmiOk = false;

char g_iniPath[MAX_PATH] = {};
ULONGLONG g_iniMtime = 0;
volatile LONG g_stop = 0;

ULONGLONG g_lastWsUpdateMs = 0;
ULONGLONG g_lastCrashSweepMs = 0;
ULONGLONG g_lastRetryMs = 0;
ULONGLONG g_lastIniCheckMs = 0;

/* Forward: defined below, used by RetryBlockedHooks. */
void ApplyFocusIfChanged();
void TimerOn();

BOOL WINAPI CtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        InterlockedExchange(&g_stop, 1);
        return TRUE;
    }
    return FALSE;
}

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

ULONGLONG IniMtimeKey()
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fad))
        return 0;
    return ((ULONGLONG)fad.ftLastWriteTime.dwLowDateTime) |
           ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32);
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

    // Commit charge guard: defer (not drop) new spawns past the threshold.
    int blockPct = config_get_int("TASX", "CommitBlockThreshold", 85);
    if (blockPct <= 0) blockPct = 85;
    int curPct = tasx_get_commit_percent();
    if (curPct >= 0 && curPct >= blockPct) {
        if (!g_retryBlocked.count(pid) && LogRateLimit("commit-block", 60))
            LOGW("[TASX] Commit charge %d%%, spawn of PID %lu deferred (threshold %d%%) — retry in ~5s",
                 curPct, pid, blockPct);
        g_retryBlocked[pid] = GetTickCount64() + 5000;
        return;
    }

    HANDLE hProc = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA |
        PROCESS_SET_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE, pid);
    if (!hProc) {
        LOGW("[TASX] Failed to open PID %lu", pid);
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
        LOGI("[TASX] New Roblox instance PID %lu hooked (job-tracked)", pid);
    }
    else {
        CpuApplyFocusProfile(hProc, 0);
        LOGI("[TASX] New Roblox instance PID %lu hooked (per-process fallback)", pid);
    }

    StartTrimmer(pid, hProc);

    WarmClientFilesAsync(pid);
}

/* Retry commit-deferred spawns (~5s cadence, on the existing main loop).
   A retried client gets the FULL hook once commit drops below the
   threshold; PIDs that exited meanwhile are dropped silently. */
void RetryBlockedHooks()
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastRetryMs < 5000 || g_retryBlocked.empty())
        return;
    g_lastRetryMs = now;

    int blockPct = config_get_int("TASX", "CommitBlockThreshold", 85);
    if (blockPct <= 0) blockPct = 85;
    int curPct = tasx_get_commit_percent();

    std::vector<DWORD> due;
    due.reserve(g_retryBlocked.size());
    for (auto& kv : g_retryBlocked) {
        if (now >= kv.second)
            due.push_back(kv.first);
    }
    for (DWORD pid : due) {
        g_retryBlocked.erase(pid);
        if (g_rbxHandles.count(pid))
            continue;
        HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!probe)
            continue; /* exited while deferred — silent drop */
        CloseHandle(probe);
        if (curPct >= 0 && curPct >= blockPct) {
            g_retryBlocked[pid] = now + 5000; /* still choking, stay queued */
            continue;
        }
        bool wasEmpty = g_rbxHandles.empty();
        HookClient(pid);
        if (g_rbxHandles.count(pid)) {
            ApplyFocusIfChanged();
            if (wasEmpty) {
                TweaksPowerEnter();
                TimerOn();
            }
        }
    }
}

void ReleaseClient(DWORD pid, bool logExit)
{
    g_retryBlocked.erase(pid); /* deferred spawn that never hooked: silent */

    auto it = g_rbxHandles.find(pid);
    if (it == g_rbxHandles.end()) return;

    if (logExit)
        LOGI("[TASX] Roblox PID %lu exited.", pid);

    UnregisterProcessWait(pid);
    StopTrimmer(pid);
    TrimmerRemoveWorkingSet(pid);
    JobRelease(pid);
    CloseHandle(it->second);
    g_rbxHandles.erase(it);

    if (g_appliedFocus == pid) {
        g_appliedFocus = 0;
        TrimmerSetFocused(0);
        // The PID is dead — its WASAPI sessions are already gone.
        // No audio API call; just drop focus/mute bookkeeping (above).
    }
}

void KillCrashHandlerPid(DWORD pid)
{
    std::wstring name = QueryProcessNameByPid(pid);
    if (!NameContains(name, L"robloxcrashhandler.exe")) return;

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) {
        TerminateProcess(h, 0);
        CloseHandle(h);
        LOGI("[TASX] Killed RobloxCrashHandler PID %lu", pid);
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
            if (g_retryBlocked.count(pid)) continue; // owned by the retry queue
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
        if (g_retryBlocked.count(s.pid)) continue;
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
    // Configurable hysteresis, bypassed so the FIRST focus detection
    // after startup (or when nothing was focused yet) is instant.
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
                LOGI("[TASX] Roblox PID %lu lost focus", oldFocus);
                if (!JobApplyProfile(oldFocus, 0))
                    CpuApplyFocusProfile(old->second, 0);
                AudioMuteByPid(oldFocus);
            }
        }

        LOGI("[TASX] Roblox PID %lu in focus", pid);
        auto cur = g_rbxHandles.find(pid);
        if (cur != g_rbxHandles.end()) {
            if (!JobApplyProfile(pid, 1))
                CpuApplyFocusProfile(cur->second, 1);
        }
        AudioUnmuteByPid(pid);
        TrimmerSetFocused(pid);
        g_appliedFocus = pid;
        // NOTE: no UpdateWorkingSetCache() here — the 10s periodic refresh
        // covers it. A full NtQuerySystemInformation scan on every focus
        // switch is a syscall storm during fast Alt-Tab.
    }
    else if (!isRoblox && g_appliedFocus)
    {
        DWORD oldFocus = g_appliedFocus;
        auto old = g_rbxHandles.find(oldFocus);
        if (old != g_rbxHandles.end()) {
            LOGI("[TASX] Roblox PID %lu lost focus (other app)", oldFocus);
            if (!JobApplyProfile(oldFocus, 0))
                CpuApplyFocusProfile(old->second, 0);
            AudioMuteByPid(oldFocus);
        }
        TrimmerSetFocused(0);
        g_appliedFocus = 0;
    }

    // Rewrite the dynamic job profile only when the focus STATE
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
        LOGI("[TASX] Timer resolution set to %lu us", actual / 10);
    }
}

void TimerOff()
{
    if (!g_timerActive) return;
    g_timerActive = false;

    unsigned long actual = 0;
    tasx_set_timer_resolution(5000, 0, &actual);
    LOGI("[TASX] Timer resolution released");
}

/* Full-system memory pass: standby purge + empty every working set. Needs
   elevation; without admin this is a silent no-op (the limited-mode banner
   at startup already told the user). */
void SystemCleanPass(bool announce)
{
    if (!config_get_bool("TASX", "SystemCleaner", 1)) return;
    if (!tasx_is_elevated()) return;

    if (tasx_purge_standby_list() && tasx_empty_working_sets_system()) {
        if (announce)
            LOGI("[TASX] System clean: standby purged + all working sets emptied");
    }
}

/* Kernel wakes us only when commit memory runs low -> no cleaning timer. */
void LowMemReactorStart()
{
    if (!config_get_bool("TASX", "LowMemReactor", 1)) return;

    HANDLE hThread = CreateThread(nullptr, 0, [](LPVOID)->DWORD {
        HANDLE hLow = CreateMemoryResourceNotification(LowMemoryResourceNotification);
        if (!hLow) {
            LOGW("[TASX] Low-memory notification unavailable");
            return 0;
        }
        // The notification is manual-reset and stays signaled while memory
        // is low — debounce so the event queue is not flooded.
        while (true) {
            if (WaitForSingleObject(hLow, INFINITE) != WAIT_OBJECT_0) break;
            PushEvent({Ev::LowMem, 0});
            int cd = config_get_int("TASX", "LowMemCooldownSec", 30);
            if (cd < 5) cd = 5;
            if (cd > 300) cd = 300;
            Sleep((DWORD)cd * 1000);
        }
        CloseHandle(hLow);
        return 0;
    }, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);
}

void CheckIniReload()
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastIniCheckMs < 10000)
        return;
    g_lastIniCheckMs = now;
    ULONGLONG mtime = IniMtimeKey();
    if (mtime && mtime != g_iniMtime)
        PushEvent({Ev::ConfigChanged, 0});
}

void HandleConfigChanged()
{
    g_iniMtime = IniMtimeKey();
    config_reload(g_iniPath);
    tasx_log_configure(config_get_str("Log", "LogFile", ""),
                       tasx_log_level_from_str(config_get_str("Log", "LogLevel", "info")));
    LOGI("[TASX] Config reloaded: %s", g_iniPath);
    /* Idempotent re-applies; trimmer thresholds are picked up lazily by
       IntervalMs()/BelowSkipThresholdCached on the next deadline. */
    FFlagsApply();
    TweaksApplyOneShot();
    JobsRefreshDynamic(g_appliedFocus != 0);
}

void HandleRobloxCreated(DWORD pid)
{
    /* Memory safety: pagefile volume free-space check */
    ULARGE_INTEGER freeBytes;
    ZeroMemory(&freeBytes, sizeof(freeBytes));
    if (GetDiskFreeSpaceExW(NULL, &freeBytes, NULL, NULL)) {
        if (freeBytes.QuadPart < (8ULL << 30) && LogRateLimit("pagefile-low-boot", 300))
            LOGW("[TASX] WARNING: Pagefile volume < 8 GB free");
    }
    bool isNew = !g_rbxHandles.count(pid);
    HookClient(pid);
    if (g_rbxHandles.count(pid))
        g_retryBlocked.erase(pid); /* hooked (possibly via retry) — dequeue */
    ApplyFocusIfChanged(); /* the new instance may already be the foreground */

    if (isNew && g_rbxHandles.count(pid)) {
        if (tasx_is_elevated() && config_get_bool("TASX", "PurgeStandbyOnLaunch", 1)) {
            if (tasx_purge_standby_list())
                LOGI("[TASX] Standby list purged (RAM reclaimed for the game)");
        }
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
    // commit block auto-unblocks on next hook due to fresh check

    if (g_rbxHandles.empty()) {
        TimerOff();
        TweaksPowerExit();
    }
}

/* One-time startup in dependency order. Returns 1 when the main loop
   should run, 0 when the process must exit silently (already running). */
int InitSubsystems()
{
    tasx_log_init();

    config_default_path(g_iniPath, MAX_PATH);
    config_load(g_iniPath);
    tasx_log_configure(config_get_str("Log", "LogFile", ""),
                       tasx_log_level_from_str(config_get_str("Log", "LogLevel", "info")));
    g_iniMtime = IniMtimeKey();
    LOGI("[TASX] Config: %s%s", g_iniPath, config_loaded() ? " (loaded)" : " (defaults)");

    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\TASX_Optimizer_Mutex");
    if (g_singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
#ifdef TASX_CONSOLE
        LOGI("[TASX] TASX is already running in the background (agent autostart or another window).");
        LOGI("[TASX] Nothing is broken - this window closes in 5 seconds.");
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
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    bool elevated = tasx_is_elevated() != 0;

    CpuInit();
    if (config_get_bool("TASX", "DisableCpuBoost", 0)) {
        if (elevated)
            CpuSetBoostMode(0);
        else if (LogRateLimit("boost-noadmin", 3600))
            LOGW("[TASX] DisableCpuBoost skipped (needs admin)");
    }
    TweaksApplyOneShot();
    FFlagsApply();
    NetCacheApply();
    SystemCleanPass(true);

    bool jobsOk = JobsInit();

    // Startup banner: admin status, job mode, enabled/disabled features.
    // Printed ONCE — replaces the old per-feature "needs admin" spam.
    LOGI("[TASX] Admin: %s | Job mode: %s",
         elevated ? "YES (elevated)" : "NO (limited mode)",
         jobsOk ? "native cgroup + per-process focus" : "per-process fallback");
    if (!elevated) {
        LOGI("[TASX] Limited mode WITHOUT admin — DISABLED: standby purge, system cleaner, HKLM tweaks, ETW, CPU boost. ENABLED: priority, affinity, FFlags, crash-handler kill, trimmer(soft). Run Install\\ScheduledTaskInstaller.bat for full effect.");
    } else {
        LOGI("[TASX] Features: priority, affinity, jobs, FFlags, trimmer, standby purge, system cleaner, HKLM tweaks — all ENABLED");
    }

    // FPS config sanity (single place): UncapFps=1 makes TargetFps
    // meaningless — the FFlags writer ignores it and uncaps to 999.
    if (config_get_bool("Roblox", "UncapFps", 1)) {
        int tf = config_get_int("Roblox", "TargetFps", 999);
        if (tf < 240 && LogRateLimit("fps-conflict", 3600))
            LOGW("[FFlags] WARNING: UncapFps=1 conflicts with TargetFps=%d -> TargetFps ignored (uncapped)", tf);
    }

    g_wmiOk = _wmimon();
    if (!g_wmiOk)
        LOGW("[TASX] WMI unavailable -> snapshot discovery active");

    g_hook = new WinHook();
    g_hook->Start();

    LowMemReactorStart();

    SweepDiscover();
    ApplyFocusIfChanged();
    UpdateWorkingSetCache();
    ULONGLONG now = GetTickCount64();
    g_lastCrashSweepMs = now;
    g_lastRetryMs = now;
    g_lastIniCheckMs = now;
    if (!g_rbxHandles.empty()) { TweaksPowerEnter(); TimerOn(); }

    LOGI("[TASX] Launched");
    return 1;
}

/* Teardown in reverse order. Runs on Ctrl/close/logoff and on natural exit
   (the main loop only ends via g_stop, so this is always reached). */
void ShutdownSubsystems()
{
    LOGI("[TASX] Shutting down...");
    delete g_hook;
    g_hook = nullptr;
    _wmishutdown();
    StopAllTrimmers();
    JobShutdown();
    TimerOff();
    TweaksPowerExit();
    if (g_singleInstanceMutex) {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
    }
    LOGI("[TASX] Stopped");
    tasx_log_shutdown();
}

int RunTasx()
{
    if (!InitSubsystems())
        return 0;

    while (!InterlockedCompareExchange(&g_stop, 0, 0))
    {
        auto ev = PopEvent(250);

        if (ev) {
            switch (ev->kind) {
            case Ev::RobloxCreated: HandleRobloxCreated(ev->pid); break;
            case Ev::RobloxExited:  HandleRobloxExited(ev->pid);  break;
            case Ev::JobChild:      KillCrashHandlerPid(ev->pid); break;
            case Ev::Focus:         ApplyFocusIfChanged();        break;
            case Ev::ConfigChanged: HandleConfigChanged();        break;
            case Ev::LowMem: {
                static ULONGLONG lastLowMemCleanMs = 0;
                ULONGLONG now = GetTickCount64();
                int minIntervalSec = config_get_int("TASX", "SystemCleanMinIntervalSec", 60);
                if (now - lastLowMemCleanMs < (ULONGLONG)minIntervalSec * 1000) {
                    break;  // rate-limited
                }
                lastLowMemCleanMs = now;
                LOGI("[TASX] Low memory event -> reactive clean");
                tasx_purge_standby_list();
                TrimmerTrimAllAggressive();
                // NEVER call SystemCleanPass (global WS empty) if focused client exists
                if (g_appliedFocus == 0) {
                    SystemCleanPass(true);
                } else {
                    LOGI("[TASX] Skipped system clean (focused PID %lu active)", g_appliedFocus);
                }
                break;
            }
            }
        }

        // Focus safety net (hook may miss exotic switches): PopEvent timeout 250ms already does cadence via loop
        // But also ensure ApplyFocus on each iteration (lightweight, does GetForegroundWindow)
        ApplyFocusIfChanged();
        RetryBlockedHooks();

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
            CheckIniReload();
        }

        // Crash handlers: only every 60s plus on RBX_ON/JobChild
        if (now - g_lastCrashSweepMs >= 60000) {
            SweepCrashHandlers();
            NetCacheLogConnections(ClientPidSet());
            g_lastCrashSweepMs = now;

            // periodic pagefile free-space check + self CPU watchdog.
            ULARGE_INTEGER freeBytes;
    ZeroMemory(&freeBytes, sizeof(freeBytes));
            if (GetDiskFreeSpaceExW(NULL, &freeBytes, NULL, NULL)) {
                if (freeBytes.QuadPart < (8ULL << 30)) {
                    if (LogRateLimit("pagefile-low", 300))
                        LOGW("[TASX] WARNING: Pagefile volume < 8 GB free");
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
                        int cpuPct = (int)(((ku - lastSelfCpuMs) * 100) /
                                           ((now - lastCpuCheckMs) * 10000));
                        int threshold = config_get_int("TASX", "SelfCpuWatchdogPercent", 5);
                        if (cpuPct > threshold && LogRateLimit("self-cpu", 300)) {
                            LOGW("[TASX] WARNING: Self CPU %d%% exceeds watchdog threshold %d%%",
                                 cpuPct, threshold);
                        }
                    }
                    lastSelfCpuMs = ku;
                    lastCpuCheckMs = now;
                }
            }
        }
    }

    ShutdownSubsystems();
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
