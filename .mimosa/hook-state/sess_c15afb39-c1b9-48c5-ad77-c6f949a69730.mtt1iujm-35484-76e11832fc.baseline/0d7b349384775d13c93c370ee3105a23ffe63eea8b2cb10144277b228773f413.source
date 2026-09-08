#pragma once

/* Writes TASX's curated FastFlag set into every installed client version's
   ClientSettings\ClientAppSettings.json (%LOCALAPPDATA%\Roblox\Versions).
   Existing user flags are preserved; TASX flags are merged over them.
   Call at startup and whenever a new Roblox process appears (version
   updates install new version folders). */
void FFlagsApply();
