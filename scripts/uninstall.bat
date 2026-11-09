@echo off
rem --------------------------------------------------
rem Uninstaller (Windows)
rem
rem Wrapper for uninstall.ps1 that skips the PowerShell
rem execution policy, so it can be started by double-click or from cmd.
rem
rem Run:
rem   scripts\uninstall.bat
rem   (add --list or -List to only list what would be deleted)
rem --------------------------------------------------

setlocal

set "ARGS="
for %%A in (%*) do (
    if /i "%%~A"=="--list" (
        call set "ARGS=%%ARGS%% -List"
    ) else (
        call set "ARGS=%%ARGS%% %%A"
    )
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %ARGS%
set "EXIT_CODE=%ERRORLEVEL%"

rem Keep the window open when started by double-click
pause

exit /b %EXIT_CODE%
