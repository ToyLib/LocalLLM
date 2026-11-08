@echo off
rem --------------------------------------------------
rem Model cache cleaner (Windows)
rem
rem Wrapper for clean-model-cache.ps1 that skips the PowerShell
rem execution policy, so it can be started by double-click or from cmd.
rem
rem Run:
rem   scripts\clean-model-cache.bat
rem   (add --list or -List to only list the models)
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

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0clean-model-cache.ps1" %ARGS%
set "EXIT_CODE=%ERRORLEVEL%"

rem Keep the window open when started by double-click
pause

exit /b %EXIT_CODE%
