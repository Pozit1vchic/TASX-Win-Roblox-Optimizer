#include "WMI.h"
#include "CPU.h"
#include "trimmer.h"
#include "winhook.h"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <atomic>

std::unordered_map<DWORD, std::thread> g_trimmerThreads;
std::unordered_map<DWORD, std::atomic<bool>> g_trimmerStates;
std::unordered_map<DWORD, HANDLE> g_rbxHandles;
std::mutex g_mutex;

std::optional<DWORD> g_lastFocusedRoblox;
WinHook* g_hook = nullptr;

std::vector<DWORD> Scope()
{
    std::vector<DWORD> pids;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(hSnap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"RobloxPlayerBeta.exe") == 0) {
                pids.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(hSnap, &entry));
    }

    CloseHandle(hSnap);
    return pids;
}

void MonNew()
{
    auto pids = Scope();
    std::lock_guard<std::mutex> lock(g_mutex);

    for (DWORD pid : pids)
    {
        if (g_rbxHandles.find(pid) != g_rbxHandles.end()) continue;

        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA | PROCESS_SET_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
        if (!hProc) {
            std::cerr << "[TASX] Failed to open PID " << pid << std::endl;
            continue;
        }

        g_rbxHandles[pid] = hProc;
        std::cout << "[TASX] New Roblox instance PID " << pid << " hooked" << std::endl;

        std::thread([] {
            Sleep(2000);
            HANDLE hSnapCrash = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (hSnapCrash != INVALID_HANDLE_VALUE)
            {
                PROCESSENTRY32W crash{};
                crash.dwSize = sizeof(crash);

                if (Process32FirstW(hSnapCrash, &crash)) {
                    do {
                        if (_wcsicmp(crash.szExeFile, L"RobloxCrashHandler.exe") == 0) {
                            HANDLE hCrashProc = OpenProcess(PROCESS_TERMINATE, FALSE, crash.th32ProcessID);
                            if (hCrashProc) {
                                TerminateProcess(hCrashProc, 0);
                                CloseHandle(hCrashProc);
                                std::cout << "[TASX] Killed CrashHandler PID " << crash.th32ProcessID << std::endl;
                            }
                        }
                    } while (Process32NextW(hSnapCrash, &crash));
                }
                CloseHandle(hSnapCrash);
            }
            }).detach();

        g_trimmerStates[pid] = true;
        g_trimmerThreads[pid] = std::thread([pid, hProc]() {

            while (g_trimmerStates[pid])
            {
                SetProcessWorkingSetSize(hProc, -1, -1);
                EmptyWorkingSet(hProc);

                PROCESS_MEMORY_COUNTERS_EX mem{};
                if (GetProcessMemoryInfo(hProc, (PROCESS_MEMORY_COUNTERS*)&mem, sizeof(mem))) {
                    std::wcout << L"[TASX] PID " << pid << L" WS: " << (mem.WorkingSetSize / 1024)
                        << L" KB | Private: " << (mem.PrivateUsage / 1024) << L" KB" << std::endl;
                }

                std::this_thread::sleep_for(std::chrono::seconds(15));
            }

            std::wcout << L"[TASX] PID " << pid << L" trim loop exited." << std::endl;
            });

        std::thread([pid, hProc]() {
            if (TasxSetLowestPriorClass(hProc))
                std::cout << "[TASX] LowestPriorityClass applied to PID " << pid << std::endl;
            else
                std::cerr << "[TASX] LowestPriorityClass application failed for PID " << pid << std::endl;
            }).detach();
    }
}

void CleanupExited()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto it = g_rbxHandles.begin(); it != g_rbxHandles.end(); )
    {
        DWORD code = 0;
        if (GetExitCodeProcess(it->second, &code) && code != STILL_ACTIVE)
        {
            DWORD pid = it->first;
            std::cout << "[TASX] Roblox PID " << pid << " exited." << std::endl;

            if (g_trimmerStates.find(pid) != g_trimmerStates.end()) {
                g_trimmerStates[pid] = false;
                if (g_trimmerThreads[pid].joinable())
                    g_trimmerThreads[pid].join();

                g_trimmerThreads.erase(pid);
                g_trimmerStates.erase(pid);
            }

            if (g_lastFocusedRoblox.has_value() && g_lastFocusedRoblox.value() == pid)
                g_lastFocusedRoblox.reset();

            CloseHandle(it->second);
            it = g_rbxHandles.erase(it);
        }
        else {
            ++it;
        }
    }
}

void StartForegroundMonitor()
{
    g_hook = new WinHook();

    std::thread([] {
        while (true) {
            HWND hwnd = GetForegroundWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);

            {
                std::lock_guard<std::mutex> lock(g_mutex);

                if (g_rbxHandles.find(pid) != g_rbxHandles.end()) {
                    if (!g_lastFocusedRoblox.has_value() || g_lastFocusedRoblox.value() != pid) {
                        if (g_lastFocusedRoblox.has_value()) {
                            DWORD old = g_lastFocusedRoblox.value();
                            if (g_rbxHandles.find(old) != g_rbxHandles.end()) {
                                std::wcout << L"[TASX] Roblox PID " << old << L" lost focus" << std::endl;
                                TasxSetLowestPriorClass(g_rbxHandles[old]);
                            }
                        }

                        std::wcout << L"[TASX] Roblox PID " << pid << L" in focus" << std::endl;
                        TasxSetHighestPriorClass(g_rbxHandles[pid]);
                        g_lastFocusedRoblox = pid;
                    }
                }
                else {
                    if (g_lastFocusedRoblox.has_value()) {
                        DWORD old = g_lastFocusedRoblox.value();
                        if (g_rbxHandles.find(old) != g_rbxHandles.end()) {
                            std::wcout << L"[TASX] Roblox PID " << old << L" lost focus" << std::endl;
                            TasxSetLowestPriorClass(g_rbxHandles[old]);
                        }
                        g_lastFocusedRoblox.reset();
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        }).detach();
}

int main()
{
    if (!_wmimon()) {
        std::cerr << "Failed to initialize WMI monitor." << std::endl;
        return -1;
    }

    StartForegroundMonitor();

    std::cout << "[TASX] Launched" << std::endl;

    while (true)
    {
        std::string signal = WaitForRBXEvent();
        if (signal == "RBX_ON")
            MonNew();
        else if (signal == "RBX_OFF")
            CleanupExited();
    }

    _wmishutdown();
    return 0;
}

int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow
)
{
    if (!_wmimon()) {
        MessageBoxW(nullptr, L"Failed to initialize WMI monitor.", L"TASX", MB_ICONERROR);
        return -1;
    }

    StartForegroundMonitor();

    while (true)
    {
        std::string signal = WaitForRBXEvent();
        if (signal == "RBX_ON")
            MonNew();
        else if (signal == "RBX_OFF")
            CleanupExited();
    }

    _wmishutdown();
    return 0;
}