@echo off
setlocal EnableDelayedExpansion

:: ============================================================
:: Setec Partition Wizard — Build Script
:: Manually sets MSVC x64 environment (no vcvars dependency)
:: ============================================================

set "MSVC_VER=14.44.35207"
set "WINSDK_VER=10.0.26100.0"

set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Professional"
set "VCDIR=%VSDIR%\VC\Tools\MSVC\%MSVC_VER%"
set "SDKDIR=C:\Program Files (x86)\Windows Kits\10"

:: ---- PATH ----
set "PATH=%VCDIR%\bin\Hostx64\x64"
set "PATH=%PATH%;%SDKDIR%\bin\%WINSDK_VER%\x64"
set "PATH=%PATH%;C:\Qt\Tools\Ninja"
set "PATH=%PATH%;C:\Program Files\CMake\bin"
set "PATH=%PATH%;C:\Qt\6.10.0\msvc2022_64\bin"
set "PATH=%PATH%;C:\Windows\System32;C:\Windows"
set "PATH=%PATH%;C:\Program Files\Git\cmd"
set "PATH=%PATH%;C:\Program Files\Git\usr\bin"

:: ---- INCLUDE ----
set "INCLUDE=%VCDIR%\include"
set "INCLUDE=%INCLUDE%;%SDKDIR%\Include\%WINSDK_VER%\ucrt"
set "INCLUDE=%INCLUDE%;%SDKDIR%\Include\%WINSDK_VER%\um"
set "INCLUDE=%INCLUDE%;%SDKDIR%\Include\%WINSDK_VER%\shared"
set "INCLUDE=%INCLUDE%;%SDKDIR%\Include\%WINSDK_VER%\winrt"
set "INCLUDE=%INCLUDE%;%SDKDIR%\Include\%WINSDK_VER%\cppwinrt"

:: ---- LIB ----
set "LIB=%VCDIR%\lib\x64"
set "LIB=%LIB%;%SDKDIR%\Lib\%WINSDK_VER%\ucrt\x64"
set "LIB=%LIB%;%SDKDIR%\Lib\%WINSDK_VER%\um\x64"

:: ---- Other vars MSVC needs ----
set "LIBPATH=%VCDIR%\lib\x64"
set "Platform=x64"
set "VisualStudioVersion=17.0"
set "VSCMD_ARG_HOST_ARCH=x64"
set "VSCMD_ARG_TGT_ARCH=x64"

:: ---- Verify compiler ----
echo === Setec Partition Wizard Build ===
cl /? >nul 2>&1
if errorlevel 1 (
    echo ERROR: cl.exe not found. Check MSVC_VER.
    exit /b 1
)
echo Compiler: cl.exe OK
echo.

:: ---- Change to project dir ----
cd /d "%~dp0"

:: ---- Parse arguments ----
set "PRESET=default"
set "ACTION=all"
if not "%1"=="" set "PRESET=%1"
if not "%2"=="" set "ACTION=%2"

if "%ACTION%"=="configure" goto :configure
if "%ACTION%"=="build" goto :build
if "%ACTION%"=="all" goto :all
if "%ACTION%"=="clean" goto :clean

echo Usage: build.bat [preset] [configure^|build^|all^|clean]
exit /b 1

:all
:configure
echo --- CMake Configure (preset: %PRESET%) ---
cmake --preset %PRESET%
if errorlevel 1 (
    echo.
    echo CONFIGURE FAILED
    exit /b 1
)
echo.
if "%ACTION%"=="configure" goto :done

:build
echo --- CMake Build (preset: %PRESET%) ---
cmake --build --preset %PRESET%
if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)
echo.
goto :done

:clean
echo --- Clean (preset: %PRESET%) ---
if exist "build\%PRESET%" rmdir /s /q "build\%PRESET%"
echo Cleaned build\%PRESET%
goto :done

:done
echo === Done ===
exit /b 0
