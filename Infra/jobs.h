#pragma once

#include <windows.h>

/* Two-job model: one focus job (no cap) and one background job (capped).
   Each Roblox process is assigned to exactly one of them and moved on
   focus change via AssignProcessToJobObject — one syscall instead of
   per-thread NtSetInformationProcess. */

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
