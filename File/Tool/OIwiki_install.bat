@echo off
setlocal EnableDelayedExpansion
title OI Wiki Auto Setup

echo ============================================
echo   OI Wiki Auto Setup Script
echo   Auto-install Git / Python if missing
echo   Multi-mirror fallback download
echo ============================================
echo.

REM ============================================================
REM Configuration: Mirror lists (priority order)
REM ============================================================
set "OIVIKI_GITHUB=https://github.com/OI-wiki/OI-wiki.git"
set "OIVIKI_GITEE=https://gitee.com/OI-wiki/OI-wiki.git"
set "OIVIKI_GITCLONE=https://gitclone.com/github.com/OI-wiki/OI-wiki.git"
set "OIVIKI_CNPMJS=https://github.com.cnpmjs.org/OI-wiki/OI-wiki.git"

set "GIT_VER=v2.47.1.windows.1"
set "GIT_FILE=Git-2.47.1-64-bit.exe"
set "GIT_DOWNLOAD_URLS=https://github.com/git-for-windows/git/releases/download/%GIT_VER%/%GIT_FILE%;https://registry.npmmirror.com/-/binary/git-for-windows/%GIT_VER%/%GIT_FILE%"

set "PYTHON_VERSION=3.12.3"
set "PYTHON_FILE=python-%PYTHON_VERSION%-amd64.exe"
set "PYTHON_DOWNLOAD_URLS=https://www.python.org/ftp/python/%PYTHON_VERSION%/%PYTHON_FILE%;https://mirrors.tuna.tsinghua.edu.cn/python/%PYTHON_VERSION%/%PYTHON_FILE%;https://mirrors.ustc.edu.cn/python/%PYTHON_VERSION%/%PYTHON_FILE%;https://registry.npmmirror.com/-/binary/python/%PYTHON_VERSION%/%PYTHON_FILE%"

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "TEMP_DIR=%TEMP%\oiwiki_setup"
if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"

REM ============================================================
REM Main flow
REM ============================================================
call :CheckAndInstallGit
if %errorlevel% neq 0 (
    echo [ERROR] Git installation failed. Cannot continue.
    pause
    exit /b 1
)

call :CheckAndInstallPython

call :DownloadOIWiki

echo.
echo ============================================
echo   Setup complete.
echo   Directory: %SCRIPT_DIR%\OI-wiki
echo ============================================
pause
exit /b 0

REM ============================================================
REM Function: Check Git, auto-install if missing
REM ============================================================
:CheckAndInstallGit
echo [1/3] Checking Git...
where git >nul 2>&1
if %errorlevel% equ 0 (
    echo   Git found:
    git --version
    exit /b 0
)

echo   Git not found. Downloading installer...

for %%U in ("%GIT_DOWNLOAD_URLS:;=" "%") do (
    set "CURRENT_URL=%%~U"
    echo   Trying: !CURRENT_URL!
    powershell -Command "try { Invoke-WebRequest -Uri '!CURRENT_URL!' -OutFile '%TEMP_DIR%\git_installer.exe' -UseBasicParsing -TimeoutSec 60; exit 0 } catch { exit 1 }"
    if !errorlevel! equ 0 (
        echo   Download OK. Installing silently...
        start /wait "" "%TEMP_DIR%\git_installer.exe" /VERYSILENT /NORESTART /NOCANCEL /SP- /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS /COMPONENTS="icons,ext\reg\shellhere,assoc,assoc_sh" /PathOption=Cmd
        call :RefreshPath
        where git >nul 2>&1
        if !errorlevel! equ 0 (
            echo   Git installed:
            git --version
            exit /b 0
        ) else (
            if exist "C:\Program Files\Git\cmd\git.exe" (
                set "PATH=C:\Program Files\Git\cmd;!PATH!"
                echo   Git ready:
                git --version
                exit /b 0
            )
        )
    ) else (
        echo   Download failed. Trying next mirror...
    )
)

echo   [ERROR] All Git download sources failed.
exit /b 1

REM ============================================================
REM Function: Check Python, auto-install if missing
REM ============================================================
:CheckAndInstallPython
echo [2/3] Checking Python...
where python >nul 2>&1
if %errorlevel% equ 0 (
    echo   Python found:
    python --version
    exit /b 0
)
where python3 >nul 2>&1
if %errorlevel% equ 0 (
    echo   Python found (python3):
    python3 --version
    exit /b 0
)
where py >nul 2>&1
if %errorlevel% equ 0 (
    echo   Python found (py launcher):
    py --version
    exit /b 0
)

echo   Python not found. Downloading installer...

for %%U in ("%PYTHON_DOWNLOAD_URLS:;=" "%") do (
    set "CURRENT_URL=%%~U"
    echo   Trying: !CURRENT_URL!
    powershell -Command "try { Invoke-WebRequest -Uri '!CURRENT_URL!' -OutFile '%TEMP_DIR%\python_installer.exe' -UseBasicParsing -TimeoutSec 120; exit 0 } catch { exit 1 }"
    if !errorlevel! equ 0 (
        echo   Download OK. Installing silently...
        start /wait "" "%TEMP_DIR%\python_installer.exe" /quiet InstallAllUsers=1 PrependPath=1 Include_test=0 Include_pip=1
        call :RefreshPath
        where python >nul 2>&1
        if !errorlevel! equ 0 (
            echo   Python installed:
            python --version
            exit /b 0
        )
    ) else (
        echo   Download failed. Trying next mirror...
    )
)

echo   [WARNING] All Python download sources failed. OI Wiki can still be downloaded, but local preview will not work.
exit /b 0

REM ============================================================
REM Function: Multi-mirror clone OI Wiki
REM ============================================================
:DownloadOIWiki
echo [3/3] Downloading OI Wiki...

if exist "%SCRIPT_DIR%\OI-wiki\.git" (
    echo   OI-wiki already exists. Trying update...
    pushd "%SCRIPT_DIR%\OI-wiki"
    git pull
    popd
    exit /b 0
)

for %%U in ("%OIVIKI_GITHUB%" "%OIVIKI_GITEE%" "%OIVIKI_GITCLONE%" "%OIVIKI_CNPMJS%") do (
    set "CURRENT_URL=%%~U"
    echo   Trying clone: !CURRENT_URL!
    git clone --depth 1 "!CURRENT_URL!" "%SCRIPT_DIR%\OI-wiki" 2>nul
    if !errorlevel! equ 0 (
        echo   OI Wiki cloned successfully.
        goto :CloneSuccess
    ) else (
        echo   Clone failed. Trying next mirror...
        if exist "%SCRIPT_DIR%\OI-wiki" rmdir /s /q "%SCRIPT_DIR%\OI-wiki" 2>nul
    )
)

echo   [ERROR] All OI Wiki clone mirrors failed.
exit /b 1

:CloneSuccess
echo   Repository: %SCRIPT_DIR%\OI-wiki
exit /b 0

REM ============================================================
REM Function: Refresh PATH from registry
REM ============================================================
:RefreshPath
for /f "tokens=2*" %%A in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v Path 2^>nul') do set "SYS_PATH=%%B"
for /f "tokens=2*" %%A in ('reg query "HKCU\Environment" /v Path 2^>nul') do set "USR_PATH=%%B"
if defined SYS_PATH set "PATH=%SYS_PATH%"
if defined USR_PATH set "PATH=%PATH%;%USR_PATH%"
exit /b 0