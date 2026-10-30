@echo off
rem --------------------------------------------------
rem llama.cpp server launcher (Windows / Vulkan)
rem
rem Wrapper for llama-server-windows.ps1 that skips the PowerShell
rem execution policy, so it can be started by double-click or from cmd.
rem
rem Run:
rem   scripts\llama-server-windows.bat
rem   (add --no-embed or -NoEmbed to start the chat server only, without RAG)
rem --------------------------------------------------

setlocal

set "ARGS="
for %%A in (%*) do (
    if /i "%%~A"=="--no-embed" (
        call set "ARGS=%%ARGS%% -NoEmbed"
    ) else (
        call set "ARGS=%%ARGS%% %%A"
    )
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0llama-server-windows.ps1" %ARGS%
set "EXIT_CODE=%ERRORLEVEL%"

rem Keep the window open on errors when started by double-click
if not "%EXIT_CODE%"=="0" pause

exit /b %EXIT_CODE%
