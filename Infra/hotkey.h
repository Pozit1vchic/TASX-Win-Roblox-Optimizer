#pragma once

#include <windows.h>
#include <atomic>

/* Global FarmBoost hotkey. System-wide (works while any window is focused):
   toggles ALL tracked clients between the background profile
   (IDLE/E-cores/EcoQoS) and the boosted profile (HIGH/all cores, EcoQoS
   off, no trimming). Implemented with RegisterHotKey on a dedicated
   message-pump thread — no window, no DLL, no polling.
   Default combo: Ctrl+Alt+B. Override with TASX.ini [TASX] BoostHotkey,
   e.g. BoostHotkey=Ctrl+Alt+F9. BoostHotkey=off disables the hotkey.
   Implemented in master.cpp: enqueues a HOTKEY event for the main loop. */
void TasxNotifyHotkey(void);

class FarmHotkey {
public:
    FarmHotkey() = default;
    ~FarmHotkey();

    /* Parses BoostHotkey and starts the pump thread. No-op (with one LOGW)
       when disabled or registration fails (e.g. combo taken by macro soft). */
    void Start();

    /* Stops the pump thread and unregisters. */
    void Stop();

private:
    FarmHotkey(const FarmHotkey&) = delete;
    FarmHotkey& operator=(const FarmHotkey&) = delete;

    static DWORD WINAPI PumpThread(LPVOID self);

    HANDLE m_thread = nullptr;
    DWORD m_threadId = 0;
    int m_hotkeyId = 1;
    std::atomic<bool> m_registered{false};
};
