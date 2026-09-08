@echo off
rem Runs TASX with a visible log console. Close this window to stop TASX.
rem If TASX is already running as the TASX Agent startup task, stop it first:
rem   taskkill /IM TASX.exe /F
cd /d "%~dp0"
TASX.exe
pause
