# Setec Partition Wizard — Build Documentation

> Last updated: 2026-03-11

## Project Overview

**Setec Partition Wizard** is a comprehensive C++17/Qt6 professional disk utility for Windows. It provides partition management, disk cloning, image creation/restore, ISO flashing, file/partition recovery, boot repair, S.M.A.R.T. diagnostics, benchmarking, surface scanning, secure erase, FIDO2 security keys, encrypted vaults, and boot authentication — covering everything that commercial tools like Partition Magic, Acronis, and EaseUS used to offer.

---

## Build Requirements

| Component | Required Version | Location on this system |
|-----------|-----------------|------------------------|
| MSVC (cl.exe) | 2022 (toolset 14.44.35207) | `C:\Program Files\Microsoft Visual Studio\2022\Professional\` |
| Windows SDK | 10.0.26100.0 | `C:\Program Files (x86)\Windows Kits\10\` |
| CMake | 3.25+ | `C:\Program Files\CMake\bin\cmake.exe` |
| Ninja | any | `C:\Qt\Tools\Ninja\ninja.exe` |
| Qt | 6.10.0 (msvc2022_64) | `C:\Qt\6.10.0\msvc2022_64\` |
| Python | 3.x (for icon generation) | `C:\Python314\python.exe` |

### Optional Tools
| Tool | Purpose | Location |
|------|---------|----------|
| w64devkit (GCC 15.2.0) | Alternative compiler | `C:\w64devkit\bin\` |
| Clang (via Qt llvm-mingw) | Alternative compiler | `C:\Qt\Tools\llvm-mingw1706_64\bin\` |
| VS Build Tools | Headless builds | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\` |

---

## Architecture

```
SetecPartitionWizard/
├── CMakeLists.txt              # Root build - finds Qt6, includes cmake/, adds src/
├── CMakePresets.json            # Presets with embedded MSVC/SDK environment
├── cmake/
│   ├── CompilerWarnings.cmake   # /W4 /permissive- /utf-8 flags
│   ├── Version.cmake            # SPW_VERSION_* defines
│   └── GenerateKey.cmake        # Builds spw_keygen, generates EmbeddedKey.h + garbage.xtx
├── src/
│   ├── core/                    # spw_core static library (28 .cpp files)
│   │   ├── common/              # Types.h, Result.h, Error.h, Constants.h, Logging
│   │   ├── disk/                # RawDiskHandle, VolumeHandle, DiskEnumerator, DiskGeometry,
│   │   │                        # SmartReader, PartitionTable, FilesystemDetector, FilesystemInfo
│   │   ├── filesystem/          # FormatEngine (NTFS/FAT/ext/exFAT/swap/Btrfs/XFS)
│   │   ├── operations/          # Operation base, OperationQueue, PartitionOperations (7 op types)
│   │   ├── recovery/            # PartitionRecovery, FileRecovery (MFT/FAT/ext/carving), BootRepair
│   │   ├── diagnostics/         # Benchmark (seq/random R/W, QD1/QD32), SurfaceScan
│   │   ├── imaging/             # Checksums (SHA-256/MD5/CRC32), DiskCloner, ImageCreator,
│   │   │                        # ImageRestorer, IsoFlasher (ISO9660 parser + UEFI detection)
│   │   ├── maintenance/         # SecureErase (Zero/DoD-3/7/Gutmann/Random/Custom)
│   │   └── security/            # EncryptedVault (AES-256-XTS/CBC/GCM), Fido2Manager (CTAP2),
│   │                            # BootAuthenticator (HMAC-SHA256), OratDecoder
│   ├── ui/                      # spw_ui static library
│   │   ├── MainWindow.cpp/h     # Tab container, F5=secret menu, Ctrl+R=refresh
│   │   ├── tabs/                # DiskPartitionTab, RecoveryTab, ImagingTab, DiagnosticsTab,
│   │   │                        # SecurityTab, MaintenanceTab, StarGenerator
│   │   ├── dialogs/             # AstroChicken, Arnoid, Vohaul (secret menu chain)
│   │   └── widgets/             # DiskMapWidget (visual partition map)
│   └── app/                     # SetecPartitionWizard.exe
│       ├── main.cpp             # Entry point, single-instance lock
│       └── SingleInstance.cpp/h
├── third_party/                 # GTest via FetchContent, hwdiag (secret pentesting module)
├── tests/                       # Unit tests
├── tools/                       # spw_keygen (build-time key generator)
├── resources/
│   ├── resources.qrc            # Qt resource file
│   ├── garbage.xtx              # Generated riddle file
│   └── icons/                   # app.ico + toolbar PNGs
├── scripts/
│   ├── repair_path.ps1          # PowerShell: repair PATH/INCLUDE/LIB environment
│   └── install_tools.ps1        # PowerShell: install/repair dev tools via winget/choco
└── docs/
    ├── build.md                 # This file
    └── tool_compilers.md        # Complete tool inventory with install instructions
```

### Library Dependencies

**spw_core** links against:
- `Qt6::Core` — QString, QThread, signals/slots
- `setupapi` — SetupDi* device enumeration
- `wbemuuid` — WMI (IWbemServices) for disk info
- `ole32`, `oleaut32` — COM initialization for WMI
- `bcrypt` — SHA-256, AES-256, PBKDF2, HMAC (Windows CNG)
- `ntdll` — LZNT1 compression (RtlCompressBuffer/RtlDecompressBuffer)
- `virtdisk` — VHD mount/unmount (AttachVirtualDisk)
- `hid` — HID device enumeration for FIDO2 USB tokens

**spw_ui** links against:
- `Qt6::Widgets` — QMainWindow, QTabWidget, all UI widgets
- `spw_core` — backend logic

---

## Build Instructions

### Option A: From Developer Command Prompt (Recommended)
```cmd
"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
cd C:\Users\mdavi\SetecPartitionWizard
cmake --preset default
cmake --build build/default
```

### Option B: From Git Bash (requires environment setup)
```bash
# Set MSVC environment (INCLUDE/LIB must use Windows-style paths with semicolons)
export MSVC_VER="14.44.35207"
export WINSDK_VER="10.0.26100.0"
export VCDIR="C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/MSVC/$MSVC_VER"
export SDKDIR="C:/Program Files (x86)/Windows Kits/10"

# PATH needs MSYS-style /c/ paths
export PATH="/c/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/MSVC/$MSVC_VER/bin/Hostx64/x64:/c/Program Files (x86)/Windows Kits/10/bin/$WINSDK_VER/x64:/c/Qt/Tools/Ninja:/c/Program Files/CMake/bin:/c/Qt/6.10.0/msvc2022_64/bin:/c/Windows/System32:/c/Windows:/c/Program Files/Git/cmd:/c/Program Files/Git/usr/bin"

# INCLUDE and LIB need Windows-style C:/ paths with semicolons
export INCLUDE="$VCDIR/include;$SDKDIR/Include/$WINSDK_VER/ucrt;$SDKDIR/Include/$WINSDK_VER/um;$SDKDIR/Include/$WINSDK_VER/shared;$SDKDIR/Include/$WINSDK_VER/winrt;$SDKDIR/Include/$WINSDK_VER/cppwinrt"
export LIB="$VCDIR/lib/x64;$SDKDIR/Lib/$WINSDK_VER/ucrt/x64;$SDKDIR/Lib/$WINSDK_VER/um/x64"

cmake --preset default
cmake --build build/default
```

### Option C: Using CMake Build Presets
The `CMakePresets.json` has embedded MSVC environment in both configure and build presets:
```bash
cmake --preset default
cmake --build --preset default
```
**Note:** The build preset includes INCLUDE/LIB/PATH so Ninja finds cl.exe and all headers.

### Option D: Run repair_path.ps1 first (fixes system-wide)
```powershell
# In PowerShell (Admin)
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
.\scripts\repair_path.ps1
# Then restart terminal and build normally
```

---

## Build Troubleshooting History

### Issue: windows.h not found
**Symptom:** `fatal error C1083: Cannot open include file: 'windows.h'`
**Cause:** MSVC's cl.exe relies on the `INCLUDE` environment variable to find system headers. When building from Git Bash, this variable is either not set or gets mangled by MSYS path conversion.
**Fix:** CMakePresets.json now embeds INCLUDE/LIB in both configurePresets and buildPresets. Alternatively, use `repair_path.ps1` to set them system-wide.

### Issue: INCLUDE env var works for configure but not build
**Symptom:** `cmake --preset default` succeeds (cl.exe detected), but `cmake --build build/default` fails with missing headers.
**Cause:** CMake preset `environment` in configurePresets only applies during the configure step. Ninja inherits the shell's environment, not CMake's. The build presets need their own `environment` block.
**Fix:** Added `environment` with INCLUDE/LIB/PATH to buildPresets in CMakePresets.json.

### Issue: Git Bash PATH with spaces
**Symptom:** Commands in directories with spaces fail.
**Cause:** Git Bash/MSYS uses `/c/` style paths. PATH entries must use `/c/Program Files/...` not `C:\Program Files\...`.
**Fix:** PATH uses MSYS-style, INCLUDE/LIB use Windows-style.

### Issue: F5 key conflict
**Symptom:** F5 triggered "Refresh Disks" instead of the secret AstroChicken menu.
**Cause:** `QKeySequence::Refresh` maps to F5 on Windows, intercepting keyPressEvent.
**Fix:** Changed refresh shortcut to `QKeySequence(Qt::CTRL | Qt::Key_R)`. F5 now correctly triggers the secret menu via keyPressEvent.

### Issue: hwdiag CRT mismatch (LNK2038)
**Symptom:** Linker error about mismatched RuntimeLibrary (MDd vs MD).
**Cause:** hwdiag was built as Release (/MD) but main project is Debug (/MDd).
**Fix:** Build hwdiag in Debug mode to match.

### Issue: SPW_BUILD_TESTS option ordering
**Symptom:** GTest never fetched, tests don't build.
**Cause:** `option(SPW_BUILD_TESTS)` was declared AFTER `add_subdirectory(third_party)`.
**Fix:** Moved option declaration before add_subdirectory.

---

## Current Build Status (2026-03-11)

**NOT YET COMPILING.** All ~88 source files (41 .cpp, 47 .h) are written but have never been compiled together successfully. Expected issues:
- Type mismatches between files written by different agents
- Missing includes
- Struct field name inconsistencies
- Interface mismatches between headers and implementations

The MSVC environment issue (INCLUDE/LIB not reaching Ninja) must be resolved first before code-level errors can be addressed. Use `repair_path.ps1` or build from Developer Command Prompt.

---

## Key Design Decisions

1. **No exceptions** — Uses `Result<T>` monadic error handling throughout
2. **No OpenSSL** — All crypto via Windows BCrypt (CNG) API
3. **GParted-style operation queue** — Changes are queued, previewed, then applied atomically
4. **RAII disk handles** — RawDiskHandle and VolumeHandle auto-close on destruction
5. **Removable-only safety** — IsoFlasher refuses to write to fixed disks
6. **Admin required** — Raw disk I/O requires elevation; app checks and prompts
7. **Secret menu** — F5 triggers hidden pentesting module disguised as `libspw_hwdiag` static library
