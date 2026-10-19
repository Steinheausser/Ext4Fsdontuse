@echo off
rem Guest-side launcher for crashgen (run by a scheduled task at logon).
rem Copy crashgen.exe and this file to C:\crash\ in the guest.
rem Edit VOL (ext4 volume letter) if needed.
setlocal
set VOL=E:
set ROOT=%VOL%\crash
set LOGDIR=C:\crashlog

if not exist %LOGDIR% mkdir %LOGDIR%

rem wait (max ~2 min) for the ext4 volume to appear
set /a TRIES=0
:wait
if exist %VOL%\ goto ready
set /a TRIES+=1
if %TRIES% GEQ 60 goto ready
ping -n 3 127.0.0.1 >nul
goto wait
:ready

rem random seed from %RANDOM% (0..32767 twice)
set /a SEED=%RANDOM%*32768+%RANDOM%
echo %date% %time% seed=%SEED% >> %LOGDIR%\launcher.txt

C:\crash\crashgen.exe --root %ROOT% --log %LOGDIR%\run.log --seed %SEED% --max-files 300 --max-size 2M --flush-prob 0.2 >> %LOGDIR%\stdout.txt 2>&1
