@echo off
REM ============================================================
REM  Lexara 1.12 - build x86.
REM  Toolset: VS 2022 BuildTools (Community bez toolsetu nie wystarczy).
REM  CMake: z PATH, a jak go tam nie ma - ze zmiennej LEXARA_CMAKE.
REM ============================================================
setlocal

set "CMAKE="
where cmake >nul 2>&1 && set "CMAKE=cmake"

if not defined CMAKE if defined LEXARA_CMAKE if exist "%LEXARA_CMAKE%" set "CMAKE=%LEXARA_CMAKE%"

if not defined CMAKE (
    echo [ERROR] brak cmake w PATH i brak poprawnego LEXARA_CMAKE.
    echo         zainstaluj: python -m pip install --user cmake
    echo         albo:       set LEXARA_CMAKE=C:\sciezka\do\cmake.exe
    exit /b 1
)

cd /d "%~dp0"
if not exist build mkdir build

"%CMAKE%" -S . -B build -G "Visual Studio 17 2022" -A Win32
if errorlevel 1 (
    echo [ERROR] konfiguracja nie powiodla sie
    exit /b 1
)

"%CMAKE%" --build build --config Release
if errorlevel 1 (
    echo [ERROR] budowanie nie powiodlo sie
    exit /b 1
)

echo.
echo [OK] build\out\Release\lexara112.dll
echo      Kopiowac do katalogu klienta i dopisac do dlls.txt.
