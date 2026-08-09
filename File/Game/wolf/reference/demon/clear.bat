@echo off
chcp 65001 >nul
echo ========================================
echo  Clear Log Files
echo ========================================
echo.

set count=0
for %%f in (*.log) do (
    echo Clearing: %%f
    type nul > "%%f"
    set /a count+=1
)

if %count%==0 (
    echo No log files found.
) else (
    echo %count% log file(s) cleared.
)

echo.
pause