#include "hotkey.h"

#include "config.h"
#include "log.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

/* Parses "Ctrl+Alt+B" / "Shift+F9" / "off". Returns false when disabled or
   malformed. mod accumulates MOD_*; vk gets the virtual-key code. */
bool ParseCombo(const char* s, UINT& mod, UINT& vk)
{
    mod = 0;
    vk = 0;
    if (!s || !*s) return false;

    char buf[64] = {};
    size_t n = 0;
    for (; s[n] && n + 1 < sizeof(buf); ++n) {
        char c = s[n];
        if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
        buf[n] = (c == ' ' || c == '\t') ? '\0' : c;
    }
    // NOTE: spaces become separators (strtok below splits on '+' only after
    // compacting; empty tokens are skipped).
    if (strcmp(buf, "OFF") == 0 || strcmp(buf, "NONE") == 0 ||
        strcmp(buf, "DISABLED") == 0 || strcmp(buf, "0") == 0)
        return false;

    // compact: drop NULs introduced above, keep '+' as separator
    char clean[64] = {};
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < sizeof(clean); ++i) {
        if (buf[i] != '\0') clean[w++] = buf[i];
    }

    char* ctx = nullptr;
    for (char* tok = strtok_s(clean, "+", &ctx); tok;
         tok = strtok_s(nullptr, "+", &ctx)) {
        if (*tok == '\0') continue;
        if (strcmp(tok, "CTRL") == 0 || strcmp(tok, "CONTROL") == 0)
            mod |= MOD_CONTROL;
        else if (strcmp(tok, "ALT") == 0)
            mod |= MOD_ALT;
        else if (strcmp(tok, "SHIFT") == 0)
            mod |= MOD_SHIFT;
        else if (strcmp(tok, "WIN") == 0 || strcmp(tok, "WINDOWS") == 0)
            mod |= MOD_WIN;
        else if (vk != 0)
            return false; // two main keys
        else if (strlen(tok) == 1 && isalnum((unsigned char)tok[0]))
            vk = (UINT)tok[0]; // 'A'-'Z'/'0'-'9' == VK codes
        else if (tok[0] == 'F' && tok[1] >= '1' && tok[1] <= '9' && (tok[2] == '\0' || (tok[2] >= '0' && tok[2] <= '9')) && (tok[1] != '0' || tok[2] != '0')) {
            int f = atoi(tok + 1);
            if (f < 1 || f > 24) return false;
            vk = (UINT)(VK_F1 + f - 1);
        } else {
            return false;
        }
    }
    if (vk == 0 || mod == 0) return false;
    return true;
}

} // namespace

FarmHotkey::~FarmHotkey()
{
    Stop();
}

void FarmHotkey::Start()
{
    if (m_thread) return;

    const char* combo = config_get_str("TASX", "BoostHotkey", "Ctrl+Alt+B");
    UINT mod = 0, vk = 0;
    if (!ParseCombo(combo, mod, vk)) {
        LOGI("[TASX] FarmBoost hotkey disabled (BoostHotkey=%s)", combo ? combo : "off");
        return;
    }

    // The pump thread re-parses the combo itself: RegisterHotKey with a
    // NULL hwnd must run ON the thread that pumps messages.
    m_thread = CreateThread(nullptr, 0, PumpThread, this, 0, &m_threadId);
    if (!m_thread)
        LOGW("[TASX] FarmBoost hotkey thread failed to start");
}

DWORD WINAPI FarmHotkey::PumpThread(LPVOID self)
{
    auto* hk = static_cast<FarmHotkey*>(self);

    const char* combo = config_get_str("TASX", "BoostHotkey", "Ctrl+Alt+B");
    UINT mod = 0, vk = 0;
    if (!ParseCombo(combo, mod, vk)) return 0;

    if (!RegisterHotKey(nullptr, hk->m_hotkeyId,
                        mod | MOD_NOREPEAT, vk)) {
        DWORD err = GetLastError();
        LOGW("[TASX] FarmBoost hotkey '%s' busy (err %lu) - pick another BoostHotkey or set off",
             combo, (unsigned long)err);
        return 0;
    }
    hk->m_registered.store(true);
    {
        char comboLog[64] = {};
        snprintf(comboLog, sizeof(comboLog), "%s", combo);
    LOGI("[TASX] FarmBoost hotkey armed: %s (page-in from pagefile; always P+E)",
         comboLog);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && (int)msg.wParam == hk->m_hotkeyId)
            TasxNotifyHotkey();
    }

    if (hk->m_registered.load()) {
        UnregisterHotKey(nullptr, hk->m_hotkeyId);
        hk->m_registered.store(false);
    }
    return 0;
}

void FarmHotkey::Stop()
{
    if (m_registered) {
        // Unregister from owner thread via message: post quit first, the
        // pump unregisters before returning.
    }
    if (m_thread) {
        PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(m_thread, 2000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    m_registered.store(false);
}
