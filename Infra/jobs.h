#pragma once

#include <windows.h>

/* Job model: ONE shared background cgroup (CPU/MEM caps for the farm)
   + per-process focus profiles (priority/affinity/EcoQoS via CPU.cc).
   A process is assigned to the background job exactly ONCE at hook time;
   it is NEVER moved between sibling jobs (Windows forbids that — always
   ERROR_ACCESS_DENIED). JobApplyProfile is therefore idempotent and
   syscall-free: it returns true only if already correct, false to let the
   caller apply the per-process profile. PID state: 0 in cgroup, -1 sticky
   per-process fallback (foreign job / denied, never retried). */

bool JobsInit();
void JobShutdown();

/* Assign process to the background job (initial state). Returns false if
   assignment failed (caller falls back to per-process profile). */
bool JobHookProcess(DWORD pid, HANDLE hProc);

/* Move process between g_jobFocus / g_jobBackground. Returns true if the
   process is now in the requested job (or was rewritten on assign failure). */
bool JobApplyProfile(DWORD pid, int focused);

/* Dynamic P/E migration: rewrites affinities of both global jobs when
   policy flips (hybrid vs all-cores farm). Kept for master.cpp compatibility. */
void JobsRefreshDynamic(int anyFocused);

void JobRelease(DWORD pid);

/* Implemented in master.cpp */
void TasxNotifyJobEvent(int kind, DWORD pid);
