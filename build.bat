@echo off
REM ============================================================
REM  Lexara 1.12 - x86 build.
REM  Toolset: VS 2022 BuildTools (Community without the toolset is not enough).
REM  CMake: taken from PATH, or from the LEXARA_CMAKE variable if not there.
REM ============================================================
setlocal

set "CMAKE="
where cmake >nul 2>&1 && set "CMAKE=cmake"

if not defined CMAKE if defined LEXARA_CMAKE if exist "%LEXARA_CMAKE%" set "CMAKE=%LEXARA_CMAKE%"

if not defined CMAKE (
    echo [ERROR] no cmake in PATH and no valid LEXARA_CMAKE.
    echo         install it: python -m pip install --user cmake
    echo         or set:     set LEXARA_CMAKE=C:\path\to\cmake.exe
    exit /b 1
)

cd /d "%~dp0"
if not exist build mkdir build

"%CMAKE%" -S . -B build -G "Visual Studio 17 2022" -A Win32
if errorlevel 1 (
    echo [ERROR] configuration failed
    exit /b 1
)

"%CMAKE%" --build build --config Release
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

echo.
echo [OK] build\out\Release\lexara112.dll
echo      Copy it into the client directory and add it to dlls.txt.
