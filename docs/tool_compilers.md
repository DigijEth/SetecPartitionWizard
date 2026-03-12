# SetecPartitionWizard -- Tool & Compiler Inventory

> System scan performed **2026-03-11** on `mdavi` / Windows 10 (build 26200).

## Quick Status

| Tool | Status | Version | Location |
|------|--------|---------|----------|
| MSVC (cl.exe) | FOUND | 14.44.35207 | `C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\` |
| VS Build Tools | FOUND | 17.14.14 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\` |
| Windows SDK | FOUND | 10.0.26100.0 | `C:\Program Files (x86)\Windows Kits\10\` |
| CMake | FOUND | (standalone) | `C:\Program Files\CMake\bin\cmake.exe` |
| CMake (Qt) | FOUND | (Qt-bundled) | `C:\Qt\Tools\CMake_64\bin\cmake.exe` |
| CMake (VS) | FOUND | (VS-bundled) | `...\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |
| Ninja | FOUND | (Qt-bundled) | `C:\Qt\Tools\Ninja\ninja.exe` |
| Qt 6.10.0 (MSVC) | FOUND | 6.10.0 | `C:\Qt\6.10.0\msvc2022_64\` |
| Qt 6.10.0 (MinGW) | FOUND | 6.10.0 | `C:\Qt\6.10.0\mingw_64\` |
| Qt 6.10.0 (llvm-mingw) | FOUND | 6.10.0 | `C:\Qt\6.10.0\llvm-mingw_64\` |
| Qt 6.10.0 (ARM64) | FOUND | 6.10.0 | `C:\Qt\6.10.0\msvc2022_arm64\` |
| Qt 6.9.2 (MSVC) | FOUND | 6.9.2 | `C:\Qt\6.9.2\msvc2022_64\` |
| Clang (Qt llvm-mingw) | FOUND | 17.x | `C:\Qt\Tools\llvm-mingw1706_64\bin\clang.exe` |
| Clang (standalone LLVM) | NOT FOUND | -- | Expected at `C:\Program Files\LLVM\bin\` |
| clang-cl.exe | NOT FOUND | -- | Not in standalone LLVM or VS LLVM toolset |
| lld-link.exe | NOT FOUND | -- | Not found anywhere |
| GCC (w64devkit) | FOUND | 15.2.0 | `C:\w64devkit\bin\gcc.exe` |
| make (w64devkit) | FOUND | -- | `C:\w64devkit\bin\make.exe` |
| nmake | FOUND | -- | `...\MSVC\14.44.35207\bin\Hostx64\x64\nmake.exe` |
| Python 3.14 | FOUND | 3.14.0rc2 | `C:\Python314\python.exe` |
| Python 3.13 | FOUND | 3.13.7 | `C:\Users\mdavi\AppData\Local\Programs\Python\Python313\python.exe` |
| Git | FOUND | 2.53.0 | `C:\Program Files\Git\cmd\git.exe` |
| Go | FOUND | -- | `C:\Program Files\Go\bin\go.exe` |
| CUDA | FOUND | v13.1 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\` |
| pkg-config | NOT FOUND | -- | Not installed |
| Chocolatey | FOUND | -- | `C:\ProgramData\chocolatey\` |
| GitHub CLI | FOUND | -- | `C:\Program Files\GitHub CLI\` |
| WinGet LLVM-MinGW | FOUND | 20260311 | `...\WinGet\Packages\...\llvm-mingw-20260311-ucrt-x86_64\bin\` |

---

## Detailed Notes Per Tool

### 1. MSVC / Visual Studio

**Status:** FOUND -- two installations detected.

| Installation | Edition | Version | Path |
|---|---|---|---|
| VS 2022 Professional | Professional | 17.14.14 (toolset 14.44.35207) | `C:\Program Files\Microsoft Visual Studio\2022\Professional\` |
| VS 2022 Build Tools | Build Tools | 17.14.14 (toolset 14.44.35207) | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\` |

**Key files:**
- `cl.exe`: `C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe`
- `link.exe`: same directory
- `nmake.exe`: same directory
- `vcvarsall.bat`: `C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat`
- `vcvars64.bat`: `C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat`

**Cannot be CLI-installed.** Use the Visual Studio Installer:
1. Run `"C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe"`
2. Or download from: https://visualstudio.microsoft.com/downloads/
3. Required workloads:
   - "Desktop development with C++"
   - Individual components: "MSVC v143 - VS 2022 C++ x64/x86 build tools (Latest)"
   - Individual components: "C++ CMake tools for Windows"
   - Individual components: "Windows 10/11 SDK (10.0.26100.0)"

### 2. Windows SDK

**Status:** FOUND -- version 10.0.26100.0

**Location:** `C:\Program Files (x86)\Windows Kits\10\`

**Include directories:**
- `...\Include\10.0.26100.0\ucrt\`
- `...\Include\10.0.26100.0\um\`
- `...\Include\10.0.26100.0\shared\`

**Library directories:**
- `...\Lib\10.0.26100.0\ucrt\x64\`
- `...\Lib\10.0.26100.0\um\x64\`

**Binary directory:**
- `...\bin\10.0.26100.0\x64\` (contains `rc.exe`, `mt.exe`, `signtool.exe`)

**Multiple SDK bin versions found** (older ones likely residual):
- 10.0.14393.0, 10.0.15063.0, 10.0.16299.0, 10.0.17134.0, 10.0.26100.0

**Cannot be CLI-installed.** Installed via the Visual Studio Installer as an individual component, or standalone from:
https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/

### 3. CMake

**Status:** FOUND at three locations.

| Location | Notes |
|---|---|
| `C:\Program Files\CMake\bin\cmake.exe` | **Standalone install (preferred)** |
| `C:\Qt\Tools\CMake_64\bin\cmake.exe` | Qt-bundled CMake |
| `...\VS 2022\...\CMake\bin\cmake.exe` | VS-bundled CMake |

**CLI install/repair:**
```powershell
winget install Kitware.CMake --override '/FORCE /VERYSILENT /NORESTART /ADD_CMAKE_TO_PATH=System'
```

**Manual download:** https://cmake.org/download/

### 4. Ninja

**Status:** FOUND at `C:\Qt\Tools\Ninja\ninja.exe` (Qt-bundled).

**CLI install (standalone):**
```powershell
winget install Ninja-build.Ninja
```

**Manual download:** https://github.com/nicean/ninja/releases

### 5. Qt Framework

**Status:** FOUND -- multiple kits installed.

**Qt 6.10.0 kits:**
| Kit | Path |
|---|---|
| msvc2022_64 (PRIMARY) | `C:\Qt\6.10.0\msvc2022_64\` |
| msvc2022_arm64 | `C:\Qt\6.10.0\msvc2022_arm64\` |
| mingw_64 | `C:\Qt\6.10.0\mingw_64\` |
| llvm-mingw_64 | `C:\Qt\6.10.0\llvm-mingw_64\` |

**Qt 6.9.2 kits (older):**
| Kit | Path |
|---|---|
| msvc2022_64 | `C:\Qt\6.9.2\msvc2022_64\` |
| msvc2022_arm64 | `C:\Qt\6.9.2\msvc2022_arm64\` |
| Source | `C:\Qt\6.9.2\Src\` |

**Qt Tools:**
- CMake: `C:\Qt\Tools\CMake_64\`
- Ninja: `C:\Qt\Tools\Ninja\`
- MinGW 13.1.0: `C:\Qt\Tools\mingw1310_64\`
- LLVM-MinGW 17.06: `C:\Qt\Tools\llvm-mingw1706_64\`
- Qt Creator: `C:\Qt\Tools\QtCreator\`
- Qt Design Studio: `C:\Qt\Tools\QtDesignStudio-4.8.0-preview\`
- OpenSSL v3: `C:\Qt\Tools\OpenSSLv3\`
- Qt Installer Framework: `C:\Qt\Tools\QtInstallerFramework\`

**Key CMake config:**
- `Qt6Config.cmake`: `C:\Qt\6.10.0\msvc2022_64\lib\cmake\Qt6\Qt6Config.cmake`

**CMake variables to set:**
```
Qt6_DIR=C:\Qt\6.10.0\msvc2022_64\lib\cmake\Qt6
CMAKE_PREFIX_PATH=C:\Qt\6.10.0\msvc2022_64
```

**Cannot be CLI-installed.** Use the Qt Online Installer:
1. Download from: https://www.qt.io/download-qt-installer
2. Sign in with Qt account (free for open-source use)
3. Select: Qt 6.10.0 > MSVC 2022 64-bit
4. Under "Additional Libraries", select any modules your project uses
5. Under "Developer and Designer Tools", ensure CMake and Ninja are checked

### 6. Clang / LLVM

**Status:** PARTIALLY FOUND

| Tool | Status | Location |
|---|---|---|
| clang.exe (Qt llvm-mingw) | FOUND | `C:\Qt\Tools\llvm-mingw1706_64\bin\clang.exe` |
| clang.exe (WinGet) | FOUND | `...\WinGet\...\llvm-mingw-20260311-ucrt-x86_64\bin\` |
| clang.exe (standalone) | NOT FOUND | Expected `C:\Program Files\LLVM\bin\` |
| clang-cl.exe | NOT FOUND | Not found in any location |
| lld-link.exe | NOT FOUND | Not found in any location |

**Note:** The Qt llvm-mingw distribution targets MinGW (GNU) ABI, not MSVC ABI.
For MSVC-compatible Clang (`clang-cl.exe`), install standalone LLVM:

```powershell
winget install LLVM.LLVM --override '/FORCE /VERYSILENT /NORESTART'
```

**Manual download:** https://github.com/llvm/llvm-project/releases
- Choose: `LLVM-XX.X.X-win64.exe`
- During install, select "Add LLVM to the system PATH"

### 7. GCC / MinGW / w64devkit

**Status:** FOUND

| Tool | Version | Location |
|---|---|---|
| gcc.exe | 15.2.0 | `C:\w64devkit\bin\gcc.exe` |
| g++.exe | 15.2.0 | `C:\w64devkit\bin\g++.exe` |
| make.exe | -- | `C:\w64devkit\bin\make.exe` |
| MinGW (Qt) | 13.1.0 | `C:\Qt\Tools\mingw1310_64\bin\` |

**w64devkit** is a self-contained GCC toolchain. Download/update from:
https://github.com/skeeto/w64devkit/releases

**Warning:** Do not mix w64devkit/MinGW-built libraries with MSVC-built libraries.
The SetecPartitionWizard project uses MSVC -- use w64devkit only for standalone
C/C++ utilities, not for building the main Qt application.

### 8. Python

**Status:** FOUND -- two installations.

| Version | Location | Notes |
|---|---|---|
| 3.14.0rc2 | `C:\Python314\python.exe` | Pre-release, manual install |
| 3.13.7 | `C:\Users\mdavi\AppData\Local\Programs\Python\Python313\python.exe` | Standard install, pip available |

**WindowsApps alias detected:** `python` and `python3` in PATH resolve to the
Microsoft Store redirector at `C:\Users\mdavi\AppData\Local\Microsoft\WindowsApps\`.
This may interfere with the real Python installations.

**Fix:** Settings > Apps > Advanced app settings > App execution aliases >
Turn off `python.exe` and `python3.exe`.

**CLI install:**
```powershell
winget install Python.Python.3.13
```

### 9. Git

**Status:** FOUND

- Version: 2.53.0.windows.1
- Location: `C:\Program Files\Git\cmd\git.exe`

**CLI install/update:**
```powershell
winget install Git.Git --override '/VERYSILENT /NORESTART'
```

### 10. Build Helpers

| Tool | Status | Location |
|---|---|---|
| nmake.exe | FOUND | `...\MSVC\14.44.35207\bin\Hostx64\x64\nmake.exe` |
| make.exe | FOUND | `C:\w64devkit\bin\make.exe` (GNU Make) |
| pkg-config | NOT FOUND | Not installed anywhere |
| MSBuild | FOUND (implicit) | Part of VS 2022 Professional |

**To install pkg-config:**
```powershell
choco install pkgconfiglite -y
# or
winget install bloodrock.pkg-config-lite
```

---

## Additional Tools Found (Not Project-Critical)

| Tool | Location |
|---|---|
| Go | `C:\Program Files\Go\bin\go.exe` |
| CUDA v13.1 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\` |
| .NET SDK | `C:\Program Files\dotnet\` |
| GitHub Desktop | `C:\Users\mdavi\AppData\Local\GitHubDesktop\` |
| LM Studio | `C:\Users\mdavi\.lmstudio\bin\` |
| Metasploit | `C:\metasploit-framework\bin\` |

---

## Recommended Build Command

For the SetecPartitionWizard project using MSVC + Qt 6.10.0 + CMake + Ninja:

```powershell
# Option A: From a VS Developer PowerShell (vcvars already sourced)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.10.0/msvc2022_64"
cmake --build build

# Option B: From a regular PowerShell (after running repair_path.ps1)
# The INCLUDE, LIB, Qt6_DIR, and CMAKE_PREFIX_PATH env vars are set permanently.
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build build
```

---

## PATH Issues Detected

1. **Duplicate entries:** The User PATH contains many duplicate entries (the entire System PATH
   appears to be duplicated in User PATH). Run `repair_path.ps1` to clean this up.

2. **WindowsApps Python alias:** The Store alias for `python.exe` shadows real Python installs.
   Disable via App execution aliases in Settings.

3. **Missing from PATH:** CMake (`C:\Program Files\CMake\bin`) is in the System PATH but
   MSVC, Ninja, Qt, and w64devkit are not in either User or System PATH.

4. **Quoted paths:** Some User PATH entries have literal single-quote characters around them
   (e.g., `'C:\Users\mdavi\AppData\...\Scripts'`), which may cause resolution failures.
