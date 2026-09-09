#pragma once

#include <windows.h>

/* Event-driven foreground watcher. WINEVENT_OUTOFCONTEXT callbacks are only
   delivered to a thread pumping messages, so Start() owns a dedicated pump
   thread. A 250 ms poll in the main loop acts as the safety net.
   Focus state is read by the main loop via GetForegroundWindow — this
   class only delivers change notifications (single mechanism, no cached
   PID that could desync). */
class WinHook {
public:
    WinHook() = default;
    ~WinHook();

    /* Sets the hook and starts the pump thread. */
    void Start();

    /* Implemented in master.cpp: enqueues a FOCUS event for the main loop. */
    static void NotifyFocusChanged();

private:
    WinHook(const WinHook&) = delete;
    WinHook& operator=(const WinHook&) = delete;

    static void CALLBACK HookCallback(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
        LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);

    HWINEVENTHOOK m_hook = nullptr;
    HANDLE m_pumpThread = nullptr;
    DWORD m_pumpThreadId = 0;
};
