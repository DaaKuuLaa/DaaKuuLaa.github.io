@echo off
chcp 65001 >nul
echo ========================================
echo  Copy Log Files to Clipboard (with timestamp)
echo ========================================
echo.

set output=%temp%\logs_combined.txt

if exist "%output%" del "%output%"

set count=0
echo ======================================== >> "%output%"
echo Log Export Time: %date% %time% >> "%output%"
echo ======================================== >> "%output%"
echo. >> "%output%"

for %%f in (*.log) do (
    echo ======================================== >> "%output%"
    echo [%%f] >> "%output%"
    echo ======================================== >> "%output%"
    type "%%f" >> "%output%"
    echo. >> "%output%"
    echo. >> "%output%"
    set /a count+=1
)

if %count%==0 (
    echo No log files found.
    pause
    exit /b
)

echo Combining %count% log file(s)...

:: 使用 clip 命令复制到剪切板
type "%output%" | clip

echo Copied to clipboard! (Total: %count% files)
echo.

:: 预览
echo Preview (first 15 lines):
echo ----------------------------------------
type "%output%" | findstr /n . | findstr "^[0-9]: ^[1-9]: ^10: ^11: ^12: ^13: ^14: ^15:"
echo ----------------------------------------

del "%output%"
pause