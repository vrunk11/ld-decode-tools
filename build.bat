@echo off
rem build.bat - configure and build ld-decode-tools into .\build
rem
rem Usage: build.bat [Release|Debug|RelWithDebInfo] [extra cmake arguments...]
rem   The build type defaults to Release. Anything after it is passed to the
rem   cmake configure step, for example:
rem     build.bat Release -DCMAKE_PREFIX_PATH=C:/Qt/6.9.0/msvc2022_64;C:/vcpkg/installed/x64-windows
rem
rem Requires CMake, Git, Qt 6, FFTW3 and pkg-config (e.g. vcpkg fftw3 + pkgconf).
rem Run it from a "x64 Native Tools Command Prompt for VS" so MSVC is on PATH.
rem The ezpwd Reed-Solomon headers are cloned into external\ on first use at
rem the revision flake.lock pins.
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

rem Ninja when available (single-config, honours CMAKE_BUILD_TYPE); otherwise
rem CMake's default Visual Studio generator.
set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"

cmake -S . -B build %GENERATOR% -DCMAKE_BUILD_TYPE=%BUILD_TYPE% "-DEZPWD_DIR=%EZPWD_DIR%" %EXTRA_ARGS% || goto :error
cmake --build build --config %BUILD_TYPE% --parallel || goto :error

echo.
echo Build complete: binaries are in build\bin
exit /b 0

:error
echo.
echo Build failed.
exit /b 1
