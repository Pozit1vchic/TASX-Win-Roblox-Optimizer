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
- Windows Efficiency Mode (process + every thread), VeryLow I/O priority, VeryLow memory priority
- Working set hard-trimmed on a schedule — **never while focused**, so trimming can't cause in-game stutter

**System-wide**

- Game DVR / background capture off, MMCSS `Games` profile raised, network throttling off (`SystemResponsiveness=0`)
- High-performance GPU preference registered for `RobloxPlayerBeta.exe`
- Power scheme switched to Ultimate/High Performance while Roblox runs, restored afterwards
- Standby memory list purged the moment Roblox launches (and optionally on a timer) — the game gets fresh RAM pages
- `RobloxCrashHandler.exe` suppressed on an ongoing sweep
- TASX FastFlags: FPS cap removed (999), optional renderer (Vulkan/D3D11/D3D10/OpenGL), lighting tech (Voxel/ShadowMap/Future), texture quality, telemetry off — merged into every installed client version's `ClientSettings\ClientAppSettings.json`, preserving your own flags

All of the above is configurable — see `TASX.ini` (documented, optional; sane defaults apply without it).

## Injector Coexistence Contract

TASX and the external FFlags/Injector launcher are designed to share the same clients without conflict.

| Component | Owns | Never touches |
|---|---|---|
| TASX (this .exe) | Priority, affinity, EcoQoS, I/O + mem priority, Job limits, trim, audio mute, ETW | Graphics settings (FPS, renderer, lighting, texture) |
| Injector (external) | FPS cap (`UncapFps`/`TargetFps`), render mode (`Renderer`), lighting (`Lighting`), texture (`TextureQuality`), volume | Job limits, affinity, trim |

When `InjectorOwnsGraphics=1` (default), `FFlagsApply` skips graphics keys and writes only telemetry-disable + cache settings, preserving unknown JSON keys via atomic temp-file + `MoveFileEx`.

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
               reactor; single-threaded state machine (main/WinMain via
               TASX_CONSOLE|TASX_GUI)
  WMI.cc       Async WMI process watcher; extracts the PID straight from
               TargetInstance.Handle - zero process-table rescans
  jobs.cc      Job-per-client: priority/affinity/CPU+RAM caps rewritten in
               place per focus switch (1 syscall); shared IOCP reports
               EXIT_PROCESS / NEW_PROCESS event-driven (no exit polling,
               instant RobloxCrashHandler kill)
  CPU.cc       Topology discovery via GetLogicalProcessorInformationEx:
               P-core/E-core masks, per-process scheduling fallback profile,
               CPU boost toggle
  trimmer.cc   One scheduler thread + min-heap of trim deadlines: adaptive
               interval by memory pressure, skip-if-below-threshold (0
               syscalls), focused instance never trimmed
  stats.cc     Whole-system process snapshot in ONE syscall
               (NtQuerySystemInformation) for stats/discovery/sweeps
  winhook.cc   EVENT_SYSTEM_FOREGROUND hook with a dedicated message pump
               thread -> instant focus rebalancing
  ntsys.c      [C] ntdll/privilege layer: NtSetTimerResolution,
               NtSetSystemInformation (standby purge, system-wide empty
               working sets), ProcessIoPriority, memory priority, power
               throttling, system process table
  config.c     [C] TASX.ini INI reader (no CRT-specific helpers)
  tweaks.cc    One-shot registry tweaks + power scheme switch/restore
  fflags.cc    Roblox ClientAppSettings.json reader/writer (merge, atomic
               write, per-copy caps for multi-manager farms)
```

Designed for 100+ concurrent clients: no polling loops on the hot path (exits, crash handlers, memory cleaning and discovery are event-driven), ~5 threads total regardless of client count, and one syscall for whole-system state.

The low-level core (`config.c`, `ntsys.c`) is plain C, compiled by the C compiler and linked into the C++ binary; everything that talks to Win32 is resolved dynamically so the binary runs on any Windows 10/11 version.

## Prereqs (All)

- Be on Windows

## Prereqs (If compiling)

- Visual Studio w/ C++ build tools (C++ 17), or MSYS2/MinGW-w64 (g++ + gcc)
