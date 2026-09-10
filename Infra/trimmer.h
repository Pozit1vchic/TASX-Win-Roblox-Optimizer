#pragma once

#include <windows.h>

/* Memory trim scheduler. One Win32 thread serves every client via a
   min-heap of deadlines:
   - the focused instance is never trimmed;
   - instances below TrimSkipBelowMB working set are skipped (0 syscalls, cached);
   - the interval adapts to system memory pressure (AdaptiveTrim);
   - hard trim applies ONLY to long-inactive background clients
     (unfocused longer than 2x the current interval).
   Low-memory reaction is owned by master.cpp (single event-driven reactor);
   this module only does targeted per-client trims. No std::thread. */

bool StartTrimmer(DWORD pid, HANDLE processHandle);
void StopTrimmer(DWORD pid);
void StopAllTrimmers();

/* PID of the currently focused Roblox instance (0 = none focused). */
void TrimmerSetFocused(DWORD pid);

/* Page-in hotkey: nonzero suspends ALL trimming during page-in pass. */
void TrimmerSetPageInPause(int on);

/* Aggressive pass over background clients (used by the low-memory
   reactor): soft for recently unfocused, hard only for long-inactive,
   skip below threshold, focused never touched. */
void TrimmerTrimAllAggressive(void);

/* Update cached WorkingSet for Skip check — called from master, zero syscalls in trimmer */
void TrimmerUpdateWorkingSet(DWORD pid, SIZE_T workingSetBytes);
void TrimmerRemoveWorkingSet(DWORD pid);
