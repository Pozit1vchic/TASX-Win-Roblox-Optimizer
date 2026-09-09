/* TASX low-level NT / kernel32 primitives. Everything is resolved dynamically
   so the binary loads on any Windows version and stays header-agnostic.
   Plain C on purpose (user-requested C core), shared by the C++ modules. */
#include "ntsys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward: sync logger defined at the bottom of this file
   (full prototype lives in ntsys.h). */
void tasx_log(int level, const char* fmt, ...);

typedef long NTSTATUS;

#define TASX_NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#define TASX_STATUS_PRIVILEGE_NOT_HELD ((NTSTATUS)0xC0000061L)

/* Undocumented-but-stable ntdll surface */
typedef NTSTATUS (NTAPI *NtSetTimerResolutionFn)(unsigned long desired,
                                                 unsigned char set,
                                                 unsigned long* current);
typedef NTSTATUS (NTAPI *NtSetSystemInformationFn)(int infoClass, void* buffer,
                                                   unsigned long size);
typedef NTSTATUS (NTAPI *NtSetInformationProcessFn)(HANDLE handle, int cls,
                                                    void* buffer,
                                                    unsigned long size);
typedef NTSTATUS (NTAPI *NtQuerySystemInformationFn)(int infoClass,
                                                     void* buffer,
                                                     unsigned long size,
                                                     unsigned long* needed);

/* kernel32 dynamic surface (avoids old-SDK header gaps) */
typedef int (WINAPI *SetProcessInformationFn)(HANDLE, int, void*, unsigned long);
typedef int (WINAPI *SetThreadInformationFn)(HANDLE, int, void*, unsigned long);

/* PROCESS_INFORMATION_CLASS values we rely on */
#define TASX_ProcessIoPriority     33
#define TASX_ProcessMemoryPriority 0
#define TASX_ProcessPowerThrottling 4
#define TASX_ThreadPowerThrottling  1

typedef struct {
    unsigned long Version;
    unsigned long ControlMask;
    unsigned long StateMask;
} TasxPowerThrottlingState;

#define TASX_POWER_THROTTLING_CURRENT_VERSION 1UL
#define TASX_POWER_THROTTLING_EXECUTION_SPEED 0x1UL

/* MEMORY_PRIORITY values */
#define TASX_MEMORY_PRIORITY_VERY_LOW 1UL

/* SYSTEM_INFORMATION_CLASS SystemMemoryListInformation = 80 */
#define TASX_SystemMemoryListInformation 80
/* SYSTEM_MEMORY_LIST_COMMAND values (phnt); older builds differ, so both
   spellings of "empty working sets" are attempted. */
#define TASX_MemCmd_PurgeLowPriorityStandby 3
#define TASX_MemCmd_PurgeStandby            4
#define TASX_MemCmd_EmptyWorkingSetsA       7
#define TASX_MemCmd_EmptyWorkingSetsB       3

/* SYSTEM_INFORMATION_CLASS for commit charge */
#define TASX_SystemPerformanceInformation 2

typedef struct {
    LARGE_INTEGER IdleProcessTime;
    LARGE_INTEGER IoReadTransferCount;
    LARGE_INTEGER IoWriteTransferCount;
    LARGE_INTEGER IoOtherTransferCount;
    ULONG IoReadOperationCount;
    ULONG IoWriteOperationCount;
    ULONG IoOtherOperationCount;
    ULONG AvailablePages;
    ULONG CommittedPages;
    ULONG CommitLimit;
    ULONG PeakCommitment;
    ULONG PageFaultCount;
    ULONG CopyOnWriteCount;
    ULONG TransitionCount;
    ULONG CacheTransitionCount;
    ULONG DemandZeroCount;
    ULONG PageReadCount;
    ULONG PageReadIoCount;
    ULONG CacheReadCount;
    ULONG CacheIoCount;
    ULONG DirtyPagesWriteCount;
    ULONG DirtyWriteIoCount;
    ULONG MappedPagesWriteCount;
    ULONG MappedWriteIoCount;
    ULONG PagedPoolPages;
    ULONG NonPagedPoolPages;
    ULONG PagedPoolAllocs;
    ULONG PagedPoolFrees;
    ULONG NonPagedPoolAllocs;
    ULONG NonPagedPoolFrees;
    ULONG FreeSystemPtes;
    ULONG ResidentSystemCodePage;
    ULONG TotalSystemDriverPages;
    ULONG TotalSystemCodePages;
    ULONG NonPagedPoolLookasideHits;
    ULONG PagedPoolLookasideHits;
    ULONG AvailablePagedPoolPages;
    ULONG ResidentSystemCachePage;
    ULONG ResidentPagedPoolPage;
    ULONG ResidentSystemDriverPage;
    ULONG CcFastReadNoWait;
    ULONG CcFastReadWait;
    ULONG CcFastReadResourceMiss;
    ULONG CcFastReadNotPossible;
    ULONG CcFastMdlReadNoWait;
    ULONG CcFastMdlReadWait;
    ULONG CcFastMdlReadResourceMiss;
    ULONG CcFastMdlReadNotPossible;
    ULONG CcMapDataNoWait;
    ULONG CcMapDataWait;
    ULONG CcMapDataNoWaitMiss;
    ULONG CcMapDataWaitMiss;
    ULONG CcPinMappedDataCount;
    ULONG CcPinReadNoWait;
    ULONG CcPinReadWait;
    ULONG CcPinReadNoWaitMiss;
    ULONG CcPinReadWaitMiss;
    ULONG CcCopyReadNoWait;
    ULONG CcCopyReadWait;
    ULONG CcCopyReadNoWaitMiss;
    ULONG CcCopyReadWaitMiss;
    ULONG CcMdlReadNoWait;
    ULONG CcMdlReadWait;
    ULONG CcMdlReadNoWaitMiss;
    ULONG CcMdlReadWaitMiss;
    ULONG CcReadAheadIoCount;
    ULONG CcLazyWriteIoCount;
    ULONG CcLazyWritePages;
    ULONG CcDataFlushes;
    ULONG CcDataPages;
    ULONG ContextSwitches;
    ULONG FirstLevelTbFills;
    ULONG SecondLevelTbFills;
    ULONG SystemCalls;
    ULONGLONG CcTotalDirtyPages;
    ULONGLONG CcDirtyPageThreshold;
    LONGLONG ResidentAvailablePages;
    ULONGLONG SharedCommittedPages;
} TASX_SYSTEM_PERFORMANCE_INFORMATION;

static void* nt_fn(const char* name)
{
    HMODULE h = GetModuleHandleW(L"ntdll.dll");
    if (!h) return NULL;
    return (void*)GetProcAddress(h, name);
}

static void* k32_fn(const char* name)
{
    HMODULE h = GetModuleHandleW(L"kernel32.dll");
    if (!h) return NULL;
    return (void*)GetProcAddress(h, name);
}

int tasx_set_timer_resolution(unsigned long unitsOf100ns, int enable,
                              unsigned long* actualOut)
{
    static NtSetTimerResolutionFn fn = NULL;
    static int resolved = 0;
    unsigned long current = 0;
    NTSTATUS st;

    if (!resolved) {
        fn = (NtSetTimerResolutionFn)nt_fn("NtSetTimerResolution");
        resolved = 1;
    }
    if (!fn) return 0;

    st = fn(unitsOf100ns, (unsigned char)(enable ? 1 : 0), &current);
    if (st == TASX_STATUS_PRIVILEGE_NOT_HELD) {
        /* Admin-less fallback: log once and continue without failure. */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            tasx_log(TASX_LOG_WARN,
                "[TASX] NtSetTimerResolution privilege not held, timer resolution unchanged");
        }
        return 0;
    }
    if (!TASX_NT_SUCCESS(st))
        return 0;

    if (actualOut) *actualOut = current;
    return 1;
}

int tasx_set_io_priority(HANDLE hProcess, unsigned long level)
{
    static NtSetInformationProcessFn fn = NULL;
    static int resolved = 0;
    unsigned long value = level;

    if (!resolved) {
        fn = (NtSetInformationProcessFn)nt_fn("NtSetInformationProcess");
        resolved = 1;
    }
    if (!fn || !hProcess || hProcess == INVALID_HANDLE_VALUE) return 0;

    return TASX_NT_SUCCESS(fn(hProcess, TASX_ProcessIoPriority, &value,
                              sizeof(value)));
}

int tasx_set_memory_priority(HANDLE hProcess, unsigned long priority)
{
    static SetProcessInformationFn fn = NULL;
    static int resolved = 0;

    if (!resolved) {
        fn = (SetProcessInformationFn)k32_fn("SetProcessInformation");
        resolved = 1;
    }
    if (!fn || !hProcess || hProcess == INVALID_HANDLE_VALUE) return 0;

    return fn(hProcess, TASX_ProcessMemoryPriority, &priority,
              sizeof(priority));
}

int tasx_process_power_throttling(HANDLE hProcess, int enable)
{
    static SetProcessInformationFn fn = NULL;
    static int resolved = 0;
    TasxPowerThrottlingState st;

    if (!resolved) {
        fn = (SetProcessInformationFn)k32_fn("SetProcessInformation");
        resolved = 1;
    }
    if (!fn || !hProcess || hProcess == INVALID_HANDLE_VALUE) return 0;

    st.Version = TASX_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = TASX_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = enable ? TASX_POWER_THROTTLING_EXECUTION_SPEED : 0;

    return fn(hProcess, TASX_ProcessPowerThrottling, &st, sizeof(st));
}

int tasx_thread_power_throttling(HANDLE hThread, int enable)
{
    static SetThreadInformationFn fn = NULL;
    static int resolved = 0;
    TasxPowerThrottlingState st;

    if (!resolved) {
        fn = (SetThreadInformationFn)k32_fn("SetThreadInformation");
        resolved = 1;
    }
    if (!fn || !hThread || hThread == INVALID_HANDLE_VALUE) return 0;

    st.Version = TASX_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = TASX_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = enable ? TASX_POWER_THROTTLING_EXECUTION_SPEED : 0;

    return fn(hThread, TASX_ThreadPowerThrottling, &st, sizeof(st));
}

static int enable_privilege(const wchar_t* name)
{
    HANDLE token = NULL;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    int ok = 0;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return 0;

    if (LookupPrivilegeValueW(NULL, name, &luid)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(0); /* AdjustTokenPrivileges does not clear it on success */
        if (AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL)) {
            ok = (GetLastError() != ERROR_NOT_ALL_ASSIGNED);
        }
    }

    CloseHandle(token);
    return ok;
}

static int memory_list_command(unsigned long command)
{
    static NtSetSystemInformationFn fn = NULL;
    static int resolved = 0;

    if (!resolved) {
        fn = (NtSetSystemInformationFn)nt_fn("NtSetSystemInformation");
        resolved = 1;
    }
    if (!fn) return 0;
    if (!enable_privilege(L"SeProfileSingleProcessPrivilege")) return 0;

    return TASX_NT_SUCCESS(fn((int)TASX_SystemMemoryListInformation,
                              &command, sizeof(command)));
}

int tasx_purge_standby_list(void)
{
    if (memory_list_command(TASX_MemCmd_PurgeStandby)) return 1;
    return memory_list_command(TASX_MemCmd_PurgeLowPriorityStandby);
}

int tasx_empty_working_sets_system(void)
{
    if (memory_list_command(TASX_MemCmd_EmptyWorkingSetsA)) return 1;
    if (memory_list_command(TASX_MemCmd_EmptyWorkingSetsB)) return 1;
    return 0;
}

int tasx_is_elevated(void)
{
    HANDLE tok = NULL;
    TOKEN_ELEVATION el;
    DWORD ret = 0;
    int elevated = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return 0;
    memset(&el, 0, sizeof(el));
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &ret))
        elevated = (el.TokenIsElevated != 0);
    CloseHandle(tok);
    return elevated;
}

/* --- Sync file + stdout logging -------------------------------------- */

#include <stdarg.h>

#define TASX_LOG_MAX_LINE 4096
#define TASX_LOG_ROTATE_BYTES (1024u * 1024u)

static CRITICAL_SECTION g_logCs;
static int g_logReady = 0;
static FILE* g_logFile = NULL;
static char g_logPath[MAX_PATH] = { 0 };
static int g_logLevel = TASX_LOG_INFO;
static unsigned long long g_logBytes = 0;

void tasx_log_init(void)
{
    if (g_logReady) return;
    InitializeCriticalSection(&g_logCs);
    g_logReady = 1;
}

void tasx_log_configure(const char* logFilePath, int minLevel)
{
    if (!g_logReady) tasx_log_init();
    EnterCriticalSection(&g_logCs);
    g_logLevel = (minLevel < TASX_LOG_INFO || minLevel > TASX_LOG_ERROR)
        ? TASX_LOG_INFO : minLevel;
    if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
    g_logPath[0] = '\0';
    g_logBytes = 0;
    if (logFilePath && logFilePath[0]) {
        snprintf(g_logPath, sizeof(g_logPath), "%s", logFilePath);
        g_logFile = fopen(g_logPath, "a");
        if (g_logFile) {
            fseek(g_logFile, 0, SEEK_END);
            {
                long pos = ftell(g_logFile);
                g_logBytes = (pos > 0) ? (unsigned long long)pos : 0ull;
            }
        }
    }
    LeaveCriticalSection(&g_logCs);
}

int tasx_log_level_from_str(const char* s)
{
    if (!s || !s[0]) return TASX_LOG_INFO;
    switch (s[0] | 0x20) { /* ASCII lowercase */
    case 'w': return TASX_LOG_WARN;
    case 'e': return TASX_LOG_ERROR;
    default:  return TASX_LOG_INFO;
    }
}

static void tasx_log_rotate_locked(void)
{
    char oldPath[MAX_PATH + 8];

    if (!g_logFile || g_logBytes <= TASX_LOG_ROTATE_BYTES) return;
    fclose(g_logFile);
    g_logFile = NULL;
    snprintf(oldPath, sizeof(oldPath), "%s.old", g_logPath);
    DeleteFileA(oldPath);
    MoveFileA(g_logPath, oldPath); /* best effort: continue even if it fails */
    g_logFile = fopen(g_logPath, "w");
    g_logBytes = 0;
}

void tasx_log(int level, const char* fmt, ...)
{
    char line[TASX_LOG_MAX_LINE];
    SYSTEMTIME st;
    int prefix;
    va_list ap;
    size_t len;

    if (!fmt || !fmt[0]) return;
    if (!g_logReady) {
        /* Pre-init fallback: stdout only, no lock (single-threaded startup). */
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        putchar('\n');
        return;
    }

    GetLocalTime(&st);
    prefix = snprintf(line, sizeof(line), "[%02u:%02u:%02u] ",
                      (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    if (prefix < 0) prefix = 0;
    if ((size_t)prefix >= sizeof(line) - 2) prefix = (int)sizeof(line) - 2;

    va_start(ap, fmt);
    vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';

    EnterCriticalSection(&g_logCs);
    if (level >= g_logLevel) {
        fputs(line, stdout);
        putchar('\n');
        fflush(stdout);
        if (g_logFile) {
            len = strlen(line);
            fwrite(line, 1, len, g_logFile);
            fwrite("\n", 1, 1, g_logFile);
            fflush(g_logFile);
            g_logBytes += (unsigned long long)(len + 1);
            tasx_log_rotate_locked();
        }
    }
    LeaveCriticalSection(&g_logCs);
}

void tasx_log_shutdown(void)
{
    if (!g_logReady) return;
    EnterCriticalSection(&g_logCs);
    if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
    g_logPath[0] = '\0';
    g_logBytes = 0;
    LeaveCriticalSection(&g_logCs);
    DeleteCriticalSection(&g_logCs);
    g_logReady = 0;
}

/* --- Job Objects ----------------------------------------------------- */

HANDLE tasx_job_create_ex(int killOnClose)
{
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) return NULL;

    if (killOnClose) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext;
        ZeroMemory(&ext, sizeof(ext));
        ext.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &ext, sizeof(ext))) {
            /* Some systems restrict KILL_ON_CLOSE without admin; fall back
               to a plain job instead of failing. */
            ZeroMemory(&ext, sizeof(ext));
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                         &ext, sizeof(ext))) {
                CloseHandle(job);
                return NULL;
            }
        }
    }
    return job;
}

HANDLE tasx_job_create(void)
{
    return tasx_job_create_ex(1);
}

int tasx_job_assign(HANDLE hJob, HANDLE hProcess)
{
    if (!hJob || hJob == INVALID_HANDLE_VALUE) return 0;
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) return 0;
    return AssignProcessToJobObject(hJob, hProcess) ? 1 : 0;
}

int tasx_job_set_cpu_rate(HANDLE hJob, unsigned long percent)
{
    if (!hJob || hJob == INVALID_HANDLE_VALUE) return 0;
    if (percent == 0 || percent > 100) return 0;

    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION crc;
    ZeroMemory(&crc, sizeof(crc));
    crc.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE
                     | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    crc.CpuRate = percent * 100; /* percent * 100 of total CPU (10000 = 100%) */
    return SetInformationJobObject(hJob,
                                   JobObjectCpuRateControlInformation,
                                   &crc, sizeof(crc)) ? 1 : 0;
}

int tasx_job_set_io_priority(HANDLE hJob, unsigned long level)
{
    /* BUG 10: Job-level IO priority is not portable - the real Job IoRate
       API requires Win8+ and JOBOBJECT_IO_RATE_CONTROL_INFORMATION, which is
       not exposed in public headers. Caller MUST also call per-process
       tasx_set_io_priority on each assigned process. */
    (void)hJob; (void)level;
    return 0;  /* return 0 to signal "not applied, use per-process" */
}

/* --- P/E topology ---------------------------------------------------- */

static int g_topoInited = 0;
static unsigned long long g_pMask = 0;
static unsigned long long g_eMask = 0; /* background mask, see init_topo */
static unsigned long long g_allMask = 0;

static unsigned popcnt64(unsigned long long m)
{
    unsigned c = 0;
    while (m) { m &= m - 1; ++c; }
    return c;
}

static void init_topo(void)
{
    if (g_topoInited) return;
    g_topoInited = 1;

    ULONG len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return;

    unsigned char* buf = (unsigned char*)malloc(len);
    if (!buf) return;

    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        free(buf);
        return;
    }

    unsigned char* ptr = buf;
    unsigned char* end = buf + len;
    int isHybrid = 0;
    unsigned long long pMask = 0, eMask = 0, allMask = 0;

    while (ptr < end) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX core =
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)ptr;
        if (core->Relationship == RelationProcessorCore &&
            core->Processor.GroupMask[0].Mask != 0) {
            unsigned long long mask = (unsigned long long)core->Processor.GroupMask[0].Mask;
            unsigned char eff = core->Processor.EfficiencyClass;
            allMask |= mask;
            if (eff > 0) {
                isHybrid = 1;
                pMask |= mask;
            } else if (core->Processor.GroupMask[0].Group == 0) {
                eMask |= mask;
            }
        }
        ptr += core->Size;
    }
    free(buf);

    if (!isHybrid) pMask = allMask;
    /* eMask semantic = background mask (single source of truth for CPU.cc
       and jobs.cc). Hybrid with >= 2 E-cores: E-cores. Otherwise
       (non-hybrid, or degenerate hybrid with < 2 E-bits): low half of the
       logical CPUs, so background clients never sit on every core. */
    if (!isHybrid || popcnt64(eMask) < 2) {
        unsigned total = popcnt64(allMask);
        unsigned half = total >= 2 ? total / 2 : total;
        unsigned long long low = 0;
        unsigned set = 0;
        for (unsigned bit = 0; bit < 64 && set < half; ++bit) {
            if (allMask & (1ull << bit)) { low |= (1ull << bit); ++set; }
        }
        if (low) eMask = low;
    }

    g_pMask = pMask;
    g_eMask = eMask;
    g_allMask = allMask ? allMask : pMask;
}

unsigned long long tasx_get_pcore_mask(void) { init_topo(); return g_pMask ? g_pMask : g_allMask; }
unsigned long long tasx_get_ecore_mask(void) { init_topo(); return g_eMask; }
unsigned long long tasx_get_all_mask(void)   { init_topo(); return g_allMask; }

/* --- Commit charge --------------------------------------------------- */

int tasx_get_commit_info(uint64_t* committed, uint64_t* limit)
{
    static NtQuerySystemInformationFn fn = NULL;
    static int resolved = 0;

    if (committed) *committed = 0;
    if (limit) *limit = 0;

    if (!resolved) {
        fn = (NtQuerySystemInformationFn)nt_fn("NtQuerySystemInformation");
        resolved = 1;
    }
    if (fn) {
        TASX_SYSTEM_PERFORMANCE_INFORMATION perf;
        ZeroMemory(&perf, sizeof(perf));
        unsigned long needed = 0;
        NTSTATUS s = fn((int)TASX_SystemPerformanceInformation,
                        &perf, sizeof(perf), &needed);
        if (TASX_NT_SUCCESS(s) && perf.CommitLimit != 0) {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            uint64_t page = si.dwPageSize ? si.dwPageSize : 4096;
            if (committed) *committed = (uint64_t)perf.CommittedPages * page;
            if (limit) *limit = (uint64_t)perf.CommitLimit * page;
            return 1;
        }
    }
    /* Fallback via GlobalMemoryStatusEx */
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        if (committed) *committed = ms.ullTotalPageFile - ms.ullAvailPageFile;
        if (limit) *limit = ms.ullTotalPageFile;
        return 1;
    }
    return 0;
}

int tasx_get_commit_percent(void)
{
    uint64_t committed = 0, limit = 0;
    if (!tasx_get_commit_info(&committed, &limit) || limit == 0) return -1;
    return (int)((committed * 100ull) / limit);
}

/* --- ETW ------------------------------------------------------------- */

#ifndef EVENT_CONTROL_CODE_DISABLE_PROVIDER
#define EVENT_CONTROL_CODE_DISABLE_PROVIDER 0
#endif
#ifndef EVENT_CONTROL_CODE_ENABLE_PROVIDER
#define EVENT_CONTROL_CODE_ENABLE_PROVIDER 1
#endif

/* Use generic types to avoid evntrace.h dependency when compiling as C with MinGW */
typedef ULONG (WINAPI *ControlTraceWFn)(ULONG64, const wchar_t*, void*, ULONG);
typedef ULONG (WINAPI *EnableTraceEx2Fn)(ULONG64, const GUID*, ULONG, UCHAR, ULONGLONG, ULONGLONG, ULONG, void*);

int tasx_etw_disable_provider(const wchar_t* providerName)
{
    if (!providerName || !*providerName) return 0;

    /* ETW disable needs admin; quietly fail if not elevated */
    if (!enable_privilege(L"SeSystemProfilePrivilege")) {
        /* try without - some providers allow without */
    }

    HMODULE adv = GetModuleHandleW(L"advapi32.dll");
    if (!adv) adv = LoadLibraryW(L"advapi32.dll");
    if (!adv) return 0;

    ControlTraceWFn pControlTraceW = NULL;
    EnableTraceEx2Fn pEnableTraceEx2 = NULL;
    {
        /* void* round-trip: direct FARPROC->function casts trip -Wextra. */
        void* p1 = (void*)GetProcAddress(adv, "ControlTraceW");
        void* p2 = (void*)GetProcAddress(adv, "EnableTraceEx2");
        if (p1) memcpy(&pControlTraceW, &p1, sizeof(pControlTraceW));
        if (p2) memcpy(&pEnableTraceEx2, &p2, sizeof(pEnableTraceEx2));
    }
    if (!pControlTraceW || !pEnableTraceEx2) return 0;

    /* Known provider GUIDs (verified against Microsoft instrumentation
       manifests and the winevt-kb provider database):
       - Microsoft-Windows-Diagnostics-Performance:
         {CFC18EC0-96B1-4EBA-961B-622CAEE05B0A} (diagperf.dll)
       - Microsoft-Windows-Kernel-Processor-Power:
         {0F67E49F-FE51-4E9F-B490-6F2948CC6027} (PPM_ETW_PROVIDER) */
    GUID guid;
    ZeroMemory(&guid, sizeof(guid));
    int haveGuid = 0;
    if (wcscmp(providerName, L"Microsoft-Windows-Diagnostics-Performance") == 0) {
        static const GUID g_diag = {0xcfc18ec0, 0x96b1, 0x4eba, {0x96,0x1b,0x62,0x2c,0xae,0xe0,0x5b,0x0a}};
        guid = g_diag; haveGuid = 1;
    } else if (wcscmp(providerName, L"Microsoft-Windows-Kernel-Processor-Power") == 0) {
        static const GUID g_pwr = {0x0f67e49f, 0xfe51, 0x4e9f, {0xb4,0x90,0x6f,0x29,0x48,0xcc,0x60,0x27}};
        guid = g_pwr; haveGuid = 1;
    } else {
        /* Try to enable/disable by name via ControlTrace — not supported, fail */
        return 0;
    }

    if (!haveGuid) return 0;

    /* Disable provider on all sessions (session 0 = system) */
    ULONG err = pEnableTraceEx2((ULONG64)0, &guid,
                                EVENT_CONTROL_CODE_DISABLE_PROVIDER,
                                0, 0, 0, 0, NULL);
    /* ERROR_SUCCESS == 0, also allow ERROR_WMI_GUID_NOT_FOUND as success (not active) */
    if (err == 0 || err == 4317 /* ERROR_WMI_GUID_NOT_FOUND */) return 1;
    return 0;
}

TASX_SYS_PROC* tasx_query_system_processes(void)
{
    static NtQuerySystemInformationFn fn = NULL;
    static int resolved = 0;

    if (!resolved) {
        fn = (NtQuerySystemInformationFn)nt_fn("NtQuerySystemInformation");
        resolved = 1;
    }
    if (!fn) return NULL;

    unsigned long size = 1ul << 20;
    for (int attempt = 0; attempt < 4; ++attempt) {
        unsigned char* buf = (unsigned char*)malloc(size);
        if (!buf) return NULL;

        unsigned long needed = 0;
        NTSTATUS s = fn(5 /* SystemProcessInformation */, buf, size, &needed);
        if (TASX_NT_SUCCESS(s))
            return (TASX_SYS_PROC*)buf;

        free(buf);
        size = needed > size ? needed + (1ul << 20) : size + (1ul << 20);
    }
    return NULL;
}
