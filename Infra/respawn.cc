#include "respawn.h"

#include "config.h"
#include "log.h"
#include "lograte.h"
#include "ntsys.h"

/* Windows command-line grammar helpers.
   SplitCmdLine is exactly the grammar CommandLineToArgvW implements (the
   one every CRT uses to rebuild argv) - using the real API keeps us honest
   with rounding quirks like trailing backslashes before a closing quote. */
std::vector<std::wstring> SplitCmdLine(const std::wstring& cmdline)
{
    std::vector<std::wstring> out;
    if (cmdline.empty()) return out;

    std::wstring copy = cmdline;
    copy.push_back(L'\0');

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(copy.data(), &argc);
    if (!argv) return out;

    out.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i)
        out.emplace_back(argv[i]);
    LocalFree(argv);
    return out;
}

/* Reverse of CommandLineToArgvW: rebuilds a command line that the parser
   splits back into exactly argv. An argument is quoted when it contains
   whitespace or a quote; embedded quotes become \" (backslash count before
   the closing quote is doubled so a trailing backslash round-trips). */
std::wstring BuildCmdLine(const std::vector<std::wstring>& argv)
{
    std::wstring cmd;
    bool first = true;
    for (const auto& raw : argv) {
        if (!first) cmd += L' ';
        first = false;

        std::wstring a = raw;
        bool needQuote = a.find_first_of(L" \t\"") != std::wstring::npos;
        if (!needQuote) {
            cmd += a;
            continue;
        }

        std::wstring q = L"\"";
        size_t run = 0; /* consecutive backslashes before the next char */
        size_t i = 0;
        while (i < a.size()) {
            wchar_t c = a[i];
            if (c == L'\\') { ++run; ++i; continue; }
            if (c == L'\"') {
                /* backslashes BEFORE a quote are literal (keep them), the
                   quote itself gets escaped */
                q.append(run, L'\\');
                run = 0;
                q += L"\\\"";
                ++i;
                continue;
            }
            q.append(run, L'\\');
            run = 0;
            q += c;
            ++i;
        }
        /* trailing backslashes before the closing quote must be doubled */
        q.append(run * 2, L'\\');
        q += L'\"';
        cmd += q;
    }
    return cmd;
}
/* Sliding-window rate limiters for spawn bursts. */
namespace {

std::unordered_map<DWORD, std::wstring> g_cmdlines;
std::unordered_map<DWORD, std::vector<ULONGLONG>> g_perPidTimes; /* ms */
std::vector<ULONGLONG> g_globalTimes;                            /* ms */

static void PruneWindow(std::vector<ULONGLONG>& v, ULONGLONG now)
{
    size_t c = 0;
    for (ULONGLONG t : v)
        if (now - t < 3600000ull) v[c++] = t;   /* last 1h */
    v.resize(c);
}

static bool HourOverLimit(std::vector<ULONGLONG>& v, ULONGLONG now, int cap)
{
    if (cap <= 0) return false; /* 0 = unlimited */
    PruneWindow(v, now);
    return (int)v.size() >= cap;
}

} /* namespace */

void RespawnRemember(DWORD pid, const std::wstring& cmdline)
{
    if (pid == 0 || cmdline.empty()) return;
    g_cmdlines[pid] = cmdline;
    g_perPidTimes[pid]; /* ensure bucket exists */
}

void RespawnForget(DWORD pid)
{
    g_cmdlines.erase(pid);
    g_perPidTimes.erase(pid);
}

void RespawnClearAll(void)
{
    g_cmdlines.clear();
    g_perPidTimes.clear();
    g_globalTimes.clear();
}

int RespawnConfigured(void)
{
    return config_get_bool("TASX", "RespawnOnCrash", 0) &&
           !g_cmdlines.empty();
}

size_t RespawnPendingCount(void)
{
    return g_cmdlines.size();
}

int RespawnTrySpawn(DWORD exitedPid)
{
    if (exitedPid == 0) return 0;
    if (!config_get_bool("TASX", "RespawnOnCrash", 0))
        return 0;

    auto it = g_cmdlines.find(exitedPid);
    if (it == g_cmdlines.end())
        return 0; /* never hooked (e.g. legacy pre-respawn run) - nothing to respawn */

    std::wstring cmdline = it->second;
    ULONGLONG now = GetTickCount64();
int perPidCap = config_get_int("TASX", "RespawnPerHourMax", 6);
    if (perPidCap < 0) perPidCap = 0;
    auto bucket = g_perPidTimes.find(exitedPid);
    if (bucket != g_perPidTimes.end() &&
        HourOverLimit(bucket->second, now, perPidCap)) {
        if (LogRateLimit("respawn-perpid-limit", 300))
            LOGW("[TASX] Respawn cap hit for PID %lu (%d/h) - client stays dead to avoid a crash loop",
                 exitedPid, perPidCap);
        g_cmdlines.erase(it);
        g_perPidTimes.erase(bucket);
        return 0;
    }

    int globalCap = config_get_int("TASX", "RespawnGlobalHourCap", 60);
    if (globalCap < 0) globalCap = 0;
    if (HourOverLimit(g_globalTimes, now, globalCap)) {
        if (LogRateLimit("respawn-global-limit", 300))
            LOGW("[TASX] Global respawn cap reached (%d/h) - refusing further auto-respawns", globalCap);
        return 0;
    }

    /* Honour the commit-charge guard: spawning into an OOM-ing system just
       makes the next crash worse. */
    int blockPct = config_get_int("TASX", "CommitBlockThreshold", 85);
    if (blockPct <= 0) blockPct = 85;
    int curPct = tasx_get_commit_percent();
    if (curPct >= 0 && curPct >= blockPct) {
        if (LogRateLimit("respawn-commit-block", 60))
            LOGW("[TASX] Respawn of PID %lu deferred - commit charge %d%% >= %d%%",
                 exitedPid, curPct, blockPct);
        return 0;
    }

    /* Normalize the captured command line (split -> rebuild).
       CommandLineToArgvW is lossless here: split(build(split(x))) == split(x),
       so this can only clean up quoting cosmetics, never change arguments. */
    std::vector<std::wstring> argv = SplitCmdLine(cmdline);
    if (argv.empty()) {
        LOGW("[TASX] Respawn of PID %lu skipped - unreadable command line", exitedPid);
        g_cmdlines.erase(it);
        return 0;
    }
    std::wstring flat = BuildCmdLine(argv);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, flat.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        if (LogRateLimit("respawn-fail", 60))
            LOGW("[TASX] Respawn of PID %lu failed (err %lu)",
                 exitedPid, (unsigned long)GetLastError());
        g_cmdlines.erase(it);
        g_perPidTimes.erase(exitedPid);
        return 0;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (bucket == g_perPidTimes.end())
        bucket = g_perPidTimes.emplace(exitedPid, std::vector<ULONGLONG>()).first;
    bucket->second.push_back(now);
    g_globalTimes.push_back(now);
    g_cmdlines.erase(it); /* original pid is gone - keep the bucket for stats */

    LOGI("[TASX] Respawned client PID %lu (was %lu) | cmd: %ls",
         (unsigned long)pi.dwProcessId, exitedPid, flat.c_str());
    return 1;
}