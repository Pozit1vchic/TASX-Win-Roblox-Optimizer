@echo off
set TASKNAME=TASX_Agent

:: UAC Prompt (Administrator)
:: We need this to remove TASX because TASX's set as an elevated task (Needs Administrator)
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Administrator elevation required. Prompting...
    set "params=%*"
    echo Set UAC = CreateObject^("Shell.Application"^) > "%temp%\uac.vbs"
    echo UAC.ShellExecute "cmd.exe", "/c ""%~s0"" %params%", "", "runas", 1 >> "%temp%\uac.vbs"
    "%temp%\uac.vbs"
    del "%temp%\uac.vbs"
    exit /b
)

schtasks /query /tn "%TASKNAME%" >nul 2>&1
if %errorlevel% neq 0 (
    echo TASX Does not exist on your system. Nothing to remove.
    pause
    exit /b
)

schtasks /delete /tn "%TASKNAME%" /f
echo Task '%TASKNAME%' has been removed.
pause