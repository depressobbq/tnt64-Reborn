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
    echo Please make sure all files from the release zip are extracted.
    pause
    exit /b 1
)

if exist "baserom.us.z64" (
    start "" TNTRecompiled.exe baserom.us.z64 %*
) else (
    start "" TNTRecompiled.exe %*
)
