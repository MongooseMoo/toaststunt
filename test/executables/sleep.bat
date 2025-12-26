@echo off
REM Use ping for delay - ping count is seconds + 1
set /a "count=%1+1"
ping localhost -n %count% >nul 2>&1
