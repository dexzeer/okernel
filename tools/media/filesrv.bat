@echo off
rem Serves folders of this PC to okai (inside the emulator open http://10.0.2.2:8010/).
rem Usage: filesrv.bat [--port N] [folder ...]   (no folder: Videos, Music, Downloads and Desktop of this user)
where node >nul 2>nul
if errorlevel 1 (
    echo Node.js is required: https://nodejs.org
    pause
    exit /b 1
)
node "%~dp0filesrv.js" %*
pause
