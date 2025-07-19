#include "CPU.h"

#include <iostream>
#include <vector>
#include <psapi.h>
#include <tlhelp32.h>
#include <powerbase.h>

static int g_osMajorVersion = 0;

#include <winternl.h>

typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

static void InitOSVersion()
{
    if (g_osMajorVersion != 0) return;

    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtDll) return;

    RtlGetVersionPtr fn = (RtlGetVersionPtr)GetProcAddress(hNtDll, "RtlGetVersion");
    if (!fn) return;

    RTL_OSVERSIONINFOW rovi = { 0 };
    rovi.dwOSVersionInfoSize = sizeof(rovi);

    if (fn(&rovi) == 0)
    {
        if (rovi.dwMajorVersion == 10 && rovi.dwMinorVersion == 0)
        {
            g_osMajorVersion = (rovi.dwBuildNumber >= 22000) ? 11 : 10;
        }
        std::wcout << L"[TASX] Detected Windows Version " << g_osMajorVersion << std::endl;
    }
}

static DWORD LogicalCores()
{
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return 0;

    std::vector<BYTE> buffer(len);
    auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());

    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len))
        return 0;

    DWORD count = 0;
    BYTE* ptr = buffer.data();
    while (ptr < buffer.data() + len)
    {
        auto* core = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
        count += static_cast<DWORD>(__popcnt64(core->Processor.GroupMask[0].Mask));
        ptr += core->Size;
    }

    return count;
}

static DWORD_PTR GetCoreMask(int useCores)
{
    DWORD_PTR mask = 0;
    for (int i = 0; i < useCores; ++i)
        mask |= (1ull << i);
    return mask;
}

static void DisableCPUBoost()
{
    auto hPowrProf = LoadLibraryW(L"PowrProf.dll");
    if (!hPowrProf) {
        std::wcerr << L"[TASX] Could not load PowrProf.dll" << std::endl;
        return;
    }

    using PowerSetInformationFn = NTSTATUS(WINAPI*)(HANDLE, int, PVOID, ULONG);
    auto fn = reinterpret_cast<PowerSetInformationFn>(
        GetProcAddress(hPowrProf, "PowerSetInformation")
        );

    if (fn) {
        DWORD boost = 0;
        if (fn(nullptr, 35 /*ProcessorPerformanceBoostMode*/, &boost, sizeof(boost)) == 0)
            std::wcout << L"[TASX] Boost disabled (ProcessorPerformanceBoostMode = FALSE)" << std::endl;
        else
            std::wcerr << L"[TASX] Failed to disable boost (ProcessorPerformanceBoostMode = TRUE)" << std::endl;
    }

    FreeLibrary(hPowrProf);
}

static void EnableEfficiencyMode(HANDLE hProcess)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);

    if (Thread32First(hSnap, &te)) {
        do {
            if (te.th32OwnerProcessID == GetProcessId(hProcess)) {
                HANDLE hThread = OpenThread(THREAD_SET_INFORMATION, FALSE, te.th32ThreadID);
                if (hThread) {
                    DWORD mode = 1;
                    SetThreadInformation(hThread, ThreadPowerThrottling, &mode, sizeof(mode));
                    CloseHandle(hThread);
                }
            }
        } while (Thread32Next(hSnap, &te));
    }

    CloseHandle(hSnap);
    std::wcout << L"[TASX] Efficiency mode applied (ThreadPowerThrottling = TRUE)" << std::endl;
}

static void DisableEfficiencyMode(HANDLE hProcess)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);

    if (Thread32First(hSnap, &te)) {
        do {
            if (te.th32OwnerProcessID == GetProcessId(hProcess)) {
                HANDLE hThread = OpenThread(THREAD_SET_INFORMATION, FALSE, te.th32ThreadID);
                if (hThread) {
                    DWORD mode = 0;
                    SetThreadInformation(hThread, ThreadPowerThrottling, &mode, sizeof(mode));
                    CloseHandle(hThread);
                }
            }
        } while (Thread32Next(hSnap, &te));
    }

    CloseHandle(hSnap);
    std::wcout << L"[TASX] Efficiency mode disabled (ThreadPowerThrottling = FALSE)" << std::endl;
}

bool TasxSetLowestPriorClass(HANDLE hProcess)
{
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) return false;

    InitOSVersion();

    DWORD logicalCores = LogicalCores();
    if (logicalCores < 2) {
        std::wcerr << L"[TASX] Not enough cores" << std::endl;
        return false;
    }

    DWORD useCores = max(2, logicalCores / 2);
    DWORD_PTR mask = GetCoreMask(useCores);

    if (!SetProcessAffinityMask(hProcess, mask))
        std::wcerr << L"[TASX] Affinity set failed | Code: " << GetLastError() << std::endl;
    else
        std::wcout << L"[TASX] Affinity limited to " << useCores << L" cores" << std::endl;

    if (!SetPriorityClass(hProcess, IDLE_PRIORITY_CLASS))
        std::wcerr << L"[TASX Failed to set IDLE_PRIORITY_CLASS" << std::endl;
    else
        std::wcout << L"[TASX] Priority set to IDLE_PRIORITY_CLASS" << std::endl;

    if (g_osMajorVersion == 10)
        DisableCPUBoost();

    if (g_osMajorVersion == 11)
        EnableEfficiencyMode(hProcess);

    return true;
}

bool TasxSetHighestPriorClass(HANDLE hProcess)
{
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) return false;
    InitOSVersion();

    DWORD_PTR processMask = 0, systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
        std::wcerr << L"[TASX] Could not query affinity mask | Code: " << GetLastError() << std::endl;
        return false;
    }

    if (!SetProcessAffinityMask(hProcess, systemMask)) {
        std::wcerr << L"[TASX] Failed to set full affinity | Code: " << GetLastError() << std::endl;
    }
    else {
        std::wcout << L"[TASX] Affinity set to all cores" << std::endl;
    }

    if (!SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS)) {
        std::wcerr << L"[TASX] Failed to set HIGH_PRIORITY_CLASS | Code: " << GetLastError() << std::endl;
    }
    else {
        std::wcout << L"[TASX] Priority set to HIGH_PRIORITY_CLASS" << std::endl;
    }

    if (g_osMajorVersion == 11) DisableEfficiencyMode(hProcess);

    return true;
}