@echo off
rem Magic Carpet (native port) - join a network game hosted with play_host.bat.
rem Usage: play_join.bat [host address]     e.g. play_join.bat 192.168.1.20
rem Both PCs need the same mcport.exe and their own copy of the game data.
setlocal
set "HERE=%~dp0"
set "GAME=%HERE%MagicCarpet\magic"
set "EXE=%HERE%build\Release\mcport.exe"
if not exist "%EXE%" echo mcport.exe not found: %EXE% & pause & exit /b 1
if not exist "%GAME%" echo game data not found: %GAME% & pause & exit /b 1

set "HOST=%~1"
if "%HOST%"=="" set /p HOST=Host address (shown in the host's window, e.g. 192.168.1.20):
if "%HOST%"=="" echo No address given. & pause & exit /b 1

set "MC_NET_HOST=%HOST%"
echo Joining %HOST% (port 30600)...
echo If Windows asks about the firewall, allow mcport on private networks.
"%EXE%" "%GAME%" network
endlocal
