@echo off
REM Clear the stop flag. The next scheduled tick picks up where it left off -
REM every job is resumable, so nothing was lost by stopping.
if exist "%~dp0STOP" del "%~dp0STOP"
echo.
echo   MAGPIE BACKGROUND RUNNER: RESUMED
echo.
echo   The next scheduled tick will continue where it stopped.
echo.
pause
