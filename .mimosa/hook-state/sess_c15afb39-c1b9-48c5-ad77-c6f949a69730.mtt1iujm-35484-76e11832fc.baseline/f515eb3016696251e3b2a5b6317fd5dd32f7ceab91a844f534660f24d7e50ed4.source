/* TASX low-level NT / kernel32 primitives. Everything is resolved dynamically
   so the binary loads on any Windows version and stays header-agnostic.
   Plain C on purpose (user-requested C core), shared by the C++ modules. */
#include "ntsys.h"

#include <stdio.h>
#include <stdlib.h>
#include <tlhelp32.h>

typedef long NTSTATUS;

#define TASX_NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)

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

    if (!resolved) {
        fn = (NtSetTimerResolutionFn)nt_fn("NtSetTimerResolution");
        resolved = 1;
    }
    if (!fn) return 0;

    if (!TASX_NT_SUCCESS(fn(unitsOf100ns, (unsigned char)(enable ? 1 : 0),
                            &current)))
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

int tasx_process_threads_power_throttling(HANDLE hProcess, int enable)
{
    HANDLE snap;
    THREADENTRY32 te;
    DWORD pid;
    int applied = 0;

    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) return 0;
    pid = GetProcessId(hProcess);
    if (!pid) return 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            {
                HANDLE hThread = OpenThread(THREAD_SET_INFORMATION, FALSE,
                                            te.th32ThreadID);
                if (hThread) {
                    if (tasx_thread_power_throttling(hThread, enable))
                        ++applied;
                    CloseHandle(hThread);
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return applied;
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

TASX_SYS_PROC_PERF* tasx_query_processor_performance(unsigned long* outCount)
{
    static NtQuerySystemInformationFn fn = NULL;
    static int resolved = 0;

    if (outCount) *outCount = 0;
    if (!resolved) {
        fn = (NtQuerySystemInformationFn)nt_fn("NtQuerySystemInformation");
        resolved = 1;
    }
    if (!fn) return NULL;

    unsigned long size = 4096;
    for (int attempt = 0; attempt < 4; ++attempt) {
        unsigned char* buf = (unsigned char*)malloc(size);
        if (!buf) return NULL;

        unsigned long needed = 0;
        NTSTATUS s = fn(8 /* SystemProcessorPerformanceInformation */,
                        buf, size, &needed);
        if (TASX_NT_SUCCESS(s)) {
            if (outCount && needed >= sizeof(TASX_SYS_PROC_PERF))
                *outCount = needed / sizeof(TASX_SYS_PROC_PERF);
            return (TASX_SYS_PROC_PERF*)buf;
        }

        free(buf);
        size = needed > size ? needed : size * 2;
    }
    return NULL;
}
