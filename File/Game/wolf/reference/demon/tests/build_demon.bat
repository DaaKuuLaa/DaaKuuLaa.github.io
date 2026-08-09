@echo off
if exist "D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else (
    call "D:\Soft\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)
cl /nologo /utf-8 /MT /EHsc /D "_CRT_SECURE_NO_WARNINGS" Start.cpp /link ws2_32.lib /SUBSYSTEM:CONSOLE /OUT:Start.exe
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /MT /EHsc /D "_CRT_SECURE_NO_WARNINGS" Server.cpp /link ws2_32.lib shell32.lib /SUBSYSTEM:CONSOLE /OUT:Server.exe
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /MT /EHsc /D "_CRT_SECURE_NO_WARNINGS" Client.cpp /link ws2_32.lib user32.lib /SUBSYSTEM:CONSOLE /OUT:Client.exe
if errorlevel 1 exit /b 1
echo ALL THREE BUILT OK
