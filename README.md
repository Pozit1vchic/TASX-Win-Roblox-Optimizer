# TASX Optimizer

TASX is an optimizer designed to aid in combatting Roblox engine's not-so-good optimization and memory hogging, while increasing FPS. No, this will not get you banned, it is not a cheat.

## What does it do?

TASX reduces resource usage (RAM & CPU) for unfocused Roblox windows by adjusting Windows API configuration settings, if a Roblox window is in-focus it'll be elevated to highest CPU priority & affinity (Leads to higher FPS / perf)

This is useful for both farmers & daily players.

## How do I use this?

For non-programmers, head over to the [TITAN Discord](https://hub.titansoftwork.com/) & download the the latest release in #OPTIMIZER (This comes with the source), install TASX with ``ScheduledTaskInstaller. bat``, to remove use ``Uninstall.bat``

This will automatically add TASX to startup as "TASX Agent".

For programmers, open the solution file & compile as Debug for console debugging or Release for the actual product, keep in mind ``TASX.exe`` must be in the same DIR as the ``.bat`` files for it to be serviced.

## How does it work under the hood?

## Prereqs (All)

- Be on Windows

## Prereqs (If compiling)

- Visual Studio w/ C++ build tools (C++ 17)
