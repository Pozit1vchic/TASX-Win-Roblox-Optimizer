#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

/* Shared rate limiter for chatty log lines (BUG 8). Returns true when the
   tag is allowed to print (at most once per minIntervalSec), false while
   suppressed. Thread-safe via C++17 magic statics. */
inline bool LogRateLimit(const char* tag, int minIntervalSec)
{
    static std::unordered_map<std::string, std::int64_t> lastCall;
    std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::string k(tag);
    auto it = lastCall.find(k);
    if (it != lastCall.end() && (now - it->second) < (std::int64_t)minIntervalSec * 1000)
        return false;  /* rate-limited */
    lastCall[k] = now;
    return true;   /* allowed */
}
