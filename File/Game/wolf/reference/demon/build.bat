@echo off
echo ========================================
echo  Demon Roulette - Build
echo ========================================
echo.

echo [1/3] Compiling Start (Room Manager) ...
cl /utf-8 /MT /EHsc /D "_CRT_SECURE_NO_WARNINGS" Start.cpp /link ws2_32.lib /SUBSYSTEM:CONSOLE /OUT:Start.exe
if %errorlevel% neq 0 ( echo [ERROR] Start compilation failed! & pause & exit /b %errorlevel% )
echo [OK] Start.exe generated
echo.

echo [2/3] Compiling Server (Game Server) ...
cl /utf-8 /MT /EHsc /D "_CRT_SECURE_NO_WARNINGS" Server.cpp /link ws2_32.lib /SUBSYSTEM:CONSOLE /OUT:Server.exe
if %errorlevel% neq 0 ( echo [ERROR] Server compilation failed! & pause & exit /b %errorlevel% )
echo [OK] Server.exe generated
echo.

echo [3/3] Compiling Client ...
cl /utf-8 /MT /EHsc /D "_CRT_SECURE_NO_WARNINGS" Client.cpp /link ws2_32.lib user32.lib /SUBSYSTEM:CONSOLE /OUT:Client.exe
if %errorlevel% neq 0 ( echo [ERROR] Client compilation failed! & pause & exit /b %errorlevel% )
echo [OK] Client.exe generated
echo.

echo ========================================
echo  Build completed successfully!
echo  Start.exe - Room Manager
echo  Server.exe - Game Server
echo  Client.exe - Client
echo ========================================
pause