# TASX Optimizer

TASX is an optimizer designed to aid in combatting Roblox engine's not-so-good optimization and memory hogging, while increasing FPS. No, this will not get you banned, it is not a cheat.

## What does it do?

TASX watches for Roblox processes (WMI process watcher + event-driven foreground hook, with a polling safety net) and continuously rebalances the system around them:

**Focused instance (the window you play in)**

- `HIGH_PRIORITY_CLASS` + full CPU affinity
- Exempt from Windows EcoQoS / power throttling (proper `PROCESS_POWER_THROTTLING_STATE`)
- Normal I/O & memory priority
- 0.5 ms system timer resolution for smoother frame pacing (auto-off on farm presets without focus — pure farm doesn't need it)

**Background instances (multi-account farming)**

- `IDLE_PRIORITY_CLASS`, pinned to efficiency cores on hybrid CPUs (or the low half of the CPU otherwise)
- Windows Efficiency Mode (process-level EcoQoS only, no per-thread enumeration), VeryLow I/O priority, Low memory priority on farms (`BackgroundMemPriority=2`, VeryLow elsewhere)
- Farm keep-hot (`FarmKeepHot=1` on farm presets): unfocused farm clients are WORKING, not idle — periodic pass is soft-only, hard trim (`EmptyWorkingSet`) only after `HardTrimAfterSec` (default 1800s) of continuous unfocus or commit-critical ≥90% — **never while focused**; below `TrimSkipBelowMB` (default 250) is skipped (0 syscalls, cached); 30s log line carries `avg WS MB, commit%` for farm sizing
- **FarmBoost hotkey** (`BoostHotkey=Ctrl+Alt+B`, system-wide): one keypress flips ALL clients between `IDLE/E-cores/EcoQoS` and full power (`HIGH/all cores`, EcoQoS off, trimming suspended) with a single summary line — the fix for farms that rot in efficiency mode. Newborns join the hot side automatically; focus switches never demote while ON; `FarmBoostDefault=1` starts hot. Set `BoostHotkey=off` to disable (e.g. combo taken by macro soft)

**System-wide**

- Game DVR / background capture off, MMCSS `Games` profile raised, network throttling off (`SystemResponsiveness=0`)
- High-performance GPU preference registered for the actual `RobloxPlayerBeta.exe` path (falls back to exe name)
- Power scheme switched to Ultimate/High Performance while Roblox runs, restored afterwards
- Standby memory list purged on launch, debounced to 1 per 60s for pack spawns (needs admin, otherwise safe-mode without HKLM/standby/system-cleaner); system cleaner is split: `SystemCleanStandby=1` (safe page-cache reclaim) vs `SystemCleanEmptyWS=0` on farms (global empty working sets evict whole farms — opt-in only)
- `RobloxCrashHandler.exe` suppressed on an ongoing sweep (job-child filter: only crash handlers are killed)
- TASX FastFlags `farm20` (default; `farm15` = deprecated alias, `farm30` staged for macro-speed A/B): **Roblox allowlist build (09.2025+, 18 keys)** — alive potato only: `TextureQuality=0`, `FRMQuality 0`, grass distances 0 + still air, CSG switching distances low (100/75/100/150), `PauseVoxelizer`, `SkyGray`, `MSAA=1`, `NoDPIScale`, `D3D11` (D3D10/Voxel/streaming/telemetry/physics/LOD flags are dead client-side and are NOT written; `DFIntTaskSchedulerTargetFps` kept as zero-cost FPS-intent placeholder) — merged into every installed client version's `ClientSettings\ClientAppSettings.json` atomically, preserving your own flags; mtime+size+hash skip avoids redundant rewrites; scans debounced to 1 per 5s with dirty flag; `FFlagsPruneDead=1` (farm default) removes pre-allowlist leftovers, hourly `Effective set` line shows alive counts; unknown/dead flags filtered with one-time `LOGW` + live replacement hint; anti-flags (`Future`/`ShadowMap`, `TextureQuality 1-3`, `FRM>0`, `MSAA>1`, grass>0) forced down/removed. Presets: `[FastFlags] Preset=farm20|farm30|weak|balanced|off` + manual `key=value` on top; legacy `[Roblox]` section kept for compat (D3D10→D3D11 fallback, Voxel→PauseVoxelizer+SkyGray mapping)
- File logging (`[Log] LogFile`, 1 MB rotation to `.old`, simultaneous stdout) with level filter (`LogLevel=info|warn|error`); all log lines go through `LOGI/W/E` and are rate-limited
- Hot-reload: `TASX.ini` mtime is polled every 10 s on the main loop — edits re-apply FFlags/tweaks/job limits and trimmer thresholds without restart or new threads

All of the above is configurable — see `TASX.ini` (documented, optional; sane defaults apply without it).

## Injector Coexistence Contract

TASX and the external FFlags/Injector launcher are designed to share the same clients without conflict.

| Concern | Owner |
|---------|-------|
| FPS cap, render quality, per-client RAM cap | External injector (when `InjectorOwnsGraphics=1`) else TASX farm preset |
| CPU priority, affinity, EcoQoS, I/O+mem priority, job limits | TASX |
| Audio mute | TASX (WASAPI) |
| Telemetry disable | Both (FFlags + ETW) |

Ownership rule (`Infra/fflags.cc:BuildFlagPlan`): graphics keys (`TargetFps`, renderer, lighting, texture, `GpuTextureCompressor`, `UseLevelOfDetail`) are skipped when `InjectorOwnsGraphics=1` unless `ForceGraphicsFlags=1` overrides. Streaming/telemetry always written. `ApplyToVersion` still performs the atomic read-modify-write (temp-file + `MoveFileEx`), preserving every injector-owned and user JSON key.
Default: legacy configs without the key keep `InjectorOwnsGraphics=1` (old behavior); new farm `TASX.ini` ships `InjectorOwnsGraphics=0` + `Preset=farm20` (TASX owns graphics). With a farm preset (`farm15|farm20|farm30|weak|balanced`) a missing key defaults to `0`; without preset it defaults to `1`.

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
               power/Wait/Mutex teardown); hot-reload of TASX.ini by mtime;
               FocusDwellMs anti-flap (default 1800ms, first focus instant),
               pagefile-volume warning (PagefileWarnFreeGB, free/total + hint),
               missing TASX.ini auto-create with documented defaults
  WMI.cc       Async WMI process watcher with Indication-drain (active count
               + manual-reset event) to avoid Release races; extracts PID
               straight from TargetInstance.Handle
  jobs.cc      Single background cgroup (farm CPU/MEM caps, KILL_ON_CLOSE
               gated by KillOnAgentExit); JobAssignMode=auto|diagnose|off
               (force = deprecated alias), per-PID rate-limited sticky
               fallback for foreign Job (ERROR_ACCESS_DENIED/
               ALREADY_ASSIGNED, no BREAKAWAY_OK); focus is per-process
               (priority/affinity/EcoQoS via CPU.cc, never a cross-job move);
               IOCP filters NEW_PROCESS to robloxcrashhandler.exe only
  CPU.cc       No topology state — single source of truth is ntsys.c
               (tasx_get_*_mask); per-process scheduling profile + boost toggle
  trimmer.cc   One scheduler thread + min-heap of trim deadlines: adaptive
               interval, skip-if-below-threshold, focused never trimmed,
               farm keep-hot (soft-only, hard after HardTrimAfterSec);
               shared 5s pressure snapshot; no LowMem handle (single owner
               is the master reactor)
  stats.cc     Whole-system process snapshot in ONE syscall + helper
               QueryProcessNameByPid for the job-port filter
  winhook.cc   EVENT_SYSTEM_FOREGROUND hook (notification only; focus PID is
               read via GetForegroundWindow — single mechanism)
  hotkey.cc    System-wide FarmBoost hotkey via RegisterHotKey on a dedicated
               message-pump thread (no window/DLL/polling); combo parsed from
               BoostHotkey, event-driven toggle in master.cpp
  ntsys.c      [C] ntdll/privilege layer + file logging (tasx_log, 1 MB
               rotation), P/E topology, commit charge, ETW (verified GUIDs)
  config.c     [C] TASX.ini reader with BOM skip + hot-reload (config_reload),
               auto-create defaults (config_create_default), entry iterator
               for [FastFlags] manual overrides
  tweaks.cc    One-shot registry tweaks + power scheme; UserGpuPreferences
               resolves the full exe path; HKLM gated by elevation
  fflags.cc    Roblox ClientAppSettings.json reader/writer with escaped-quote
               aware parsing and mtime+size+hash skip (atomic tmp+MoveFileEx,
               5s debounce + dirty flag); farm20/farm30(+farm15 alias)/weak/
               balanced presets built from the official 18-key allowlist
               (dead pre-allowlist flags not written, optional PruneDead
               cleanup, hourly effective-set log); anti-flags,
               InjectorOwnsGraphics/ForceGraphicsFlags gate
```

Farm `TASX.ini` keys: `[TASX] JobAssignMode=auto|diagnose|off`, `FarmKeepHot=1`, `HardTrimAfterSec=1800`, `BackgroundMemPriority=2`, `TrimSkipBelowMB=250`, `SystemCleanStandby=1`, `SystemCleanEmptyWS=0`, `FocusDwellMs=1800` (legacy `FocusHysteresisMs` fallback), `PagefileWarnFreeGB=8`, `BackgroundCpuCapPercent=25`, `JobMemoryCapMB=8192` (per-process); `[FastFlags] Preset=farm20` + `FFlagsPruneDead=1` + manual `key=value`; `[Roblox] UncapFps=0 TargetFps=20 Renderer=D3D10 Lighting=Voxel TextureQuality=0` (legacy, overridden by preset; D3D10→D3D11, Voxel→PauseVoxelizer). Budget rule: `N × avgWS < RAM × 0.75` or keep-hot is impossible (rate-limited `LOGW`).

Designed for 100+ concurrent clients: no polling loops on the hot path (exits, crash handlers, memory cleaning and discovery are event-driven), ~5 threads total regardless of client count, and one syscall for whole-system state.

The low-level core (`config.c`, `ntsys.c`) is plain C, compiled by the C compiler and linked into the C++ binary; everything that talks to Win32 is resolved dynamically so the binary runs on any Windows 10/11 version.

## Prereqs (All)

- Be on Windows

## Prereqs (If compiling)

- Visual Studio w/ C++ build tools (C++ 17), or MSYS2/MinGW-w64 (g++ + gcc)
