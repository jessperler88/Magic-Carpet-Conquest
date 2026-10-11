@echo off
rem Magic Carpet (native port) - a network game against yourself: two windows on this PC.
rem The first window is the host, the second joins it over 127.0.0.1 and keeps its own settings and saves in
rem a separate folder so the two do not write the same files. Go to Multiplayer in both.
setlocal
set "HERE=%~dp0"
set "GAME=%HERE%MagicCarpet\magic"
set "EXE=%HERE%build\Release\mcport.exe"
if not exist "%EXE%" echo mcport.exe not found: %EXE% & pause & exit /b 1
if not exist "%GAME%" echo game data not found: %GAME% & pause & exit /b 1

rem loopback only: no firewall prompt for this test
set MC_NET_BIND=127.0.0.1
set MC_NET_HOST=
start "Magic Carpet - host" "%EXE%" "%GAME%" network
timeout /t 3 /nobreak >nul
set "MC_NET_HOST=127.0.0.1"
set "MC_SAVE_DIR=%TEMP%\mcport_second_player\"
if not exist "%MC_SAVE_DIR%" mkdir "%MC_SAVE_DIR%"
rem A new settings folder starts with the game's first-run language screen: give it the first player's language
rem choice and options so the second window goes straight to the title screens like the first.
set "MAIN=%APPDATA%\Bullfrog\MagicCarpet\"
if not exist "%MC_SAVE_DIR%language.inf" if exist "%MAIN%language.inf" copy /y "%MAIN%language.inf" "%MC_SAVE_DIR%" >nul
if not exist "%MC_SAVE_DIR%intro.pld" if exist "%MAIN%intro.pld" copy /y "%MAIN%intro.pld" "%MC_SAVE_DIR%" >nul
if not exist "%MC_SAVE_DIR%mcport.ini" if exist "%MAIN%mcport.ini" copy /y "%MAIN%mcport.ini" "%MC_SAVE_DIR%" >nul
start "Magic Carpet - second player" "%EXE%" "%GAME%" network
endlocal
