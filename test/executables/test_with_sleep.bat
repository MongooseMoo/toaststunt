@echo off
REM Use ping for delay - ping count is seconds + 1
ping localhost -n 2 >nul 2>&1
echo 1
ping localhost -n 3 >nul 2>&1
echo 2
ping localhost -n 4 >nul 2>&1
echo 3
