@echo off
rem build.bat - configure and build ld-decode-tools into .\build
rem
rem Usage: build.bat [Release|Debug|RelWithDebInfo|MinSizeRel] [extra cmake arguments...]
rem   The build type defaults to Release. Everything else is passed to the
rem   cmake configure step.
rem
rem Dependencies come from a project-local vcpkg, as in decode-orc:
rem   - vcpkg is cloned into .\vcpkg and bootstrapped on first use (a real git
rem     clone, since vcpkg resolves vcpkg.json's builtin-baseline from history)
rem   - vcpkg.json lists FFTW3 and pkgconf, plus Qt 6 under the "qt" feature;
rem     vcpkg installs them into build\vcpkg_installed during configure.
rem     The first run builds Qt from source and takes a long time.
rem   - the ezpwd Reed-Solomon headers are cloned into external\ at the
rem     revision flake.lock pins
rem
rem Compiler: MSVC when cl.exe is on PATH (run from an "x64 Native Tools
rem Command Prompt for VS"), otherwise MinGW-w64 gcc from PATH.
rem
rem Environment overrides:
rem   VCPKG_TARGET_TRIPLET  vcpkg triplet (default: x64-windows with MSVC,
rem                         x64-mingw-dynamic with MinGW)
rem   USE_SYSTEM_QT=1       do not build Qt with vcpkg; point CMake at an
rem                         installed Qt with -DCMAKE_PREFIX_PATH=...
rem
rem Requires CMake and Git; Ninja is used when available.
rem
rem SPDX-License-Identifier: GPL-3.0-or-later
rem SPDX-FileCopyrightText: 2026 ld-decode-tools contributors

setlocal EnableDelayedExpansion

cd /d "%~dp0"

rem The first argument is the build type only when it names one; everything
rem else is passed to cmake configure verbatim. The raw argument string is
rem reused rather than re-tokenised, because batch splits on "=" and ";" and would
rem break values such as -DCMAKE_PREFIX_PATH=a;b.
set "BUILD_TYPE=Release"
set "EXTRA_ARGS=%*"
for %%T in (Release Debug RelWithDebInfo MinSizeRel) do (
    if /i "%~1"=="%%T" (
        set "BUILD_TYPE=%%T"
        set "EXTRA_ARGS=!EXTRA_ARGS:*%~1=!"
    )
)

rem --- ezpwd-reed-solomon headers ---------------------------------------------
set "EZPWD_REV=62a490c13f6e057fbf2dc6777fde234c7a19098e"
set "EZPWD_LOCAL=%CD%\external\ezpwd-reed-solomon"

if "%EZPWD_DIR%"=="" (
    if not exist "%EZPWD_LOCAL%\c++\ezpwd\rs_base" (
        echo Fetching ezpwd-reed-solomon headers into external\ ...
        if exist "%EZPWD_LOCAL%" rmdir /s /q "%EZPWD_LOCAL%"
        git clone --quiet https://github.com/pjkundert/ezpwd-reed-solomon "%EZPWD_LOCAL%" || goto :error
        git -C "%EZPWD_LOCAL%" checkout --quiet %EZPWD_REV% || goto :error
    )
    set "EZPWD_DIR=%EZPWD_LOCAL%\c++"
)

rem --- project-local vcpkg -----------------------------------------------------
set "VCPKG_DIR=%CD%\vcpkg"

if not exist "%VCPKG_DIR%\.git" (
    echo Cloning vcpkg into vcpkg\ ...
    if exist "%VCPKG_DIR%" rmdir /s /q "%VCPKG_DIR%"
    git clone https://github.com/microsoft/vcpkg.git "%VCPKG_DIR%" || goto :error
)
if not exist "%VCPKG_DIR%\vcpkg.exe" (
    call "%VCPKG_DIR%\bootstrap-vcpkg.bat" -disableMetrics || goto :error
)

rem --- compiler and triplet ----------------------------------------------------
set "USE_MSVC="
where cl >nul 2>&1 && set "USE_MSVC=1"

set "TRIPLET=%VCPKG_TARGET_TRIPLET%"
if "%TRIPLET%"=="" (
    if defined USE_MSVC (
        set "TRIPLET=x64-windows"
    ) else (
        set "TRIPLET=x64-mingw-dynamic"
    )
)

rem MinGW has no MSVC to build vcpkg's host tools with, so the host triplet
rem follows the target one.
set "HOST_TRIPLET_ARG="
if not defined USE_MSVC set "HOST_TRIPLET_ARG=-DVCPKG_HOST_TRIPLET=%TRIPLET%"

set "FEATURES_ARG=-DVCPKG_MANIFEST_FEATURES=qt"
if "%USE_SYSTEM_QT%"=="1" set "FEATURES_ARG="

rem --- generator ---------------------------------------------------------------
rem Ninja when available (single-config, honours CMAKE_BUILD_TYPE); otherwise
rem MinGW Makefiles for gcc, or CMake's default Visual Studio generator.
set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"
if not defined GENERATOR if not defined USE_MSVC set "GENERATOR=-G "MinGW Makefiles""

rem FFTW is found through pkg-config. Manifest mode installs into the build
rem tree during configure, before pkg_check_modules runs, so the pkgconf it
rem provides can be named up front.
set "PKGCONF=%CD%\build\vcpkg_installed\%TRIPLET%\tools\pkgconf\pkgconf.exe"

echo.
echo Build type: %BUILD_TYPE%   vcpkg triplet: %TRIPLET%
echo.

cmake -S . -B build %GENERATOR% ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_DIR%\scripts\buildsystems\vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=%TRIPLET% ^
    %HOST_TRIPLET_ARG% ^
    %FEATURES_ARG% ^
    "-DPKG_CONFIG_EXECUTABLE=%PKGCONF%" ^
    "-DEZPWD_DIR=%EZPWD_DIR%" ^
    %EXTRA_ARGS% || goto :error

cmake --build build --config %BUILD_TYPE% --parallel || goto :error

rem --- run from the build tree -------------------------------------------------
rem Point the tools at vcpkg's Qt plugins (platforms, sqldrivers, iconengines)
rem so ld-analyse starts straight from build\bin.
set "QT_PLUGINS=%CD%\build\vcpkg_installed\%TRIPLET%\Qt6\plugins"
if /i "%BUILD_TYPE%"=="Debug" set "QT_PLUGINS=%CD%\build\vcpkg_installed\%TRIPLET%\debug\Qt6\plugins"
if exist "%QT_PLUGINS%" (
    > "build\bin\qt.conf" (
        echo [Paths]
        echo Plugins=!QT_PLUGINS:\=/!
    )
)

set "VCPKG_BIN=%CD%\build\vcpkg_installed\%TRIPLET%\bin"
if /i "%BUILD_TYPE%"=="Debug" set "VCPKG_BIN=%CD%\build\vcpkg_installed\%TRIPLET%\debug\bin"

echo.
echo Build complete: binaries are in build\bin
echo If a tool reports a missing DLL, put the vcpkg runtime first on PATH:
echo   set "PATH=%VCPKG_BIN%;%%PATH%%"
exit /b 0

:error
echo.
echo Build failed.
exit /b 1
