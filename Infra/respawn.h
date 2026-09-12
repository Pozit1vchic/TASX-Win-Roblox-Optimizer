#pragma once

#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>

/* Farm auto-respawn (injection-free).

   When a farm client crashes, TASX relaunches it with the SAME command
   line it was started with (captured via NtQueryInformationProcess at hook
   time). Fully event-driven: the trigger is the existing RobloxExited
   event on the single master loop - no extra threads, no polling.

   Loop protection (both sliding 1-hour windows):
   - per-origin-PID: RespawnPerHourMax (a single bad client can't eat the farm)
   - global:         RespawnGlobalHourCap (hard breaker for hung-client loops,
     where every respawn gets a fresh PID and the per-PID bucket would reset)

   All functions are main-thread only (no locks) - the master loop is the
   single consumer, exactly like the trimmer/jobs state. */

std::vector<std::wstring> SplitCmdLine(const std::wstring& cmdline);
std::wstring BuildCmdLine(const std::vector<std::wstring>& argv);

/* Remember a client's launch command line at hook time. */
void RespawnRemember(DWORD pid, const std::wstring& cmdline);

/* Drop all knowledge of a client (clean shutdown, manual kill that must not
   be followed by a relaunch). */
void RespawnForget(DWORD pid);
void RespawnClearAll(void);

/* 1 when [TASX] RespawnOnCrash is enabled in the config and at least one
   client is remembered. */
int RespawnConfigured(void);

/* Top-level entry called from the master loop after a client exits. Relaunches
   the dead client when enabled and within the hourly caps. Returns 1 when a
   replacement process was started. */
int RespawnTrySpawn(DWORD exitedPid);

/* Number of remembered clients (for the --status one-shot summary). */
size_t RespawnPendingCount(void);