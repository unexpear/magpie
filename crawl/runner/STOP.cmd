@echo off
REM Halt the background runner. Takes effect before the next request:
REM the runner checks for this file first thing on every tick.
REM
REM Stopping must never be harder than starting, so this asks nothing and
REM needs nothing running. Double-click it.
echo stopped by user > "%~dp0STOP"
echo.
echo   MAGPIE BACKGROUND RUNNER: STOPPED
echo.
echo   No further requests will be made.
echo   Anything in progress finishes its current request and exits.
echo.
echo   Run RESUME.cmd to start again.
echo.
pause
