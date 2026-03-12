@echo off
setlocal EnableDelayedExpansion

set "MSVC_VER=14.44.35207"
set "WINSDK_VER=10.0.26100.0"
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Professional"
set "VCDIR=%VSDIR%\VC\Tools\MSVC\%MSVC_VER%"
set "SDKDIR=C:\Program Files (x86)\Windows Kits\10"

set "PATH=%VCDIR%\bin\Hostx64\x64;%SDKDIR%\bin\%WINSDK_VER%\x64;C:\Qt\Tools\Ninja;C:\Program Files\CMake\bin;C:\Qt\6.10.0\msvc2022_64\bin;C:\Windows\System32;C:\Windows;C:\Program Files\Git\cmd"
set "INCLUDE=%VCDIR%\include;%SDKDIR%\Include\%WINSDK_VER%\ucrt;%SDKDIR%\Include\%WINSDK_VER%\um;%SDKDIR%\Include\%WINSDK_VER%\shared;%SDKDIR%\Include\%WINSDK_VER%\winrt"
set "LIB=%VCDIR%\lib\x64;%SDKDIR%\Lib\%WINSDK_VER%\ucrt\x64;%SDKDIR%\Lib\%WINSDK_VER%\um\x64"

set "HWDIAG_DIR=%~dp0third_party\hwdiag"
set "INTERNAL=%HWDIAG_DIR%\internal"
set "SRC_ROOT=%~dp0src"

echo === Copying internal sources ===
copy /Y "%SRC_ROOT%\ui\dialogs\AstroChicken.h"     "%INTERNAL%\" >nul 2>&1
copy /Y "%SRC_ROOT%\ui\dialogs\AstroChicken.cpp"   "%INTERNAL%\" >nul 2>&1
copy /Y "%SRC_ROOT%\ui\dialogs\Vohaul.h"           "%INTERNAL%\" >nul 2>&1
copy /Y "%SRC_ROOT%\ui\dialogs\Vohaul.cpp"         "%INTERNAL%\" >nul 2>&1
copy /Y "%SRC_ROOT%\ui\dialogs\Arnoid.h"           "%INTERNAL%\" >nul 2>&1
copy /Y "%SRC_ROOT%\ui\dialogs\Arnoid.cpp"         "%INTERNAL%\" >nul 2>&1
copy /Y "%SRC_ROOT%\ui\tabs\StarGenerator.h"       "%INTERNAL%\" >nul 2>&1
copy /Y "%SRC_ROOT%\ui\tabs\StarGenerator.cpp"     "%INTERNAL%\" >nul 2>&1
copy /Y "%SRC_ROOT%\core\security\OratDecoder.h"   "%INTERNAL%\" >nul 2>&1
copy /Y "%SRC_ROOT%\core\security\OratDecoder.cpp" "%INTERNAL%\" >nul 2>&1

echo === Configuring hwdiag (Release) ===
cmake -B "%HWDIAG_DIR%\build_rel" -S "%HWDIAG_DIR%" ^
    -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH=C:\Qt\6.10.0\msvc2022_64 ^
    -DCMAKE_MAKE_PROGRAM=C:\Qt\Tools\Ninja\ninja.exe

if %ERRORLEVEL% NEQ 0 (
    echo CONFIGURE FAILED
    goto cleanup
)

echo === Building hwdiag (Release) ===
cmake --build "%HWDIAG_DIR%\build_rel"

if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED
    goto cleanup
)

echo === hwdiag Release library built successfully ===
echo Output: %HWDIAG_DIR%\lib\spw_hwdiag.lib

:cleanup
echo === Cleaning up internal sources ===
del /f /q "%INTERNAL%\*.h"   >nul 2>&1
del /f /q "%INTERNAL%\*.cpp" >nul 2>&1
rmdir /s /q "%HWDIAG_DIR%\build_rel" >nul 2>&1

echo === Done ===
endlocal
