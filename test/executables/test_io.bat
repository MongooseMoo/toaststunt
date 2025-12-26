@echo off
setlocal enabledelayedexpansion
set /p input=
<nul set /p =!input!
>&2 <nul set /p =!input!
exit /b 0
