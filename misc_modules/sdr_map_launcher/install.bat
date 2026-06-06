@echo off
REM install.bat - one-shot installer for the SDR Map Launcher module (Windows)
REM
REM 1) Copies the bundled Django project to %LOCALAPPDATA%\sdr_map_launcher\sdr_map
REM 2) Installs Python dependencies for the system 'py' / 'python.exe'
REM 3) (Optional) Builds and installs the SDR++ module if a source path is given
REM
REM Usage:
REM   install.bat                              -- only steps 1 & 2
REM   install.bat C:\path\to\SDRPlusPlus       -- also builds the module

setlocal enabledelayedexpansion

set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
set "DATA_DIR=%LOCALAPPDATA%\sdr_map_launcher"
set "PROJECT_DST=%DATA_DIR%\sdr_map"
set "SDRPP_DIR=%~1"

echo ==^> SDR Map Launcher - installer
echo     Bundle path : %HERE%
echo     Data dir    : %DATA_DIR%
echo.

REM --------------------------------------------------------- 1) Django
echo [1/3] Installing the Django project...
if not exist "%DATA_DIR%" mkdir "%DATA_DIR%"
if exist "%PROJECT_DST%" (
    echo     Existing project found - backing it up to %PROJECT_DST%.bak
    if exist "%PROJECT_DST%.bak" rmdir /s /q "%PROJECT_DST%.bak"
    move /y "%PROJECT_DST%" "%PROJECT_DST%.bak" >nul
)
xcopy /e /i /q "%HERE%\sdr_map" "%PROJECT_DST%" >nul
echo     OK Django project copied
if exist "%PROJECT_DST%.bak\db.sqlite3" (
    copy /y "%PROJECT_DST%.bak\db.sqlite3" "%PROJECT_DST%\db.sqlite3" >nul
    echo     OK Previous db.sqlite3 carried over
)

REM --------------------------------------------------------- 2) Python deps
echo.
echo [2/3] Installing Python dependencies...
set "PY=py -3"
%PY% --version >nul 2>&1 || set "PY=python"
%PY% --version >nul 2>&1
if errorlevel 1 (
    echo     ! Python not found. Install it from python.org first.
    exit /b 1
)
%PY% -c "import django, channels, daphne, whitenoise" >nul 2>&1
if not errorlevel 1 (
    echo     OK Dependencies already present
) else (
    %PY% -m pip install -r "%PROJECT_DST%\requirements.txt"
    if errorlevel 1 (
        echo     ! pip install failed. Try manually:
        echo       %PY% -m pip install -r "%PROJECT_DST%\requirements.txt"
        exit /b 1
    )
    echo     OK Installed
)

REM --------------------------------------------------------- 3) SDR++ module
if "%SDRPP_DIR%"=="" (
    echo.
    echo [3/3] Skipped ^(no SDR++ source path given^).
    echo.
    echo To build the module later:
    echo   install.bat C:\path\to\SDRPlusPlus
    echo.
    echo You can already test the Django side directly:
    echo   cd "%PROJECT_DST%"
    echo   %PY% manage.py migrate
    echo   %PY% manage.py runserver_sdr
    goto :end
)

if not exist "%SDRPP_DIR%\CMakeLists.txt" (
    echo     ! %SDRPP_DIR% does not look like the SDR++ source tree.
    exit /b 1
)

echo.
echo [3/3] Building the SDR++ module...
echo     (Run apply_to_sdrpp.sh under Git Bash or WSL to register the module,)
echo     (then configure with CMake and build with cmake --build.        )
echo     Module sources are in: %HERE%
echo     Copy them into:        %SDRPP_DIR%\misc_modules\sdr_map_launcher
echo     Then in %SDRPP_DIR%\CMakeLists.txt add:
echo       option(OPT_BUILD_SDR_MAP_LAUNCHER "..." OFF)
echo       if (OPT_BUILD_SDR_MAP_LAUNCHER)
echo       add_subdirectory("misc_modules/sdr_map_launcher")
echo       endif (OPT_BUILD_SDR_MAP_LAUNCHER)
echo     Then:
echo       cd %SDRPP_DIR%
echo       mkdir build ^& cd build
echo       cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_SDR_MAP_LAUNCHER=ON
echo       cmake --build . --config Release

:end
echo.
echo Done.
echo   Project Django: %PROJECT_DST%
echo.
echo In SDR++: Module Manager -^> pick 'sdr_map_launcher' -^> add an instance,
echo then click 'Start server'. The default Project dir is already correct.
endlocal
