#pragma once

/* Writes TASX's curated FastFlag set into every installed client version's
   ClientSettings\ClientAppSettings.json (%LOCALAPPDATA%\Roblox\Versions).
   Existing user flags are preserved; TASX flags are merged over them.
   Call at startup and whenever a new Roblox process appears (version
   updates install new version folders). */
void FFlagsApply();

/* Marks the flag set dirty so the next FFlagsApply() performs a full scan
   (debounce: scans are otherwise coalesced to one per 5s). */
void FFlagsMarkDirty();
