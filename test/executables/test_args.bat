@echo off
setlocal enabledelayedexpansion
set "args="
for %%a in (%*) do (
    if defined args (
        set "args=!args! %%a"
    ) else (
        set "args=%%a"
    )
)
<nul set /p ="!args!"
exit /b 0
