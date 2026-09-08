#pragma once

#include <windows.h>

/* GDI / USER object monitor for 100+ windowed clients. Windows caps each
   process at GDIProcessHandleQuota (default 10 000) USER/GDI objects, and
   the whole desktop shares one win32k heap - a farm approaching either
   limit fails window creation randomly. TASX watches each client and warns
   rate-limited before the cliff. */
void DesktopCheckClient(DWORD pid, HANDLE hProc);
