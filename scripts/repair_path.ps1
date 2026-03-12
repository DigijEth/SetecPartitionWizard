#Requires -Version 5.1
<#
.SYNOPSIS
    Repairs the Windows PATH and sets environment variables for the
    SetecPartitionWizard C++17/Qt6 build environment.

.DESCRIPTION
    This script permanently adds missing dev-tool directories to the User PATH
    (via [Environment]::SetEnvironmentVariable) and sets INCLUDE, LIB, Qt6_DIR,
    CMAKE_PREFIX_PATH, and other variables needed by CMake/Ninja/MSVC builds.

    It does NOT remove any existing PATH entries -- it only appends missing ones.

    Run from an elevated or normal PowerShell prompt. After running, open a NEW
    terminal for the changes to take effect.

.NOTES
    Generated 2026-03-11 by Goju PATH repair agent.
    Machine: mdavi / MSYS_NT-10.0-26200
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ─────────────────────────────────────────────────────────────
# 1. DISCOVERED TOOL PATHS (from system scan 2026-03-11)
# ─────────────────────────────────────────────────────────────

# MSVC 2022 Professional 17.14.14, toolset 14.44.35207
$MsvcVersion    = "14.44.35207"
$VsRoot         = "C:\Program Files\Microsoft Visual Studio\2022\Professional"
$MsvcBin        = "$VsRoot\VC\Tools\MSVC\$MsvcVersion\bin\Hostx64\x64"
$MsvcInclude    = "$VsRoot\VC\Tools\MSVC\$MsvcVersion\include"
$MsvcLib        = "$VsRoot\VC\Tools\MSVC\$MsvcVersion\lib\x64"
$VcVarsAll      = "$VsRoot\VC\Auxiliary\Build\vcvarsall.bat"
$VcVars64       = "$VsRoot\VC\Auxiliary\Build\vcvars64.bat"

# Windows SDK 10.0.26100.0
$SdkVersion     = "10.0.26100.0"
$SdkRoot        = "C:\Program Files (x86)\Windows Kits\10"
$SdkBin         = "$SdkRoot\bin\$SdkVersion\x64"
$SdkIncludeUcrt = "$SdkRoot\Include\$SdkVersion\ucrt"
$SdkIncludeUm   = "$SdkRoot\Include\$SdkVersion\um"
$SdkIncludeShared = "$SdkRoot\Include\$SdkVersion\shared"
$SdkLibUcrt     = "$SdkRoot\Lib\$SdkVersion\ucrt\x64"
$SdkLibUm       = "$SdkRoot\Lib\$SdkVersion\um\x64"

# CMake 3.x (standalone install)
$CmakeBin       = "C:\Program Files\CMake\bin"

# Ninja (Qt-bundled)
$NinjaBin       = "C:\Qt\Tools\Ninja"

# Qt 6.10.0 MSVC 2022 x64 (primary build kit)
$QtRoot         = "C:\Qt\6.10.0\msvc2022_64"
$QtBin          = "$QtRoot\bin"
$QtCmake        = "$QtRoot\lib\cmake\Qt6"

# Qt Tools CMake (separate from standalone CMake)
$QtCmakeBin     = "C:\Qt\Tools\CMake_64\bin"

# Clang/LLVM via Qt llvm-mingw 17.06
$LlvmMingwBin   = "C:\Qt\Tools\llvm-mingw1706_64\bin"

# w64devkit (GCC 15.2.0, make, etc.)
$W64DevkitBin   = "C:\w64devkit\bin"

# Python 3.14 (primary) and 3.13 (secondary)
$Python314      = "C:\Python314"
$Python313      = "C:\Users\mdavi\AppData\Local\Programs\Python\Python313"
$Python313Scripts = "C:\Users\mdavi\AppData\Local\Programs\Python\Python313\Scripts"

# Git for Windows
$GitCmd         = "C:\Program Files\Git\cmd"

# Go (found on system)
$GoBin          = "C:\Program Files\Go\bin"

# Chocolatey
$ChocoBin       = "C:\ProgramData\chocolatey\bin"

# VS Code
$VsCodeBin      = "C:\Users\mdavi\AppData\Local\Programs\Microsoft VS Code\bin"

# GitHub CLI
$GhCliBin       = "C:\Program Files\GitHub CLI"

# WinGet LLVM-MinGW (UCRT, installed 2026-03-11)
$WingetLlvmMingw = "C:\Users\mdavi\AppData\Local\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\llvm-mingw-20260311-ucrt-x86_64\bin"

# ─────────────────────────────────────────────────────────────
# 2. DEFINE DESIRED PATH ORDER (highest priority first)
# ─────────────────────────────────────────────────────────────
# Priority rationale:
#   - MSVC cl.exe and SDK tools first (primary compiler)
#   - CMake and Ninja next (build system)
#   - Qt bin (for windeployqt, moc, uic, rcc)
#   - Python, Git, and other helpers later

$DevToolPaths = @(
    $MsvcBin,           # cl.exe, link.exe, nmake.exe
    $SdkBin,            # rc.exe, mt.exe, signtool.exe
    $CmakeBin,          # cmake.exe, ctest.exe, cpack.exe
    $NinjaBin,          # ninja.exe
    $QtBin,             # windeployqt.exe, moc.exe, uic.exe, rcc.exe
    $QtCmakeBin,        # Qt-bundled cmake (fallback)
    $LlvmMingwBin,      # clang.exe, clang++.exe (Qt llvm-mingw)
    $W64DevkitBin,      # gcc.exe, g++.exe, make.exe
    $Python314,         # python.exe 3.14
    $Python313,         # python.exe 3.13
    $Python313Scripts,  # pip.exe, etc.
    $GitCmd,            # git.exe
    $GoBin,             # go.exe
    $ChocoBin,          # choco.exe
    $GhCliBin,          # gh.exe
    $VsCodeBin,         # code.exe
    $WingetLlvmMingw    # winget-installed llvm-mingw
)

# ─────────────────────────────────────────────────────────────
# 3. READ CURRENT USER PATH AND APPEND MISSING ENTRIES
# ─────────────────────────────────────────────────────────────

Write-Host "`n=== SetecPartitionWizard PATH Repair ===" -ForegroundColor Cyan
Write-Host "Scanning current User PATH for missing dev-tool entries...`n"

$CurrentUserPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (-not $CurrentUserPath) { $CurrentUserPath = "" }

# Backup current PATH
$BackupFile = "$env:USERPROFILE\path_backup_$(Get-Date -Format 'yyyyMMdd_HHmmss').txt"
$CurrentUserPath | Out-File -FilePath $BackupFile -Encoding UTF8
Write-Host "  Backed up current User PATH to: $BackupFile" -ForegroundColor DarkGray

# Normalize: split, trim, remove empty, deduplicate (case-insensitive)
$ExistingEntries = $CurrentUserPath -split ';' |
    ForEach-Object { $_.Trim().Trim("'").Trim('"').TrimEnd('\') } |
    Where-Object { $_ -ne '' }

$ExistingSet = [System.Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase
)
foreach ($e in $ExistingEntries) { [void]$ExistingSet.Add($e) }

$Added = @()
$AlreadyPresent = @()

foreach ($dir in $DevToolPaths) {
    $normalized = $dir.TrimEnd('\')
    if ($ExistingSet.Contains($normalized)) {
        $AlreadyPresent += $normalized
    }
    elseif (Test-Path $normalized) {
        $Added += $normalized
        [void]$ExistingSet.Add($normalized)
    }
    else {
        Write-Host "  [SKIP] Not found on disk: $normalized" -ForegroundColor Yellow
    }
}

if ($AlreadyPresent.Count -gt 0) {
    Write-Host "`n  Already in User PATH:" -ForegroundColor Green
    $AlreadyPresent | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGreen }
}

if ($Added.Count -gt 0) {
    Write-Host "`n  Adding to User PATH:" -ForegroundColor Cyan
    $Added | ForEach-Object { Write-Host "    $_" -ForegroundColor White }

    # Build new PATH: existing entries + new entries
    $NewPath = (($ExistingEntries + $Added) | Select-Object -Unique) -join ';'

    # Safety: check total length
    if ($NewPath.Length -gt 30000) {
        Write-Warning "New PATH is $($NewPath.Length) chars -- approaching the 32767 limit!"
    }

    [Environment]::SetEnvironmentVariable("Path", $NewPath, "User")
    Write-Host "`n  User PATH updated permanently." -ForegroundColor Green
}
else {
    Write-Host "`n  No new PATH entries needed -- all dev tools already present." -ForegroundColor Green
}

# ─────────────────────────────────────────────────────────────
# 4. SET INCLUDE AND LIB FOR MSVC + WINDOWS SDK
# ─────────────────────────────────────────────────────────────
# Note: These are typically set by vcvars64.bat at session start.
# Setting them permanently in User env makes them available to
# CMake/Ninja even outside a Developer Command Prompt.

Write-Host "`n=== Setting INCLUDE and LIB ===" -ForegroundColor Cyan

$IncludePaths = @(
    $MsvcInclude,
    $SdkIncludeUcrt,
    $SdkIncludeUm,
    $SdkIncludeShared
) -join ';'

$LibPaths = @(
    $MsvcLib,
    $SdkLibUcrt,
    $SdkLibUm
) -join ';'

[Environment]::SetEnvironmentVariable("INCLUDE", $IncludePaths, "User")
Write-Host "  INCLUDE = $IncludePaths" -ForegroundColor DarkGray

[Environment]::SetEnvironmentVariable("LIB", $LibPaths, "User")
Write-Host "  LIB     = $LibPaths" -ForegroundColor DarkGray

# ─────────────────────────────────────────────────────────────
# 5. SET Qt AND CMAKE VARIABLES
# ─────────────────────────────────────────────────────────────

Write-Host "`n=== Setting Qt6 / CMake variables ===" -ForegroundColor Cyan

[Environment]::SetEnvironmentVariable("Qt6_DIR", $QtCmake, "User")
Write-Host "  Qt6_DIR          = $QtCmake"

[Environment]::SetEnvironmentVariable("CMAKE_PREFIX_PATH", $QtRoot, "User")
Write-Host "  CMAKE_PREFIX_PATH = $QtRoot"

[Environment]::SetEnvironmentVariable("QT_ROOT", $QtRoot, "User")
Write-Host "  QT_ROOT          = $QtRoot"

# ─────────────────────────────────────────────────────────────
# 6. VCVARS HELPER FUNCTION (for session-level MSVC setup)
# ─────────────────────────────────────────────────────────────

Write-Host "`n=== vcvars64 helper ===" -ForegroundColor Cyan
Write-Host @"

  The MSVC compiler (cl.exe) works best when vcvars64.bat has been sourced
  in the current session. The permanent INCLUDE/LIB vars above cover most
  CMake/Ninja use cases, but if you need the full VS environment, run:

    cmd /k "`"$VcVars64`" & powershell"

  Or add this function to your PowerShell profile ($PROFILE):

    function Enter-VsDevShell {
        Import-Module "`"$VsRoot\Common7\Tools\Microsoft.VisualStudio.DevShell.dll`""
        Enter-VsDevShell -VsInstallPath "`"$VsRoot`" -DevCmdArguments `"-arch=amd64`"
    }

"@ -ForegroundColor DarkGray

# ─────────────────────────────────────────────────────────────
# 7. SUMMARY
# ─────────────────────────────────────────────────────────────

Write-Host "=== Done ===" -ForegroundColor Green
Write-Host "Open a NEW terminal for all changes to take effect.`n"

# Show final User PATH for verification
Write-Host "Final User PATH entries:" -ForegroundColor Cyan
$FinalPath = [Environment]::GetEnvironmentVariable("Path", "User")
$FinalPath -split ';' | Where-Object { $_ -ne '' } | ForEach-Object {
    $marker = if (Test-Path $_) { "[OK]" } else { "[!!]" }
    $color  = if (Test-Path $_) { "Green" } else { "Red" }
    Write-Host "  $marker $_" -ForegroundColor $color
}
