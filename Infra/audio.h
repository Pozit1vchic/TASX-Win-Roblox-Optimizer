#pragma once

#include <windows.h>
#include <unordered_set>

/* Background audio suppression via the WASAPI Session API — the same
   cross-process mechanism the Windows Volume Mixer uses. No injection:
   TASX enumerates audio sessions from outside and mutes sessions owned by
   background Roblox clients; the focused client keeps its audio. */
void AudioApplyBackgroundMute(const std::unordered_set<DWORD>& clientPids,
                              DWORD focusedPid);
