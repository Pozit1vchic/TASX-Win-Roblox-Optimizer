#pragma once

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Timer resolution ------------------------------------------------- */

/* Request/release a finer system clock. unitsOf100ns = 5000 requests the
   0.5 ms tick used for smooth high-FPS frame pacing. Returns 1 on success
   and stores the granted resolution in *actualOut (may be coarser). If
   NtSetTimerResolution returns STATUS_PRIVILEGE_NOT_HELD the call logs
   and returns 0 without crashing. */
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

/* --- Kernel memory lists (require elevation) --------------------------- */

/* Purge the standby list ("RAM cleaner" page cache) — frees cached pages
   system-wide. Requires SeProfileSingleProcessPrivilege (admin).
   Returns 0 if no admin — caller should continue without it. */
int tasx_purge_standby_list(void);

/* Empty all process working sets system-wide. Also privileged. */
int tasx_empty_working_sets_system(void);

/* --- Job Objects (2-job model: focus / background) -------------------- */

/* Create a Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.
   Returns NULL on failure. */
HANDLE tasx_job_create(void);

/* Assign process to job. Returns 1 on success. */
int tasx_job_assign(HANDLE hJob, HANDLE hProcess);

/* Cap CPU rate for the whole job: percent 1..100 (e.g. 20 = 20%).
   Uses JobObjectCpuRateControlInformation + HARD_CAP. */
int tasx_job_set_cpu_rate(HANDLE hJob, unsigned long percent);

/* Group I/O priority for the job: NOT portable, always returns 0
   ("not applied, use per-process"). Caller MUST call per-process
   tasx_set_io_priority on each assigned process instead. */
int tasx_job_set_io_priority(HANDLE hJob, unsigned long level);

/* --- P/E core topology ------------------------------------------------ */

/* Returns affinity masks for P-cores and E-cores via
   GetLogicalProcessorInformationEx(RelationProcessorCore). 0 if unavailable.
   Group 0 only (sufficient for client scheduling). */
unsigned long long tasx_get_pcore_mask(void);
unsigned long long tasx_get_ecore_mask(void);
unsigned long long tasx_get_all_mask(void);

/* --- Commit charge / OOM guard --------------------------------------- */

/* Query system commit charge via NtQuerySystemInformation(SystemPerformanceInformation).
   On success stores *committed and *limit in pages (caller converts via page size if needed)
   actually in bytes: committed*pageSize vs limit*pageSize gives bytes.
   Returns 1 on success. Also works via GlobalMemoryStatusEx fallback. */
int tasx_get_commit_info(uint64_t* committed, uint64_t* limit);
int tasx_get_commit_percent(void); /* 0..100, -1 on failure */

/* --- ETW suppression -------------------------------------------------- */

/* Disable an ETW provider by name for background jobs.
   Provider names: L"Microsoft-Windows-Diagnostics-Performance",
                   L"Microsoft-Windows-Kernel-Processor-Power"  etc.
   Uses ControlTraceW + EnableTraceEx2 with EVENT_CONTROL_CODE_DISABLE_PROVIDER.
   Requires admin; returns 0 silently if not elevated. */
int tasx_etw_disable_provider(const wchar_t* providerName);

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
