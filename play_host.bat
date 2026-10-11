@echo off
rem Magic Carpet (native port) - host a network game.
rem Start this first. The other player then runs play_join.bat and types one of the addresses shown below.
rem In the game: Multiplayer -> the lobby; the host chooses Conquest and the match settings.
setlocal
set "HERE=%~dp0"
set "GAME=%HERE%MagicCarpet\magic"
set "EXE=%HERE%build\Release\mcport.exe"
if not exist "%EXE%" echo mcport.exe not found: %EXE% & pause & exit /b 1
if not exist "%GAME%" echo game data not found: %GAME% & pause & exit /b 1

rem No address to connect to: this instance is the host and listens on port 30600 (MC_NET_PORT changes it).
set MC_NET_HOST=
echo This PC's addresses (the other player types one of them into play_join.bat):
ipconfig | findstr /c:"IPv4"
echo.
echo If Windows asks about the firewall, allow mcport on private networks.
echo Starting the game as host...
"%EXE%" "%GAME%" network
endlocal
