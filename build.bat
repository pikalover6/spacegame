@echo off
setlocal enabledelayedexpansion

REM Default Raylib root. Can be overridden by passing an argument.
set "RAYLIB_ROOT=C:\raylib\raylib\src"
if not "%~1"=="" (
    set "RAYLIB_ROOT=%~1"
)

REM Stop on errors
set "ERRORLEVEL=0"

REM Create output directory
if not exist ".\bin" (
    mkdir ".\bin"
)

REM Compile server (winsock)
gcc .\server\server.c .\server\toy_term.c ^
    -o .\bin\server.exe ^
    -I.\common -I.\server ^
    -lws2_32 -lm -std=c99

if errorlevel 1 goto :error

REM Compile client (raylib)
gcc .\client\client.c .\client\net.c .\client\terminal_ui.c .\client\psx_shader.c ^
    -o .\bin\client.exe ^
    -I.\common -I.\client ^
    -I"%RAYLIB_ROOT%" -L"%RAYLIB_ROOT%" ^
    -lraylib -lopengl32 -lgdi32 -lwinmm -lws2_32 ^
    -std=c99

if errorlevel 1 goto :error

echo Built bin\server.exe and bin\client.exe
goto :eof

:error
echo Build failed.
exit /b 1
