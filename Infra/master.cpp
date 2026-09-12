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
#include "hotkey.h"
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
/* Event core - WMI, job completion port, foreground hook and the       */
/* low-memory reactor all feed one single-consumer loop, so process     */
/* state is touched by the main thread only.                            */
/* ------------------------------------------------------------------ */

namespace {

enum class Ev { RobloxCreated, RobloxExited, JobChild, Focus, LowMem, ConfigChanged, HotkeyBoost };

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

void TasxNotifyHotkey()
{
    PushEvent({Ev::HotkeyBoost, 0});
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
bool g_pageInBusy = false;
ULONGLONG g_lastPageInMs = 0;
WinHook* g_hook = nullptr;
FarmHotkey* g_hotkey = nullptr;
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
void TimerOffIfFarmNoFocus();

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
            LOGW("[TASX] Commit charge %d%%, spawn of PID %lu deferred (threshold %d%%) - retry in ~5s",
                 curPct, pid, blockPct);
        g_retryBlocked[pid] = GetTickCount64() + 5000;
        return;
    }

    HANDLE hProc = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA |
        PROCESS_SET_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE |
        PROCESS_VM_READ,
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
        tasx_process_power_throttling(hProc, 0); // always P+E, no EcoQoS
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
            continue; /* exited while deferred - silent drop */
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
        // The PID is dead - its WASAPI sessions are already gone.
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

ULONGLONG g_farmWsBytes = 0; /* sum of tracked clients WS, refreshed ~10s */

void UpdateWorkingSetCache()
{
    std::vector<ProcStat> stats;
    if (!QueryProcStats(stats)) return;
    ULONGLONG sum = 0;
    for (const auto& s : stats) {
        if (g_rbxHandles.count(s.pid)) {
            TrimmerUpdateWorkingSet(s.pid, s.workingSet);
            sum += (ULONGLONG)s.workingSet;
        }
    }
    g_farmWsBytes = sum;
    g_lastWsUpdateMs = GetTickCount64();
}

/* Farm RAM budget honesty check: keep-hot is physically impossible when the
   farm working set alone exceeds ~75% of installed RAM - paging is then
   inevitable and the log should say so instead of silently trimming. */
static void FarmBudgetCheck()
{
    if (g_rbxHandles.empty()) return;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms) || !ms.ullTotalPhys) return;
    double frac = (double)g_farmWsBytes / (double)ms.ullTotalPhys;
    if (frac > 0.75) {
        if (LogRateLimit("farm-budget", 600)) {
            double farmGB = (double)g_farmWsBytes / (1ull << 30);
            double totalGB = (double)ms.ullTotalPhys / (1ull << 30);
            LOGW("[TASX] WARNING: farm WS %.1f/%.1f GB RAM (>75%%) - keep-hot impossible, paging inevitable: reduce client count or add RAM",
                 farmGB, totalGB);
        }
    }
}

static int GetFocusDwellMs()
{
    // primary: FocusDwellMs, fallback: FocusHysteresisMs for compat, default 1800
    const char* dwellStr = config_get_str("TASX", "FocusDwellMs", nullptr);
    if (dwellStr)
        return config_get_int("TASX", "FocusDwellMs", 1800);
    const char* hyst = config_get_str("TASX", "FocusHysteresisMs", nullptr);
    if (hyst)
        return config_get_int("TASX", "FocusHysteresisMs", 1800);
    return 1800;
}

void ApplyFocusIfChanged()
{
    HWND hwnd = GetForegroundWindow();
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);

    bool isRoblox = pid != 0 && g_rbxHandles.count(pid) != 0;
    ULONGLONG now = GetTickCount64();
    int dwellMs = GetFocusDwellMs();
    if (dwellMs < 100) dwellMs = 100;
    if (dwellMs > 10000) dwellMs = 10000;

    DWORD candidate = isRoblox ? pid : 0;

    // Dwell/anti-flap: pending coalescing state
    static DWORD s_pendingPid = 0;
    static ULONGLONG s_pendingSince = 0;
    static bool s_hasPending = false;

    if (candidate == g_appliedFocus) {
        // stable state - clear pending (no switch needed)
        s_hasPending = false;
    } else if (g_appliedFocus == 0 && candidate != 0) {
        // Legacy bypass restored: very first focus after startup is instant
        // (old FocusHysteresisMs behavior). Dwell applies only to later switches.
        s_hasPending = false;
        // fallthrough to apply below
    } else {
        // need to switch to candidate, but must dwell
        if (s_hasPending && s_pendingPid == candidate) {
            if (now - s_pendingSince < (ULONGLONG)dwellMs) {
                return; // still dwelling, ignore flap
            }
            // dwell elapsed -> commit
            s_hasPending = false;
        } else if (s_hasPending && s_pendingPid != candidate) {
            // flap: candidate changed again before dwell elapsed - coalesce, restart dwell
            s_pendingPid = candidate;
            s_pendingSince = now;
            return;
        } else {
            s_hasPending = true;
            s_pendingPid = candidate;
            s_pendingSince = now;
            return;
        }
    }
    // dwell satisfied - proceed to apply candidate (which equals isRoblox?pid:0)

    // Focus changes use all-cores profile (always P+E)
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
        TimerOn();
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
        TimerOffIfFarmNoFocus();
    }

    // Rewrite the dynamic job profile only when the focus STATE
    // (focused / not focused) actually changed, not on every 250 ms tick.
    // FarmBoost counts as focused (farm runs all-cores, rate-cap policy flips).
    static int lastAppliedFocusState = -1;
    int currentState = (g_appliedFocus != 0) ? 1 : 0;
    if (currentState != lastAppliedFocusState) {
        JobsRefreshDynamic(currentState);
        lastAppliedFocusState = currentState;
    }
}

/* Page-in hotkey: pull all tracked clients out of pagefile.
   Always keeps P+E affinity; never changes CPU profile or trims. */
/* Parallel page-in (4 workers) for speed; aggregate stats. */
struct PageInWorkerCtx {
    HANDLE h;
    CpuPageInStats st{};
};

static DWORD WINAPI PageInWorker(LPVOID arg)
{
    auto* ctx = static_cast<PageInWorkerCtx*>(arg);
    CpuPageInProcess(ctx->h, &ctx->st);
    return 0;
}

void PageInFarm()
{
    if (g_pageInBusy) {
        LOGI("[TASX] Page-in already running (hotkey ignored)");
        return;
    }
    g_pageInBusy = true;
    TrimmerSetPageInPause(1);
    ULONGLONG t0 = GetTickCount64();

    std::vector<std::pair<DWORD, HANDLE>> clients;
    for (auto& kv : g_rbxHandles) clients.push_back({kv.first, kv.second});

    std::vector<PageInWorkerCtx> ctxs; ctxs.reserve(clients.size());
    std::vector<HANDLE> threads; threads.reserve(clients.size());
    for (auto& p : clients) {
        HANDLE dup = nullptr;
        DuplicateHandle(GetCurrentProcess(), p.second, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        ctxs.push_back({dup ? dup : p.second, {}});
    }

    for (size_t i = 0; i < ctxs.size(); ++i) {
        HANDLE th = CreateThread(nullptr, 0, PageInWorker, &ctxs[i], 0, nullptr);
        if (th) threads.push_back(th);
    }

    for (HANDLE th : threads) { WaitForSingleObject(th, 60000); CloseHandle(th); }

    unsigned clientsDone = 0;
    unsigned long long totalAttempted = 0, totalTouched = 0, totalFailed = 0, totalBytes = 0;
    for (size_t i = 0; i < clients.size(); ++i) {
        if (ctxs[i].h) {
            clientsDone++;
            totalAttempted += ctxs[i].st.attemptedPages;
            totalTouched += ctxs[i].st.touchedPages;
            totalFailed += ctxs[i].st.failedPages;
            totalBytes += ctxs[i].st.bytesTouched;
            if (clients[i].first) LOGI("[TASX] Page-in PID %lu: %llu pages touched, %llu failed | ~%llu KB",
                clients[i].first,
                (unsigned long long)ctxs[i].st.touchedPages,
                (unsigned long long)ctxs[i].st.failedPages,
                (unsigned long long)(ctxs[i].st.bytesTouched >> 10));
            if (ctxs[i].h != clients[i].second) CloseHandle(ctxs[i].h);
        }
    }

    ULONGLONG t1 = GetTickCount64();
    TrimmerSetPageInPause(0);
    g_pageInBusy = false;
    LOGI("[TASX] Page-in complete (%lu ms): %u clients | %llu pages attempted, %llu touched, %llu failed | ~%llu MB fetched",
         (unsigned long)(t1 - t0), clientsDone,
         (unsigned long long)totalAttempted,
         (unsigned long long)totalTouched,
         (unsigned long long)totalFailed,
         (unsigned long long)(totalBytes >> 20));
}

/* Timer resolution: 0.5 ms tick while Roblox is running -> smoother frame
   pacing and snappier input scheduling. State-guarded: prints/acts only on
   the on/off transition. */
bool g_timerActive = false;

/* Farm preset detection (single place): farm15 is a deprecated alias of
   farm20; weak/balanced are the other TASX-owned-graphics presets. */
static bool IsFarmPreset()
{
    const char* pPre = config_get_str("FastFlags", "Preset", nullptr);
    if (!pPre) pPre = config_get_str("TASX", "Preset", nullptr);
    if (!pPre) return false;
    char pl[16] = {};
    size_t pn = 0;
    for (; pPre[pn] && pn + 1 < sizeof(pl); ++pn)
        pl[pn] = (char)tolower((unsigned char)pPre[pn]);
    pl[pn] = '\0';
    return (strcmp(pl, "farm15") == 0 || strcmp(pl, "farm20") == 0 ||
            strcmp(pl, "farm30") == 0 || strcmp(pl, "weak") == 0 ||
            strcmp(pl, "balanced") == 0);
}

void TimerOffIfFarmNoFocus()
{
    // disable 0.5ms timer when farm preset, nothing focused
    if (IsFarmPreset() && g_appliedFocus == 0 && g_timerActive) {
        unsigned long actual = 0;
        tasx_set_timer_resolution(5000, 0, &actual);
        g_timerActive = false;
        // Do NOT log on every toggle to avoid spam (timer off is silent/good)
    }
}

void TimerOn()
{
    if (g_timerActive) return;
    // If farm preset and nothing focused, keep timer off (user directive)
    if (IsFarmPreset() && g_appliedFocus == 0) return; // no timer for pure farm
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

/* Full-system memory pass, split in two halves (farm keep-hot):
   - standby purge: cheap page-cache reclaim, safe for farms (default ON);
   - empty every working set: evicts ALL processes incl. farm clients,
     the direct cause of "slow return from swap" (default OFF on farm*).
   Needs elevation; without admin this is a silent no-op (the limited-mode
   banner at startup already told the user). */
static bool IsFarmStrict()
{
    // Strict farm presets only (farm15/20/30): keep-hot farms where a global
    // empty-WS is pure harm. weak/balanced keep legacy default (ON).
    const char* pPre = config_get_str("FastFlags", "Preset", nullptr);
    if (!pPre) pPre = config_get_str("TASX", "Preset", nullptr);
    if (!pPre) return false;
    char pl[16] = {};
    size_t pn = 0;
    for (; pPre[pn] && pn + 1 < sizeof(pl); ++pn)
        pl[pn] = (char)tolower((unsigned char)pPre[pn]);
    pl[pn] = '\0';
    return (strcmp(pl, "farm15") == 0 || strcmp(pl, "farm20") == 0 ||
            strcmp(pl, "farm30") == 0);
}

static bool SystemCleanEmptyWSAllowed()
{
    const char* v = config_get_str("TASX", "SystemCleanEmptyWS", nullptr);
    if (!v) return IsFarmStrict() ? false : true;
    return config_get_bool("TASX", "SystemCleanEmptyWS", 0) != 0;
}

void SystemCleanPass(bool announce)
{
    if (!config_get_bool("TASX", "SystemCleaner", 1)) return;
    if (!tasx_is_elevated()) return;

    bool standby = false, emptied = false;
    if (config_get_bool("TASX", "SystemCleanStandby", 1))
        standby = tasx_purge_standby_list() != 0;
    // Global empty-WS never touches a focused client and never runs on a
    // keep-hot farm unless explicitly opted in (SystemCleanEmptyWS=1).
    if (SystemCleanEmptyWSAllowed() && g_appliedFocus == 0)
        emptied = tasx_empty_working_sets_system() != 0;

    if (announce && (standby || emptied))
        LOGI("[TASX] System clean: standby %s%s",
             standby ? "purged" : "skipped",
             emptied ? " + all working sets emptied" : "");
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
        // is low - debounce so the event queue is not flooded.
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
    TimerOffIfFarmNoFocus();
    FFlagsMarkDirty();
    FFlagsApply();
    TweaksApplyOneShot();
    JobsRefreshDynamic(g_appliedFocus != 0);
}

static std::wstring GetPagefileVolumeRoot()
{
    wchar_t sysDrive[16] = L"C:";
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
            0, KEY_QUERY_VALUE, &h) == ERROR_SUCCESS) {
        wchar_t buf[512] = {};
        DWORD type = 0, sz = sizeof(buf);
        if (RegQueryValueExW(h, L"PagingFiles", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
            // REG_MULTI_SZ: first string is e.g. "C:\\pagefile.sys 0 0"
            if (buf[1] == L':' && (buf[2] == L'\\' || buf[2] == L'/')) {
                sysDrive[0] = buf[0];
                sysDrive[1] = L':';
                sysDrive[2] = L'\0';
            }
        }
        RegCloseKey(h);
    } else {
        wchar_t env[16] = {};
        if (GetEnvironmentVariableW(L"SystemDrive", env, 16) && env[1] == L':') {
            sysDrive[0] = env[0];
            sysDrive[1] = L':';
            sysDrive[2] = L'\0';
        }
    }
    std::wstring root(sysDrive);
    if (root.size() == 2) root += L"\\";
    else if (root.back() != L'\\') root += L"\\";
    return root;
}

static void CheckPagefileWarning(const char* tag)
{
    int threshGB = config_get_int("TASX", "PagefileWarnFreeGB", 8);
    if (threshGB <= 0) return;
    std::wstring root = GetPagefileVolumeRoot();
    ULARGE_INTEGER freeBytes{}, totalBytes{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &totalBytes, nullptr)) {
        // fallback to NULL (current volume)
        if (!GetDiskFreeSpaceExW(nullptr, &freeBytes, &totalBytes, nullptr))
            return;
        root = L"?\\";
    }
    ULONGLONG thresh = (ULONGLONG)threshGB << 30;
    if (freeBytes.QuadPart < thresh) {
        if (LogRateLimit(tag, 300)) {
            char driveA[8] = "?";
            if (!root.empty() && root[0] != L'?') {
                driveA[0] = (char)root[0];
                driveA[1] = ':';
                driveA[2] = '\0';
            }
            double f = (double)freeBytes.QuadPart / (1ull<<30);
            double t = (double)totalBytes.QuadPart / (1ull<<30);
            LOGW("[TASX] WARNING: Pagefile volume %s low free %.1f/%.1f GB (< %d GB) - enlarge/move the pagefile: System -> Advanced settings -> Performance -> Advanced -> Virtual memory", driveA, f, t, threshGB);
        }
    }
}

void HandleRobloxCreated(DWORD pid)
{
    CheckPagefileWarning("pagefile-low-boot");
    bool isNew = !g_rbxHandles.count(pid);
    HookClient(pid);
    if (g_rbxHandles.count(pid))
        g_retryBlocked.erase(pid); /* hooked (possibly via retry) - dequeue */
    ApplyFocusIfChanged(); /* the new instance may already be the foreground */

    if (isNew && g_rbxHandles.count(pid)) {
        if (tasx_is_elevated() && config_get_bool("TASX", "PurgeStandbyOnLaunch", 1)) {
            // Debounced: a 30-client pack spawns 30 events, one purge is enough.
            static ULONGLONG lastLaunchPurgeMs = 0;
            ULONGLONG nowPurge = GetTickCount64();
            if (nowPurge - lastLaunchPurgeMs >= 60000) {
                lastLaunchPurgeMs = nowPurge;
                if (tasx_purge_standby_list())
                    LOGI("[TASX] Standby list purged (RAM reclaimed for the game)");
            }
        }
        FFlagsMarkDirty();
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

/* Ini hygiene: warn once per unknown section|key (typos like BackgroudCpuCap
   otherwise fail silently into defaults). [FastFlags] accepts any flag name
   (filtered separately by the FFlags allowlist), so only its section name
   and the reserved keys (Preset/InjectorOwnsGraphics/ForceGraphicsFlags)
   are checked here. */
static void ValidateIniKeys()
{
    static const char* known[] = {
        "tasx|trimunfocused", "tasx|trimintervalsec", "tasx|adaptivetrim",
        "tasx|trimskipbelowmb", "tasx|farmkeephot", "tasx|hardtrimaftersec",
        "tasx|backgroundmempriority", "tasx|jobassignmode",
        "tasx|backgroundcpucappercent",
        "tasx|jobcpucappercent",
        "tasx|jobmemorycapmb", "tasx|killonagentexit",
        "tasx|commitblockthreshold", "tasx|mutebackground",
        "tasx|pinbackgroundtoecores", "tasx|dynamicaffinity",
        "tasx|focusdwellms", "tasx|focushysteresisms",
        "tasx|pagefilewarnfreegb", "tasx|lowmemreactor",
        "tasx|lowmemcooldownsec", "tasx|systemcleaner",
        "tasx|systemcleanstandby", "tasx|systemcleanemptyws",
        "tasx|systemcleanminintervalsec", "tasx|purgestandbyonlaunch",
        "tasx|selfcpuwatchdogpercent", "tasx|killcrashhandler",
        "tasx|timerresolution", "tasx|powerplan", "tasx|applytweaks",
        "tasx|disablecpuboost", "tasx|warmclientfiles", "tasx|warmmaxmb",
        "tasx|desktopheapexpand", "tasx|injectorownsgraphics",
        "tasx|forcegraphicsflags", "tasx|preset", "tasx|disabletelemetry",
        "tasx|boosthotkey", "tasx|pageinintervalsec", "tasx|farmboostdefault",
        "roblox|uncapfps", "roblox|targetfps", "roblox|renderer",
        "roblox|lighting", "roblox|texturequality",
        "roblox|disabletelemetry", "roblox|extraversionsdirs",
        "etw|disabletelemetry", "log|loglevel", "log|logfile",
        "net|sharedcacheroot", "net|cachelinks",
        // [FastFlags] section name itself (+ any flag key, see above)
        "fastflags|preset", "fastflags|injectorownsgraphics",
        "fastflags|forcegraphicsflags", "fastflags|jobassignmode",
    };
    int cnt = config_get_entry_count();
    for (int i = 0; i < cnt; ++i) {
        char sec[32] = {}, key[64] = {}, val[192] = {};
        if (!config_get_entry(i, sec, sizeof(sec), key, sizeof(key), val, sizeof(val)))
            continue;
        char lowSec[32] = {}, lowKey[64] = {}, low[96] = {};
        size_t a = 0;
        for (; sec[a] && a + 1 < sizeof(lowSec); ++a) {
            char c = sec[a];
            if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
            lowSec[a] = c;
        }
        size_t b = 0;
        for (; key[b] && b + 1 < sizeof(lowKey); ++b) {
            char c = key[b];
            if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
            lowKey[b] = c;
        }
        if (strcmp(lowSec, "fastflags") == 0) continue; // any flag name allowed
        snprintf(low, sizeof(low), "%s|%s", lowSec, lowKey);
        bool ok = false;
        for (size_t k = 0; k < sizeof(known) / sizeof(known[0]); ++k) {
            if (strcmp(low, known[k]) == 0) { ok = true; break; }
        }
        if (!ok) {
            char tag[128];
            snprintf(tag, sizeof(tag), "ini-unknown-%s", low);
            if (LogRateLimit(tag, 3600))
                LOGW("[TASX] WARNING: unknown ini key [%s] %s - typo? ignored, default applies",
                     sec, key);
        }
    }
}

/* One-time startup in dependency order. Returns 1 when the main loop
   should run, 0 when the process must exit silently (already running). */
int InitSubsystems()
{
    tasx_log_init();

    config_default_path(g_iniPath, MAX_PATH);
    // If ini is missing, create documented defaults atomically
    {
        DWORD attr = GetFileAttributesA(g_iniPath);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            if (config_create_default(g_iniPath)) {
                // will be loaded below
            }
        }
    }
    config_load(g_iniPath);
    tasx_log_configure(config_get_str("Log", "LogFile", ""),
                       tasx_log_level_from_str(config_get_str("Log", "LogLevel", "info")));
    g_iniMtime = IniMtimeKey();
    if (!config_loaded()) {
        LOGW("[TASX] WARNING: TASX.ini not found - built-in defaults apply. Expected path: %s", g_iniPath);
        if (config_create_default(g_iniPath))
            LOGI("[TASX] Created default TASX.ini at %s", g_iniPath);
    } else {
        LOGI("[TASX] Config: %s (loaded)", g_iniPath);
        ValidateIniKeys();
    }

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
    if (!config_get_bool("TASX", "TrimUnfocused", 1)) StopAllTrimmers();

    // Startup banner: admin status, job mode, enabled/disabled features.
    // Printed ONCE - replaces the old per-feature "needs admin" spam.
    LOGI("[TASX] Admin: %s | Job mode: %s",
         elevated ? "YES (elevated)" : "NO (limited mode)",
         jobsOk ? "native cgroup + per-process focus" : "per-process fallback");
    if (!elevated) {
        LOGI("[TASX] Limited mode WITHOUT admin - DISABLED: standby purge, system cleaner, HKLM tweaks, ETW, CPU boost. ENABLED: priority, affinity, FFlags, crash-handler kill, trimmer(soft). Run Install\\ScheduledTaskInstaller.bat for full effect.");
    } else {
        LOGI("[TASX] Features: priority, affinity, jobs, FFlags, trimmer, standby purge, system cleaner, HKLM tweaks - all ENABLED");
    }

    // FPS config sanity (single place): UncapFps=1 makes TargetFps
    // meaningless - the FFlags writer ignores it and uncaps to 999.
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

    g_hotkey = new FarmHotkey();
    g_hotkey->Start();

    LowMemReactorStart();

    SweepDiscover();
    ApplyFocusIfChanged();
    UpdateWorkingSetCache();
    ULONGLONG now = GetTickCount64();
    g_lastCrashSweepMs = now;
    g_lastRetryMs = now;
    g_lastIniCheckMs = now;
    if (!g_rbxHandles.empty()) { TweaksPowerEnter(); TimerOn(); }

    // Page-in hotkey (always P+E); no startup page-in by default.
    LOGI("[TASX] Launched");
    return 1;
}

/* Teardown in reverse order. Runs on Ctrl/close/logoff and on natural exit
   (the main loop only ends via g_stop, so this is always reached). */
void ShutdownSubsystems()
{
    LOGI("[TASX] Shutting down...");
    delete g_hotkey;
    g_hotkey = nullptr;
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
            case Ev::HotkeyBoost:   PageInFarm();                 break;
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
                // SystemCleanPass is internally split: standby always, global
                // empty-WS only when allowed AND nothing focused.
                if (g_appliedFocus == 0) {
                    SystemCleanPass(true);
                } else {
                    if (config_get_bool("TASX", "SystemCleanStandby", 1))
                        tasx_purge_standby_list(); // safe half still applies
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

        // Auto page-in every N minutes (default 300 = 5 min) when clients active
        int pageInSec = config_get_int("TASX", "PageInIntervalSec", 300);
        if (pageInSec > 0 && !g_rbxHandles.empty() && (now - g_lastPageInMs) >= (ULONGLONG)pageInSec * 1000 && !g_pageInBusy) {
            g_lastPageInMs = now;
            PageInFarm();
        }

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

            // periodic pagefile free-space check + farm budget + self CPU watchdog.
            CheckPagefileWarning("pagefile-low");
            FarmBudgetCheck();
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
