#include "winhook.h"
#include <psapi.h>

std::atomic_bool WinHook::robloxInFocus = false;

WinHook::WinHook() {
    hookHandle = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        nullptr,
        HookCallback,
        0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
}

WinHook::~WinHook() {
    if (hookHandle) {
        UnhookWinEvent(hookHandle);
    }
}

bool WinHook::isRobloxInFocus() const {
    return robloxInFocus.load();
}

void CALLBACK WinHook::HookCallback(HWINEVENTHOOK, DWORD, HWND hwnd,
    LONG, LONG, DWORD, DWORD) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) {
        robloxInFocus.store(false);
        return;
    }

    WCHAR path[MAX_PATH];
    DWORD size = MAX_PATH;

    if (QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
        std::wstring exePath(path);
        if (exePath.find(L"RobloxPlayerBeta.exe") != std::wstring::npos) {
            robloxInFocus.store(true);
        }
        else {
            robloxInFocus.store(false);
        }
    }
    else {
        robloxInFocus.store(false);
    }

    CloseHandle(hProcess);
}