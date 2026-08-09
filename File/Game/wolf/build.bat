@echo off
rem 切换到本脚本所在目录，保证源码与产物定位不依赖调用方的工作目录
cd /d "%~dp0"

echo ========================================
echo  Werewolf (狼人杀) - Build
echo ========================================
echo.

rem 先杀残留进程：exe 被占用时链接会报 LNK1168，必须提前清理
taskkill /f /im Start.exe >nul 2>&1
taskkill /f /im Server.exe >nul 2>&1
taskkill /f /im Client.exe >nul 2>&1
taskkill /f /im Client_en.exe >nul 2>&1

rem 按序探测 vcvars64.bat：不同机器安装路径不同，找到第一个就直接使用
if exist "D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    goto :env_ok
)
if exist "D:\Soft\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" (
    call "D:\Soft\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    goto :env_ok
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    goto :env_ok
)
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    goto :env_ok
)

rem 四个候选都找不到：后续编译必然失败，直接退出
echo 找不到 vcvars64.bat，请手动设置 MSVC 环境
exit /b 1

:env_ok
echo [1/4] 编译 Start（房间管理器）...
cl /nologo /utf-8 /MT /EHsc /D "_CRT_SECURE_NO_WARNINGS" Start.cpp /link ws2_32.lib /SUBSYSTEM:CONSOLE /OUT:Start.exe
if %errorlevel% neq 0 (
    echo [ERROR] Start 编译失败！
    exit /b %errorlevel%
)
echo [OK] Start.exe 已生成
echo.

echo [2/4] 编译 Server（游戏服务器）...
cl /nologo /utf-8 /MT /EHsc /D "_CRT_SECURE_NO_WARNINGS" Server.cpp /link ws2_32.lib /SUBSYSTEM:CONSOLE /OUT:Server.exe
if %errorlevel% neq 0 (
    echo [ERROR] Server 编译失败！
    exit /b %errorlevel%
)
echo [OK] Server.exe 已生成
echo.

echo [3/4] 编译 Client（中文版客户端）...
cl /nologo /utf-8 /MT /EHsc /D "_CRT_SECURE_NO_WARNINGS" Client.cpp /link ws2_32.lib user32.lib shell32.lib /SUBSYSTEM:CONSOLE /OUT:Client.exe
if %errorlevel% neq 0 (
    echo [ERROR] Client 编译失败！
    exit /b %errorlevel%
)
echo [OK] Client.exe 已生成
echo.

echo [4/4] 编译 Client_en（英文版客户端）...
cl /nologo /utf-8 /MT /EHsc /D "_CRT_SECURE_NO_WARNINGS" /D "WOLF_EN" Client.cpp /link ws2_32.lib user32.lib shell32.lib /SUBSYSTEM:CONSOLE /OUT:Client_en.exe
if %errorlevel% neq 0 (
    echo [ERROR] Client_en 编译失败！
    exit /b %errorlevel%
)
echo [OK] Client_en.exe 已生成
echo.

echo ========================================
echo  全部编译成功！
echo  Start.exe     - 房间管理器
echo  Server.exe    - 游戏服务器
echo  Client.exe    - 中文版客户端
echo  Client_en.exe - 英文版客户端
echo ========================================
