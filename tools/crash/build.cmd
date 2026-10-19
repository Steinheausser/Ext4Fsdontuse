@echo off
rem Build crashgen.exe (guest-side generator). Requires Visual Studio C++ tools.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /W4 /WX crashgen.c bcrypt.lib
set RC=%ERRORLEVEL%
if exist crashgen.obj del /q crashgen.obj
exit /b %RC%
