@echo off
rem --------------------------------------------------
rem Windows distribution package
rem
rem Wrapper for package-windows.ps1 that skips the PowerShell
rem execution policy, so it can be started by double-click or from cmd.
rem
rem Do not run it from the Developer Command Prompt for VS
rem (CMakeLists.txt explains why).
rem
rem Run:
rem   scripts\package-windows.bat
rem   (add -SkipBuild to package the existing build\Release as it is,
rem    -Version 1.0.0 to replace the git commit in the file name)
rem --------------------------------------------------

setlocal

rem cmake --preset needs the repository root as the current folder,
rem but a double-click starts in scripts\
pushd "%~dp0.."

powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\package-windows.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"

popd

rem Keep the window open when started by double-click
pause

exit /b %EXIT_CODE%
