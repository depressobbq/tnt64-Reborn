@echo off
rem Launcher for The New Tetris: Reborn (Windows)
setlocal
cd /d "%~dp0"

if not exist "TNTRecompiled.exe" (
    if exist "TNTRecomp\out\build\windows\TNTRecompiled.exe" (
        cd TNTRecomp\out\build\windows
    )
)

if not exist "TNTRecompiled.exe" (
    echo [!] TNTRecompiled.exe not found!
    echo Please build the game or extract the release zip.
    pause
    exit /b 1
)

if not exist "baserom.us.z64" if not exist "tnt_data\tnt.n64.us.1.0.z64" (
    echo [!] Notice: 'baserom.us.z64' was not found in this folder.
    echo If this is your first time playing, please copy your USA 1.0 ROM here as 'baserom.us.z64'.
    echo.
)

if exist "baserom.us.z64" (
    start "" TNTRecompiled.exe baserom.us.z64 %*
) else (
    start "" TNTRecompiled.exe %*
)
