@echo off
REM Build the hwdiag library from source.
REM This script copies internal sources, builds the library, then cleans up.
REM The resulting .lib is placed in lib/ and committed to the repo.
REM
REM Prerequisites:
REM   - Qt6 installed and findable by CMake
REM   - MSVC build tools on PATH (run from Developer Command Prompt)
REM   - Main project configured at least once (for EmbeddedKey.h)

setlocal

set SCRIPT_DIR=%~dp0
set INTERNAL=%SCRIPT_DIR%internal
set SRC_ROOT=%SCRIPT_DIR%..\..\src

echo === Copying internal sources ===
copy /Y "%SRC_ROOT%\ui\dialogs\AstroChicken.h"    "%INTERNAL%\" >nul
copy /Y "%SRC_ROOT%\ui\dialogs\AstroChicken.cpp"  "%INTERNAL%\" >nul
copy /Y "%SRC_ROOT%\ui\dialogs\Vohaul.h"          "%INTERNAL%\" >nul
copy /Y "%SRC_ROOT%\ui\dialogs\Vohaul.cpp"        "%INTERNAL%\" >nul
copy /Y "%SRC_ROOT%\ui\dialogs\Arnoid.h"          "%INTERNAL%\" >nul
copy /Y "%SRC_ROOT%\ui\dialogs\Arnoid.cpp"        "%INTERNAL%\" >nul
copy /Y "%SRC_ROOT%\ui\tabs\StarGenerator.h"      "%INTERNAL%\" >nul
copy /Y "%SRC_ROOT%\ui\tabs\StarGenerator.cpp"    "%INTERNAL%\" >nul
copy /Y "%SRC_ROOT%\core\security\OratDecoder.h"  "%INTERNAL%\" >nul
copy /Y "%SRC_ROOT%\core\security\OratDecoder.cpp" "%INTERNAL%\" >nul

echo === Building library ===
cmake -B "%SCRIPT_DIR%build" -S "%SCRIPT_DIR%" -G Ninja
cmake --build "%SCRIPT_DIR%build" --config Release

if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED
    goto cleanup
)

echo === Library built successfully ===
echo Output: %SCRIPT_DIR%lib\

:cleanup
echo === Cleaning up internal sources ===
del /f /q "%INTERNAL%\*.h"   >nul 2>&1
del /f /q "%INTERNAL%\*.cpp" >nul 2>&1
rmdir /s /q "%SCRIPT_DIR%build" >nul 2>&1

echo === Done ===
endlocal
