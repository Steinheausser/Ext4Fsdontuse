@echo off
rem Builds ext4bench.exe with MSVC (x64) and removes the .obj afterwards.
setlocal
if not defined VCVARS64 set "VCVARS64=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
where cl >nul 2>nul
if errorlevel 1 (
    call "%VCVARS64%" >nul 2>nul
    if errorlevel 1 (
        echo build.cmd: failed to run vcvars64.bat ^(set VCVARS64 to override^)
        exit /b 1
    )
)
cd /d "%~dp0"
cl /nologo /O2 /W4 /WX ext4bench.c
set "RC=%ERRORLEVEL%"
if exist ext4bench.obj del /q ext4bench.obj
exit /b %RC%
