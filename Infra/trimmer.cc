#include "trimmer.h"

#include "CPU.h"
#include "config.h"
#include "ntsys.h"
#include "log.h"
#include "lograte.h"

#include <psapi.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

namespace {

struct Due {
    std::int64_t ms;
    DWORD pid;
};
struct DueGreater {
    bool operator()(const Due& a, const Due& b) const { return a.ms > b.ms; }
};

std::mutex g_mtx;
std::priority_queue<Due, std::vector<Due>, DueGreater> g_heap;
std::unordered_map<DWORD, HANDLE> g_handles;   /* pid -> proc handle */
std::unordered_map<DWORD, SIZE_T> g_wsCache;   /* pid -> last WS bytes (from master) */
std::unordered_map<DWORD, std::int64_t> g_wsTime; /* pid -> cache timestamp ms */
std::unordered_map<DWORD, std::int64_t> g_unfocusedSince; /* pid -> when lost focus */
std::atomic<DWORD> g_focusedPid{0};
std::atomic<int> g_pageInPause{0};

HANDLE g_hThread = nullptr;
HANDLE g_hStop = nullptr;
HANDLE g_hWake = nullptr;
/* NOTE: no LowMem handle here by design. The single low-memory owner is
   master.cpp LowMemReactorStart (standby + aggressive trim + system-clean
   gating). A second watcher in this module caused double-trim per signal. */

std::int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t ClampInterval(std::int64_t ms)
{
    if (ms < 3000) return 3000;
    if (ms > 600000) return 600000;
    return ms;
}

namespace {

/* Shared snapshot of memory pressure with TTL = avoids 20 NT calls per tick.
   Both trimmer IntervalMs() and master low-mem reactor use this if needed. */
static std::int64_t g_snapshotMs = 0;
static int g_snapshotLoad = 50;
static int g_snapshotCommit = -1;
static std::mutex g_snapshotMtx;

int GetPressureLoad()
{
    std::lock_guard<std::mutex> lk(g_snapshotMtx);
    std::int64_t now = NowMs();
    if (now - g_snapshotMs < 5000) return g_snapshotLoad; // TTL 5s
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) g_snapshotLoad = (int)ms.dwMemoryLoad;
    else g_snapshotLoad = 50;
    g_snapshotCommit = tasx_get_commit_percent();
    g_snapshotMs = now;
    return g_snapshotLoad;
}

int GetPressureCommit()
{
    std::lock_guard<std::mutex> lk(g_snapshotMtx);
    std::int64_t now = NowMs();
    if (now - g_snapshotMs < 5000) return g_snapshotCommit;
    GetPressureLoad(); // refresh
    return g_snapshotCommit;
}

} // namespace

/* Base interval modulated by system memory pressure (shared TTL snapshot). */
std::int64_t IntervalMs()
{
    std::int64_t base = (std::int64_t)config_get_int("TASX", "TrimIntervalSec", 10) * 1000;

    if (!config_get_bool("TASX", "AdaptiveTrim", 1))
        return ClampInterval(base);

    int load = GetPressureLoad();
    if (load >= 90) return ClampInterval(base / 4);   /* farm is choking RAM */
    if (load >= 75) return ClampInterval(base / 2);
    if (load <= 60) return ClampInterval(base * 2);   /* plenty of headroom */
    int commitPct = GetPressureCommit();
    if (commitPct >= 80) return ClampInterval(base / 2);
    if (commitPct >= 0 && commitPct < 50) return ClampInterval(base * 2);
    return ClampInterval(base);
}

/* Farm keep-hot policy: unfocused farm clients are WORKING, not idle.
   With FarmKeepHot=1 (default on farm* presets) the periodic pass is
   soft-only; HardTrim (EmptyWorkingSet) fires only after HardTrimAfterSec
   of continuous unfocus (default 1800s) or on commit-critical (>=90%).
   The old "hard after 2*interval" (~6-20s) is what evicted whole farms. */
static bool IsFarmKeepHot()
{
    const char* v = config_get_str("TASX", "FarmKeepHot", nullptr);
    if (v) return config_get_bool("TASX", "FarmKeepHot", 1) != 0;
    const char* pPre = config_get_str("FastFlags", "Preset", nullptr);
    if (!pPre) pPre = config_get_str("TASX", "Preset", nullptr);
    if (pPre) {
        char pl[16] = {};
        size_t pn = 0;
        for (; pPre[pn] && pn + 1 < sizeof(pl); ++pn)
            pl[pn] = (char)tolower((unsigned char)pPre[pn]);
        if (strcmp(pl, "farm15") == 0 || strcmp(pl, "farm20") == 0 ||
            strcmp(pl, "farm30") == 0)
            return true;
    }
    return false;
}

static std::int64_t HardTrimAfterMs()
{
    int s = config_get_int("TASX", "HardTrimAfterSec", 1800);
    if (s < 60) s = 60;
    if (s > 86400) s = 86400;
    return (std::int64_t)s * 1000;
}

static unsigned long BgMemPrio()
{
    return CpuBackgroundMemPrio(); // single source of truth lives in CPU.cc
}

void SoftTrim(HANDLE h)
{
    // Background memory priority + release of the trimmed page cost only.
    // Deliberately NO QUOTA_LIMITS_HARDWS_MIN_ENABLE: pinning the current
    // working set as a hard minimum would fight every later trim.
    tasx_set_memory_priority(h, BgMemPrio());
    SetProcessWorkingSetSizeEx(h, (SIZE_T)-1, (SIZE_T)-1, 0);
}

void HardTrim(HANDLE h)
{
    EmptyWorkingSet(h);
}

bool BelowSkipThresholdCached(DWORD pid)
{
    int skipMB = config_get_int("TASX", "TrimSkipBelowMB", 250);
    if (skipMB <= 0) return false;

    auto it = g_wsCache.find(pid);
    auto itT = g_wsTime.find(pid);
    if (it == g_wsCache.end() || itT == g_wsTime.end()) return false;

    std::int64_t age = NowMs() - itT->second;
    if (age > 30000) return false; // stale -> don't skip, allow trim; master will refresh

    const SIZE_T limit = (SIZE_T)(unsigned)skipMB << 20;
    return it->second < limit;
}

DWORD WINAPI SchedulerThread(LPVOID)
{
    std::int64_t lastLog = NowMs();
    int trimmed = 0, skipped = 0, softOnly = 0;

    HANDLE waitHandles[2];
    waitHandles[0] = g_hStop;
    waitHandles[1] = g_hWake;

    while (true)
    {
        std::int64_t timeoutMs;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_heap.empty()) {
                std::int64_t due = g_heap.top().ms;
                std::int64_t now = NowMs();
                if (due <= now) timeoutMs = 0;
                else timeoutMs = due - now;
                if (timeoutMs > 600000) timeoutMs = 600000;
            } else {
                timeoutMs = INT64_MAX; /* infinite below */
            }
        }

        DWORD to = (timeoutMs == INT64_MAX) ? INFINITE : (DWORD)timeoutMs;
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, to);

        if (waitResult == WAIT_OBJECT_0) {
            break; /* stop */
        }
        if (waitResult == WAIT_OBJECT_0 + 1) {
            continue; /* woken by Start/Stop/Focus — re-evaluate heap top */
        }
        if (waitResult == WAIT_FAILED) {
            Sleep(100);
            continue;
        }
        /* WAIT_TIMEOUT: a heap deadline fired — fall through. */

        Due d{};
        bool haveDue = false;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_heap.empty()) continue;
            if (g_heap.top().ms > NowMs()) continue; // woken early
            d = g_heap.top();
            g_heap.pop();
            haveDue = true;
        }
        if (!haveDue) continue;

        DWORD pid = d.pid;
        if (g_pageInPause.load() || g_focusedPid.load() == pid) {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_heap.push({NowMs() + 3000, pid}); // never fight the player
            g_unfocusedSince.erase(pid);
            continue;
        }

        HANDLE h = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            auto it = g_handles.find(pid);
            if (it != g_handles.end()) h = it->second;
        }

        if (h) {
            // Commit-critical path: hard trim, but still never the focused
            // instance (filtered above) and never below-skip-threshold
            // processes (trimming them only causes re-faults, zero gain).
            int pct = tasx_get_commit_percent();
            if (pct >= 90) {
                if (BelowSkipThresholdCached(pid)) {
                    ++skipped;
                } else {
                    if (LogRateLimit("trim-commit90", 60))
                        LOGW("[Trimmer] Commit %d%% -> hard trim PID %lu (reason: commit-critical)",
                             pct, pid);
                    HardTrim(h);
                    ++trimmed;
                    static std::int64_t lastPurge = 0;
                    std::int64_t now = NowMs();
                    if (now - lastPurge > 30000) {
                        if (tasx_is_elevated()) tasx_purge_standby_list();
                        lastPurge = now;
                    }
                }
            } else if (BelowSkipThresholdCached(pid)) {
                ++skipped;
                // no syscall, just count
            } else {
                // Two-phase logic (farm-aware)
                std::int64_t unfocusedMs = 0;
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    auto itU = g_unfocusedSince.find(pid);
                    if (itU != g_unfocusedSince.end()) {
                        unfocusedMs = NowMs() - itU->second;
                    } else {
                        // first time we see unfocused, mark now
                        g_unfocusedSince[pid] = NowMs();
                        unfocusedMs = 0;
                    }
                }
                // Soft pass always
                SoftTrim(h);
                bool doHard = false;
                if (IsFarmKeepHot()) {
                    // farm: hard only after hours-scale inactivity, never on cadence
                    doHard = (unfocusedMs > HardTrimAfterMs());
                } else {
                    std::int64_t interval = IntervalMs();
                    doHard = (unfocusedMs > 2 * interval);
                }
                if (doHard) {
                    HardTrim(h);
                    ++trimmed;
                } else {
                    ++softOnly;
                }
            }

            std::lock_guard<std::mutex> lk(g_mtx);
            g_heap.push({NowMs() + IntervalMs() + (std::int64_t)(pid % 5) * 1000, pid});
        } else {
            // stale entry (client released): drop, no re-push
            std::lock_guard<std::mutex> lk(g_mtx);
            g_unfocusedSince.erase(pid);
        }

        std::int64_t now = NowMs();
        if (now - lastLog >= 30000) {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_handles.empty()) {
                // No clients: drop stale window counters instead of printing
                // someone else's stats under "0 client(s)".
                trimmed = 0;
                skipped = 0;
                softOnly = 0;
                lastLog = now;
            } else {
            // Farm observability: avg WS + commit in the same line, zero new
            // syscalls (wsCache already here, pressure via 5s TTL snapshot).
            unsigned long long wsSum = 0;
            for (auto& kv : g_wsCache) wsSum += (unsigned long long)kv.second;
            unsigned avgMB = g_wsCache.empty() ? 0
                : (unsigned)(wsSum / g_wsCache.size() >> 20);
            int commit = g_snapshotCommit;
            LOGI("[Trimmer] %u client(s), %d hard trimmed, %d soft, %d skipped (30s) | avg WS %u MB, commit %d%%",
                 (unsigned)g_handles.size(), trimmed, softOnly, skipped, avgMB, commit);
            lastLog = now;
            trimmed = 0;
            skipped = 0;
            softOnly = 0;
            } // end else (non-empty): log summary
        }
    }
    return 0;
}

} // namespace

void TrimmerSetPageInPause(int on)
{
    g_pageInPause.store(on ? 1 : 0);
    if (g_hWake) SetEvent(g_hWake);
}

void TrimmerSetFocused(DWORD pid)
{
    g_focusedPid.store(pid ? pid : 0);
    std::lock_guard<std::mutex> lk(g_mtx);
    if (pid) {
        g_unfocusedSince.erase(pid);
    } else {
        // lost focus: mark all current as unfocused start if not already
        std::int64_t now = NowMs();
        for (auto &kv : g_handles) {
            if (g_unfocusedSince.find(kv.first) == g_unfocusedSince.end())
                g_unfocusedSince[kv.first] = now;
        }
    }
    if (g_hWake) SetEvent(g_hWake);
}

bool StartTrimmer(DWORD pid, HANDLE processHandle)
{
    if (!config_get_bool("TASX", "TrimUnfocused", 1))
        return false;

    bool needStart = false;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (g_handles.count(pid)) return true;

        g_handles[pid] = processHandle;
        // Jitter deadlines by PID so N clients don't trim in one burst
        // (thundering herd -> fault storm). Deterministic, no rand() needed.
        g_heap.push({NowMs() + IntervalMs() + (std::int64_t)(pid % 7) * 1000, pid});
        // initialize unfocused tracking: if not focused, start counting
        if (g_focusedPid.load() != pid) {
            if (g_unfocusedSince.find(pid) == g_unfocusedSince.end())
                g_unfocusedSince[pid] = NowMs();
        }
        needStart = (g_hThread == nullptr);
        if (needStart) {
            g_hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_hWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
    }

    if (needStart) {
        g_hThread = CreateThread(nullptr, 0, SchedulerThread, nullptr, 0, nullptr);
        if (!g_hThread) {
            LOGE("[Trimmer] Failed to create scheduler thread");
            if (g_hStop) { CloseHandle(g_hStop); g_hStop = nullptr; }
            if (g_hWake) { CloseHandle(g_hWake); g_hWake = nullptr; }
            return false;
        }
    }

    if (g_hWake) SetEvent(g_hWake);
    return true;
}

void StopTrimmer(DWORD pid)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_handles.erase(pid);
    g_wsCache.erase(pid);
    g_wsTime.erase(pid);
    g_unfocusedSince.erase(pid);
    // heap entry is skipped lazily on pop
    if (g_hWake) SetEvent(g_hWake);
}

void TrimmerUpdateWorkingSet(DWORD pid, SIZE_T ws)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_wsCache[pid] = ws;
    g_wsTime[pid] = NowMs();
}

void TrimmerRemoveWorkingSet(DWORD pid)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_wsCache.erase(pid);
    g_wsTime.erase(pid);
}

void TrimmerTrimAllAggressive()
{
    // Hard trim ONLY for long-inactive background clients.
    // Farm keep-hot: hard only after HardTrimAfterSec; otherwise 2x interval.
    // Below-TrimSkipBelowMB processes are skipped entirely (0 syscalls
    // beyond the cached check). Focused instance is never touched.
    // FarmBoost ON: the whole farm runs hot — nothing is trimmed at all.
    if (g_pageInPause.load()) {
        if (LogRateLimit("trim-pagein", 60))
            LOGI("[Trimmer] Aggressive skipped (page-in in progress)");
        return;
    }
    DWORD focused = g_focusedPid.load();
    int skipMB = config_get_int("TASX", "TrimSkipBelowMB", 250);
    std::int64_t now = NowMs();
    std::int64_t hardAfterMs = IsFarmKeepHot() ? HardTrimAfterMs()
                                               : 2 * IntervalMs();
    std::vector<HANDLE> soft;
    std::vector<HANDLE> hard;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        for (auto& kv : g_handles) {
            if (kv.first == focused) continue;
            auto itW = g_wsCache.find(kv.first);
            auto itT = g_wsTime.find(kv.first);
            if (skipMB > 0 && itW != g_wsCache.end() && itT != g_wsTime.end() &&
                (now - itT->second) <= 30000 &&
                itW->second < (SIZE_T)(unsigned)skipMB << 20)
                continue; // below threshold, fresh cache: skip
            auto itU = g_unfocusedSince.find(kv.first);
            std::int64_t age = (itU != g_unfocusedSince.end()) ? (now - itU->second) : 0;
            if (age > hardAfterMs)
                hard.push_back(kv.second);
            else
                soft.push_back(kv.second);
        }
    }
    for (HANDLE h : soft) SoftTrim(h);
    for (HANDLE h : hard) HardTrim(h);
    if (!soft.empty() || !hard.empty())
        LOGI("[Trimmer] Aggressive: %u soft, %u hard (reason: low-mem/commit, focused untouched)",
             (unsigned)soft.size(), (unsigned)hard.size());
}

void StopAllTrimmers()
{
    if (g_hStop) SetEvent(g_hStop);
    if (g_hThread) {
        WaitForSingleObject(g_hThread, 5000);
        CloseHandle(g_hThread);
        g_hThread = nullptr;
    }
    if (g_hStop) { CloseHandle(g_hStop); g_hStop = nullptr; }
    if (g_hWake) { CloseHandle(g_hWake); g_hWake = nullptr; }

    std::lock_guard<std::mutex> lock(g_mtx);
    g_handles.clear();
    g_wsCache.clear();
    g_wsTime.clear();
    g_unfocusedSince.clear();
    while (!g_heap.empty()) g_heap.pop();
}
