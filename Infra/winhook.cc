#include "winhook.h"

#include "log.h"

WinHook::~WinHook()
{
    if (m_hook) UnhookWinEvent(m_hook);
    if (m_pumpThread) {
        PostThreadMessageW(m_pumpThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(m_pumpThread, 2000);
        CloseHandle(m_pumpThread);
    }
}

void WinHook::Start()
{
    m_pumpThread = CreateThread(
        nullptr, 0,
        [](LPVOID self) -> DWORD {
            auto* hook = static_cast<WinHook*>(self);

            hook->m_hook = SetWinEventHook(
                EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                nullptr, HookCallback,
                0, 0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

            if (!hook->m_hook) {
                LOGW("[TASX] Foreground hook unavailable, polling only");
                return 0;
            }

            /* GetMessageW pumps events to the hook callback until WM_QUIT. */
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) { /* drain */ }

            return 0;
        },
        this, 0, &m_pumpThreadId);

    if (!m_pumpThread)
        LOGW("[TASX] Failed to start foreground hook pump");
}

void CALLBACK WinHook::HookCallback(HWINEVENTHOOK, DWORD, HWND,
                                    LONG, LONG, DWORD, DWORD)
{
    NotifyFocusChanged();
}
