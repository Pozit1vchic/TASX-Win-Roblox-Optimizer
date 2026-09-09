#pragma once

/* Unified logging: every module logs through these macros (printf-style).
   Output goes to stdout AND to [Log] LogFile when configured (1 MB
   rotation, single .old backup). Level filtering via [Log] LogLevel:
   info shows all, warn shows warnings+errors, error shows errors only. */

#include "ntsys.h"

#include <windows.h>
#include <cstring>

#define LOGI(...) tasx_log(TASX_LOG_INFO, __VA_ARGS__)
#define LOGW(...) tasx_log(TASX_LOG_WARN, __VA_ARGS__)
#define LOGE(...) tasx_log(TASX_LOG_ERROR, __VA_ARGS__)

/* GetProcAddress without -Wcast-function-type noise: FARPROC -> void*,
   then memcpy into the typed pointer at the call site. */
inline void* TasxProcAddress(HMODULE mod, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(mod, name));
}

template <typename Fn>
inline Fn TasxProcFn(HMODULE mod, const char* name)
{
    void* p = TasxProcAddress(mod, name);
    Fn fn = nullptr;
    if (p)
        std::memcpy(&fn, &p, sizeof(fn));
    return fn;
}
