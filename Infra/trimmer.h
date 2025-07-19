#pragma once
#include <Windows.h>

bool StartTrimmer(DWORD pid, HANDLE processHandle);
void StopTrimmer(DWORD pid);
void StopAllTrimmers();