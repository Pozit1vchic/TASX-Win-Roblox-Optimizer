#pragma once
#include <Windows.h>
#include <string>
#include <atomic>

class WinHook {
public:
    WinHook();
    ~WinHook();

    bool isRobloxInFocus() const;

private:
    static void CALLBACK HookCallback(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
        LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);

    static std::atomic_bool robloxInFocus;
    HWINEVENTHOOK hookHandle;
};