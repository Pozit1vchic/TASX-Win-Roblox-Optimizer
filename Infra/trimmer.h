#pragma once

#include <windows.h>

/* Memory trim scheduler. One Win32 thread serves every client via a
   min-heap of deadlines (replaces the old thread-per-instance model):
   - the focused instance is never trimmed;
   - instances below TrimSkipBelowMB working set are skipped (0 syscalls, cached);
   - the interval adapts to system memory pressure (AdaptiveTrim);
   - LowMemoryResourceNotification is an additional wake source for aggressive trim.
   System-wide cleaning is event-driven elsewhere; this module only does
   targeted per-client trims. No std::thread / condition_variable. */

bool StartTrimmer(DWORD pid, HANDLE processHandle);
void StopTrimmer(DWORD pid);
void StopAllTrimmers();

/* PID of the currently focused Roblox instance (0 = none focused). */
void TrimmerSetFocused(DWORD pid);

/* Immediate trim pass over all registered background clients
   (used by the low-memory reactor). */
void TrimmerTrimAll(void);

/* Aggressive hard trim + standby purge when commit >90% or LowMem fires */
void TrimmerTrimAllAggressive(void);

/* Update cached WorkingSet for Skip check — called from master, zero syscalls in trimmer */
void TrimmerUpdateWorkingSet(DWORD pid, SIZE_T workingSetBytes);
void TrimmerRemoveWorkingSet(DWORD pid);
