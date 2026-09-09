#include "trimmer.h"

#include "config.h"
#include "ntsys.h"
#include "lograte.h"

#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
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

HANDLE g_hThread = nullptr;
HANDLE g_hStop = nullptr;
HANDLE g_hWake = nullptr;
HANDLE g_hLowMem = nullptr;

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

/* Base interval modulated by system memory pressure. */
std::int64_t IntervalMs()
{
    std::int64_t base = (std::int64_t)config_get_int("TASX", "TrimIntervalSec", 10) * 1000;

    if (!config_get_bool("TASX", "AdaptiveTrim", 1))
        return ClampInterval(base);

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    int load = 50;
    if (GlobalMemoryStatusEx(&ms))
        load = (int)ms.dwMemoryLoad;

    if (load >= 90) return ClampInterval(base / 4);   /* farm is choking RAM */
    if (load >= 75) return ClampInterval(base / 2);
    if (load <= 60) return ClampInterval(base * 2);   /* plenty of headroom */
    /* Also adapt by commit charge (STEP 3) */
    int commitPct = tasx_get_commit_percent();
    if (commitPct >= 80) return ClampInterval(base / 2);
    if (commitPct < 50) return ClampInterval(base * 2);
    return ClampInterval(base);
}

void SoftTrim(HANDLE h)
{
    // VeryLow memory priority + minimal working set threshold
    tasx_set_memory_priority(h, 1);
    // QUOTA_LIMITS_HARDWS_MIN_ENABLE = 0x1 enforces hard min; using -1 keeps current but marks as hard-state ready
#ifndef QUOTA_LIMITS_HARDWS_MIN_ENABLE
#define QUOTA_LIMITS_HARDWS_MIN_ENABLE 0x00000001
#endif
    SetProcessWorkingSetSizeEx(h, (SIZE_T)-1, (SIZE_T)-1, QUOTA_LIMITS_HARDWS_MIN_ENABLE);
}

void HardTrim(HANDLE h)
{
    EmptyWorkingSet(h);
}

bool BelowSkipThresholdCached(DWORD pid)
{
    int skipMB = config_get_int("TASX", "TrimSkipBelowMB", 150);
    if (skipMB <= 0) return false;

    auto it = g_wsCache.find(pid);
    auto itT = g_wsTime.find(pid);
    if (it == g_wsCache.end() || itT == g_wsTime.end()) return false;

    std::int64_t age = NowMs() - itT->second;
    if (age > 30000) return false; // stale -> don't skip, allow trim; master will refresh

    const SIZE_T limit = (SIZE_T)skipMB << 20;
    return it->second < limit;
}

DWORD WINAPI SchedulerThread(LPVOID)
{
    std::int64_t lastLog = NowMs();
    int trimmed = 0, skipped = 0, softOnly = 0;

    HANDLE waitHandles[3];
    waitHandles[0] = g_hStop;
    waitHandles[1] = g_hLowMem;
    waitHandles[2] = g_hWake;

    while (true)
    {
        std::int64_t timeoutMs = INFINITE;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_heap.empty()) {
                std::int64_t due = g_heap.top().ms;
                std::int64_t now = NowMs();
                if (due <= now) timeoutMs = 0;
                else timeoutMs = due - now;
                if (timeoutMs > 600000) timeoutMs = 600000;
            } else {
                timeoutMs = INFINITE;
            }
        }

        DWORD waitResult = 0;
        if (g_hLowMem) {
            waitResult = WaitForMultipleObjects(3, waitHandles, FALSE, timeoutMs == (std::int64_t)INFINITE ? INFINITE : (DWORD)timeoutMs);
        } else {
            HANDLE w2[2] = { g_hStop, g_hWake };
            DWORD to = timeoutMs == (std::int64_t)INFINITE ? INFINITE : (DWORD)timeoutMs;
            waitResult = WaitForMultipleObjects(2, w2, FALSE, to);
            // map to 3-handle indices
            if (waitResult == WAIT_OBJECT_0 + 1) waitResult = WAIT_OBJECT_0 + 2; // wake
        }

        if (waitResult == WAIT_OBJECT_0) {
            // stop
            break;
        }
        if (waitResult == WAIT_OBJECT_0 + 1 && g_hLowMem) {
            // BUG 1 fix: the notification is manual-reset and stays signaled
            // while memory is low; without a cooldown this branch spins
            // (log + trim storm). Config-gated cooldown (LowMemCooldownSec).
            std::int64_t nowMs = NowMs();
            static std::int64_t lastLowMemMs = 0;
            int cooldownSec = config_get_int("TASX", "LowMemCooldownSec", 30);
            if (nowMs - lastLowMemMs < (std::int64_t)cooldownSec * 1000) {
                BOOL dummy = FALSE;
                QueryMemoryResourceNotification(g_hLowMem, &dummy);
                Sleep(1000);
                continue;
            }
            lastLowMemMs = nowMs;
            std::cout << "[Trimmer] LowMem signal -> aggressive trim (cooldown "
                      << cooldownSec << "s)" << std::endl;
            int pct = tasx_get_commit_percent();
            if (pct >= 90) {
                std::cout << "[Trimmer] Commit " << pct << "% >90% -> hard trim + standby purge" << std::endl;
                TrimmerTrimAllAggressive();
                if (tasx_is_elevated()) tasx_purge_standby_list();
            } else {
                TrimmerTrimAllAggressive();
            }
            Sleep(cooldownSec * 1000);   // enforce cooldown
            continue;
        }
        if (waitResult == WAIT_OBJECT_0 + 2) {
            // wake from StartTrimmer / TrimmerSetFocused etc. - re-evaluate heap top
            continue;
        }
        if (waitResult == WAIT_TIMEOUT) {
            // heap deadline reached
        } else if (waitResult == WAIT_FAILED) {
            // error, sleep briefly
            Sleep(100);
            continue;
        }

        // Process due entries
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
        if (g_focusedPid.load() == pid) {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_heap.push({NowMs() + 3000, pid}); // never fight the player
            // update unfocusedSince cleared when focused
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
            // Check commit >90 inside trim path as well (periodic).
            // Even under pressure: never touch focused (handled above),
            // skip processes already below TrimSkipBelowMB (trimming them
            // only causes re-faults for zero gain).
            int pct = tasx_get_commit_percent();
            if (pct >= 90) {
                if (BelowSkipThresholdCached(pid)) {
                    ++skipped;
                } else {
                    if (LogRateLimit("trim-commit90", 60))
                        std::cout << "[Trimmer] Commit " << pct << "% -> hard trim PID "
                                  << pid << " (reason: commit-critical)" << std::endl;
                    HardTrim(h);
                    ++trimmed;
                    // also purge standby once per cycle
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
                // Two-phase logic
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
                std::int64_t interval = IntervalMs();
                // Soft pass always
                SoftTrim(h);
                if (unfocusedMs > 2 * interval) {
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
            std::cout << "[Trimmer] " << g_handles.size() << " client(s), "
                      << trimmed << " hard trimmed, " << softOnly << " soft, " << skipped
                      << " skipped (30s)" << std::endl;
            lastLog = now;
            trimmed = 0;
            skipped = 0;
            softOnly = 0;
        }
    }
    return 0;
}

} // namespace

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
            // NOTE: no CreateMemoryResourceNotification here by design.
            // The single LowMem owner is master.cpp LowMemReactorStart
            // (it does standby + aggressive + SystemClean gating with
            // SystemCleanMinIntervalSec). A second watcher in this module
            // caused double-trim on every signal. g_hLowMem stays null and
            // the scheduler below runs on pure interval deadlines.
            g_hLowMem = nullptr;
        }
    }

    if (needStart) {
        g_hThread = CreateThread(nullptr, 0, SchedulerThread, nullptr, 0, nullptr);
        if (!g_hThread) {
            std::cout << "[Trimmer] Failed to create scheduler thread" << std::endl;
            if (g_hStop) { CloseHandle(g_hStop); g_hStop = nullptr; }
            if (g_hWake) { CloseHandle(g_hWake); g_hWake = nullptr; }
            if (g_hLowMem) { CloseHandle(g_hLowMem); g_hLowMem = nullptr; }
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

void TrimmerTrimAll()
{
    // Legacy entry: soft-only by design. Hard trim lives exclusively in
    // TrimmerTrimAllAggressive with unfocused-age + skip-threshold guards.
    // Focused instance is never touched.
    std::vector<HANDLE> targets;
    DWORD focused = g_focusedPid.load();

    {
        std::lock_guard<std::mutex> lock(g_mtx);
        for (auto& kv : g_handles)
            if (kv.first != focused)
                targets.push_back(kv.second);
    }

    for (HANDLE h : targets) {
        SoftTrim(h);
    }
}

void TrimmerTrimAllAggressive()
{
    // Hard trim ONLY for long-inactive background clients:
    // unfocused longer than 2x current interval. Everyone else gets soft.
    // Below-TrimSkipBelowMB processes are skipped entirely (0 syscalls
    // beyond the cached check). Focused instance is never touched.
    DWORD focused = g_focusedPid.load();
    int skipMB = config_get_int("TASX", "TrimSkipBelowMB", 150);
    std::int64_t now = NowMs();
    std::int64_t interval = IntervalMs();
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
                itW->second < (SIZE_T)((SIZE_T)skipMB << 20))
                continue; // below threshold, fresh cache: skip
            auto itU = g_unfocusedSince.find(kv.first);
            std::int64_t age = (itU != g_unfocusedSince.end()) ? (now - itU->second) : 0;
            if (age > 2 * interval)
                hard.push_back(kv.second);
            else
                soft.push_back(kv.second);
        }
    }
    for (HANDLE h : soft) SoftTrim(h);
    for (HANDLE h : hard) HardTrim(h);
    if (!soft.empty() || !hard.empty())
        std::cout << "[Trimmer] Aggressive: " << soft.size() << " soft, "
                  << hard.size() << " hard (reason: low-mem/commit, focused untouched)"
                  << std::endl;
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
    if (g_hLowMem) { CloseHandle(g_hLowMem); g_hLowMem = nullptr; }

    std::lock_guard<std::mutex> lock(g_mtx);
    g_handles.clear();
    g_wsCache.clear();
    g_wsTime.clear();
    g_unfocusedSince.clear();
    while (!g_heap.empty()) g_heap.pop();
}
