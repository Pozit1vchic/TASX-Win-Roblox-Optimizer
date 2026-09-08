#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Timer resolution ------------------------------------------------- */

/* Request/release a finer system clock. unitsOf100ns = 5000 requests the
   0.5 ms tick used for smooth high-FPS frame pacing. Returns 1 on success
   and stores the granted resolution in *actualOut (may be coarser). */
int tasx_set_timer_resolution(unsigned long unitsOf100ns, int enable,
                              unsigned long* actualOut);

/* --- Process scheduling primitives ------------------------------------ */

/* I/O priority for a process handle: 0 = VeryLow, 1 = Low, 2 = Normal,
   3 = High. Silently fails on unsupported systems. */
int tasx_set_io_priority(HANDLE hProcess, unsigned long level);

/* Memory priority (page-fetch ranking): 1 = VeryLow .. 5 = Normal. */
int tasx_set_memory_priority(HANDLE hProcess, unsigned long priority);

/* Process-wide EcoQoS / power throttling. enable != 0 -> throttled
   (efficiency mode), enable == 0 -> exempt (performance mode). */
int tasx_process_power_throttling(HANDLE hProcess, int enable);

/* Per-thread power throttling, same semantics as above. */
int tasx_thread_power_throttling(HANDLE hThread, int enable);

/* Applies per-thread throttling to every thread of the process. */
int tasx_process_threads_power_throttling(HANDLE hProcess, int enable);

/* --- Kernel memory lists (require elevation) --------------------------- */

/* Purge the standby list ("RAM cleaner" page cache) — frees cached pages
   system-wide. Requires SeProfileSingleProcessPrivilege (admin). */
int tasx_purge_standby_list(void);

/* Empty all process working sets system-wide. Also privileged. */
int tasx_empty_working_sets_system(void);

/* --- Single-syscall process table ------------------------------------- */

/* Minimal x64 layout of SYSTEM_PROCESS_INFORMATION (SystemProcessInformation
   = 5). Matches the Win7+ layout (WorkingSetPrivateSize/HardFaultCount/
   CycleTime after NumberOfThreads); walk the chain via NextEntryOffset. */
typedef struct {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} TASX_UNICODE_STRING;

typedef struct {
    ULONG  NextEntryOffset;
    ULONG  NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG  HardFaultCount;
    ULONG  NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    TASX_UNICODE_STRING ImageName;
    LONG   BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG  HandleCount;
    ULONG  SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG  PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
} TASX_SYS_PROC;

/* One NtQuerySystemInformation call for ALL processes: replaces per-process
   GetProcessMemoryInfo / CreateToolhelp32Snapshot at 100+ clients.
   Returns a malloc'd entry chain (caller frees with free()); NULL on fail. */
TASX_SYS_PROC* tasx_query_system_processes(void);

/* Per-core time counters (SystemProcessorPerformanceInformation = 8). One
   syscall for every logical CPU; delta two samples to get busy percentages.
   Returns malloc'd array, *outCount = element count; NULL on fail. */
typedef struct {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;   /* excludes idle on this info class */
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
} TASX_SYS_PROC_PERF;

TASX_SYS_PROC_PERF* tasx_query_processor_performance(unsigned long* outCount);

#ifdef __cplusplus
}
#endif
