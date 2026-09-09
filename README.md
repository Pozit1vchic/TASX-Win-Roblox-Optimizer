# TASX Optimizer

TASX is an optimizer designed to aid in combatting Roblox engine's not-so-good optimization and memory hogging, while increasing FPS. No, this will not get you banned, it is not a cheat.

## What does it do?

TASX watches for Roblox processes (WMI process watcher + event-driven foreground hook, with a polling safety net) and continuously rebalances the system around them:

**Focused instance (the window you play in)**

- `HIGH_PRIORITY_CLASS` + full CPU affinity
- Exempt from Windows EcoQoS / power throttling (proper `PROCESS_POWER_THROTTLING_STATE`)
- Normal I/O & memory priority
- 0.5 ms system timer resolution for smoother frame pacing

**Background instances (multi-account farming)**

- `IDLE_PRIORITY_CLASS`, pinned to efficiency cores on hybrid CPUs (or the low half of the CPU otherwise)
- Windows Efficiency Mode (process-level EcoQoS only, no per-thread enumeration), VeryLow I/O priority, VeryLow memory priority
- Working set trimmed on a schedule — soft for recently unfocused, hard only for long-inactive — **never while focused**, so trimming can't cause in-game stutter; below `TrimSkipBelowMB` is skipped (0 syscalls, cached)

**System-wide**

- Game DVR / background capture off, MMCSS `Games` profile raised, network throttling off (`SystemResponsiveness=0`)
- High-performance GPU preference registered for the actual `RobloxPlayerBeta.exe` path (falls back to exe name)
- Power scheme switched to Ultimate/High Performance while Roblox runs, restored afterwards
- Standby memory list purged the moment Roblox launches (needs admin, otherwise safe-mode without HKLM/standby/system-cleaner)
- `RobloxCrashHandler.exe` suppressed on an ongoing sweep (job-child filter: only crash handlers are killed)
- TASX FastFlags: FPS cap removed (uncapped when `UncapFps=1`, otherwise `TargetFps`), optional renderer (Vulkan/D3D11/D3D10/OpenGL), lighting tech (Voxel/ShadowMap/Future), texture quality, telemetry off — merged into every installed client version's `ClientSettings\ClientAppSettings.json` atomically, preserving your own flags; mtime+hash skip avoids redundant rewrites
- File logging (`[Log] LogFile`, 1 MB rotation to `.old`, simultaneous stdout) with level filter (`LogLevel=info|warn|error`); all log lines go through `LOGI/W/E` and are rate-limited
- Hot-reload: `TASX.ini` mtime is polled every 10 s on the main loop — edits re-apply FFlags/tweaks/job limits and trimmer thresholds without restart or new threads

All of the above is configurable — see `TASX.ini` (documented, optional; sane defaults apply without it).

## Injector Coexistence Contract

TASX and the external FFlags/Injector launcher are designed to share the same clients without conflict.

| Concern | Owner |
|---------|-------|
| FPS cap, render quality, per-client RAM cap | External injector |
| CPU priority, affinity, EcoQoS, I/O+mem priority, job limits | TASX |
| Audio mute | TASX (WASAPI) |
| Telemetry disable | Both (FFlags + ETW) |

When `InjectorOwnsGraphics=1` (default), `BuildFlagPlan` omits the graphics keys (FPS unlock, Renderer, Lighting, TextureQuality) and TASX writes only the telemetry-disable flags; `ApplyToVersion` still performs the atomic read-modify-write (temp-file + `MoveFileEx`), preserving every injector-owned and user JSON key.

## What do I need to know?

- HKLM-level tweaks and the standby-list purge require elevation — run ``ScheduledTaskInstaller.bat`` once (creates the elevated "TASX Agent" startup task). Values are written only when they differ, everything is idempotent.
- To remove TASX, run ``Uninstall.bat``.
- Only one TASX instance can run at a time (single-instance guard).

## How do I use this?

For non-programmers, head over to the [RYFTENIUS Discord](https://hub.ryftenius.com/) & download the latest release in #OPTIMIZER (This comes with the source), install TASX with ``ScheduledTaskInstaller.bat``, to remove use ``Uninstall.bat``.

This will automatically add TASX to startup as "TASX Agent".

For programmers, open the solution file & compile: **Debug** = console build with logging, **Release** = silent windowed build. Keep in mind ``TASX.exe`` must be in the same DIR as the ``.bat`` files (and optionally ``TASX.ini``) for it to be serviced.

Or build from the command line (MSYS2/MinGW):

```bash
make            # console build with logging
make windows    # silent GUI-subsystem build
make clean
```

## How does it work under the hood?

```
Infra/
  master.cpp   Orchestrator: one typed event queue fed by WMI, the job
               completion port, the foreground hook and the low-memory
               reactor; single-threaded state machine via InitSubsystems() /
               ShutdownSubsystems() and a Ctrl-handler (graceful exit,
               power/Wait/Mutex teardown); hot-reload of TASX.ini by mtime
  WMI.cc       Async WMI process watcher with Indication-drain (active count
               + manual-reset event) to avoid Release races; extracts PID
               straight from TargetInstance.Handle
  jobs.cc      Single background cgroup (farm CPU/MEM caps, KILL_ON_CLOSE
               gated by KillOnAgentExit); focus is per-process (priority/
               affinity/EcoQoS via CPU.cc, never a cross-job move — always
               ERROR_ACCESS_DENIED); IOCP filters NEW_PROCESS to
               robloxcrashhandler.exe only
  CPU.cc       No topology state — single source of truth is ntsys.c
               (tasx_get_*_mask); per-process scheduling profile + boost toggle
  trimmer.cc   One scheduler thread + min-heap of trim deadlines: adaptive
               interval, skip-if-below-threshold, focused never trimmed,
               soft/hard by unfocused age; no LowMem handle (single owner is
               the master reactor)
  stats.cc     Whole-system process snapshot in ONE syscall + helper
               QueryProcessNameByPid for the job-port filter
  winhook.cc   EVENT_SYSTEM_FOREGROUND hook (notification only; focus PID is
               read via GetForegroundWindow — single mechanism)
  ntsys.c      [C] ntdll/privilege layer + file logging (tasx_log, 1 MB
               rotation), P/E topology, commit charge, ETW (verified GUIDs)
  config.c     [C] TASX.ini reader with BOM skip + hot-reload (config_reload)
  tweaks.cc    One-shot registry tweaks + power scheme; UserGpuPreferences
               resolves the full exe path; HKLM gated by elevation
  fflags.cc    Roblox ClientAppSettings.json reader/writer with escaped-quote
               aware parsing and mtime+hash skip (atomic tmp+MoveFileEx)
```

Designed for 100+ concurrent clients: no polling loops on the hot path (exits, crash handlers, memory cleaning and discovery are event-driven), ~5 threads total regardless of client count, and one syscall for whole-system state.

The low-level core (`config.c`, `ntsys.c`) is plain C, compiled by the C compiler and linked into the C++ binary; everything that talks to Win32 is resolved dynamically so the binary runs on any Windows 10/11 version.

## Prereqs (All)

- Be on Windows

## Prereqs (If compiling)

- Visual Studio w/ C++ build tools (C++ 17), or MSYS2/MinGW-w64 (g++ + gcc)
