#Requires -Version 5.1
<#
.SYNOPSIS
    Installs or repairs development tools for the SetecPartitionWizard project
    using winget (primary), choco (fallback), and pip.

.DESCRIPTION
    For each tool, the script checks whether it is already installed and functional.
    If not, it attempts installation via winget, then falls back to Chocolatey.
    Python packages are installed via pip after Python is confirmed working.

    Run from an elevated PowerShell prompt for best results (some winget/choco
    installs require admin).

.NOTES
    Generated 2026-03-11 by Goju PATH repair agent.
    Tools that CANNOT be CLI-installed (MSVC, Qt, Windows SDK) are documented
    in docs/tool_compilers.md with manual install instructions.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

# ─────────────────────────────────────────────────────────────
# Helper: test if a command is available
# ─────────────────────────────────────────────────────────────
function Test-CommandExists {
    param([string]$Command)
    $null -ne (Get-Command $Command -ErrorAction SilentlyContinue)
}

function Write-Step {
    param([string]$Name, [string]$Status, [string]$Detail = "")
    $color = switch ($Status) {
        "FOUND"    { "Green" }
        "INSTALL"  { "Cyan" }
        "SKIP"     { "DarkGray" }
        "FAIL"     { "Red" }
        "OK"       { "Green" }
        default    { "White" }
    }
    Write-Host "  [$Status] $Name" -ForegroundColor $color -NoNewline
    if ($Detail) { Write-Host " -- $Detail" -ForegroundColor DarkGray } else { Write-Host "" }
}

# ─────────────────────────────────────────────────────────────
# Check for package managers
# ─────────────────────────────────────────────────────────────
Write-Host "`n=== SetecPartitionWizard Tool Installer ===" -ForegroundColor Cyan

$HasWinget = Test-CommandExists "winget"
$HasChoco  = Test-CommandExists "choco"

if ($HasWinget) { Write-Step "winget" "FOUND" } else { Write-Step "winget" "FAIL" "Not available -- winget installs will be skipped" }
if ($HasChoco)  { Write-Step "choco"  "FOUND" } else { Write-Step "choco"  "SKIP" "Not available -- choco fallback disabled" }

# ─────────────────────────────────────────────────────────────
# 1. CMake
# ─────────────────────────────────────────────────────────────
Write-Host "`n--- CMake ---" -ForegroundColor Yellow

$cmakeExe = "C:\Program Files\CMake\bin\cmake.exe"
if (Test-Path $cmakeExe) {
    $ver = & $cmakeExe --version 2>&1 | Select-Object -First 1
    Write-Step "CMake" "FOUND" $ver
}
else {
    Write-Step "CMake" "INSTALL" "Installing via winget..."
    if ($HasWinget) {
        winget install Kitware.CMake --accept-package-agreements --accept-source-agreements --override '/FORCE /VERYSILENT /NORESTART /ADD_CMAKE_TO_PATH=System'
    }
    elseif ($HasChoco) {
        choco install cmake --installargs '"ADD_CMAKE_TO_PATH=System"' -y --force
    }
    else {
        Write-Step "CMake" "FAIL" "No package manager available. Download from https://cmake.org/download/"
    }
}

# ─────────────────────────────────────────────────────────────
# 2. Ninja
# ─────────────────────────────────────────────────────────────
Write-Host "`n--- Ninja ---" -ForegroundColor Yellow

$ninjaExe = "C:\Qt\Tools\Ninja\ninja.exe"
if (Test-Path $ninjaExe) {
    $ver = & $ninjaExe --version 2>&1
    Write-Step "Ninja" "FOUND" "v$ver (Qt-bundled)"
}
elseif (Test-CommandExists "ninja") {
    Write-Step "Ninja" "FOUND" "in PATH"
}
else {
    Write-Step "Ninja" "INSTALL" "Installing via winget..."
    if ($HasWinget) {
        winget install Ninja-build.Ninja --accept-package-agreements --accept-source-agreements
    }
    elseif ($HasChoco) {
        choco install ninja -y
    }
    else {
        Write-Step "Ninja" "FAIL" "Download from https://github.com/nicean/ninja/releases"
    }
}

# ─────────────────────────────────────────────────────────────
# 3. Git
# ─────────────────────────────────────────────────────────────
Write-Host "`n--- Git ---" -ForegroundColor Yellow

if (Test-CommandExists "git") {
    $ver = git --version 2>&1
    Write-Step "Git" "FOUND" $ver
}
else {
    Write-Step "Git" "INSTALL" "Installing via winget..."
    if ($HasWinget) {
        winget install Git.Git --accept-package-agreements --accept-source-agreements --override '/VERYSILENT /NORESTART'
    }
    elseif ($HasChoco) {
        choco install git -y --force
    }
}

# ─────────────────────────────────────────────────────────────
# 4. Python
# ─────────────────────────────────────────────────────────────
Write-Host "`n--- Python ---" -ForegroundColor Yellow

$python314 = "C:\Python314\python.exe"
$python313 = "C:\Users\mdavi\AppData\Local\Programs\Python\Python313\python.exe"

if (Test-Path $python314) {
    $ver = & $python314 --version 2>&1
    Write-Step "Python 3.14" "FOUND" $ver
}
else {
    Write-Step "Python 3.14" "SKIP" "Not found at C:\Python314 -- install manually (pre-release)"
}

if (Test-Path $python313) {
    $ver = & $python313 --version 2>&1
    Write-Step "Python 3.13" "FOUND" $ver
}
else {
    Write-Step "Python 3.13" "INSTALL" "Installing via winget..."
    if ($HasWinget) {
        winget install Python.Python.3.13 --accept-package-agreements --accept-source-agreements
    }
    elseif ($HasChoco) {
        choco install python313 -y
    }
}

# Disable WindowsApps python alias (common source of confusion)
Write-Host "`n  TIP: If 'python' opens the Microsoft Store, disable the alias:" -ForegroundColor DarkGray
Write-Host "    Settings > Apps > Advanced app settings > App execution aliases" -ForegroundColor DarkGray
Write-Host "    Turn off 'python.exe' and 'python3.exe' aliases`n" -ForegroundColor DarkGray

# ─────────────────────────────────────────────────────────────
# 5. LLVM / Clang (standalone)
# ─────────────────────────────────────────────────────────────
Write-Host "--- LLVM / Clang ---" -ForegroundColor Yellow

$llvmPaths = @(
    "C:\Program Files\LLVM\bin\clang.exe",
    "C:\Qt\Tools\llvm-mingw1706_64\bin\clang.exe"
)
$foundLlvm = $false
foreach ($p in $llvmPaths) {
    if (Test-Path $p) {
        $ver = & $p --version 2>&1 | Select-Object -First 1
        Write-Step "Clang" "FOUND" "$ver ($p)"
        $foundLlvm = $true
        break
    }
}
if (-not $foundLlvm) {
    Write-Step "Clang" "INSTALL" "Installing standalone LLVM via winget..."
    if ($HasWinget) {
        winget install LLVM.LLVM --accept-package-agreements --accept-source-agreements --override '/FORCE /VERYSILENT /NORESTART'
    }
    elseif ($HasChoco) {
        choco install llvm -y --force
    }
    else {
        Write-Step "Clang" "FAIL" "Download from https://github.com/llvm/llvm-project/releases"
    }
}

# ─────────────────────────────────────────────────────────────
# 6. GitHub CLI
# ─────────────────────────────────────────────────────────────
Write-Host "`n--- GitHub CLI ---" -ForegroundColor Yellow

if (Test-CommandExists "gh") {
    $ver = gh --version 2>&1 | Select-Object -First 1
    Write-Step "GitHub CLI" "FOUND" $ver
}
else {
    Write-Step "GitHub CLI" "INSTALL" "Installing via winget..."
    if ($HasWinget) {
        winget install GitHub.cli --accept-package-agreements --accept-source-agreements
    }
    elseif ($HasChoco) {
        choco install gh -y
    }
}

# ─────────────────────────────────────────────────────────────
# 7. Python packages (pip)
# ─────────────────────────────────────────────────────────────
Write-Host "`n--- Python packages (pip) ---" -ForegroundColor Yellow

# Find the best available python
$PythonExe = $null
if (Test-Path $python314) { $PythonExe = $python314 }
elseif (Test-Path $python313) { $PythonExe = $python313 }
elseif (Test-CommandExists "python") { $PythonExe = "python" }

if ($PythonExe) {
    $pipPackages = @(
        "Pillow",       # Icon/image generation for the app
        "jinja2",       # Template engine (useful for code generation)
        "pyyaml"        # YAML parsing
    )

    foreach ($pkg in $pipPackages) {
        Write-Host "  Checking $pkg..." -NoNewline
        $installed = & $PythonExe -m pip show $pkg 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host " already installed" -ForegroundColor Green
        }
        else {
            Write-Host " installing..." -ForegroundColor Cyan
            & $PythonExe -m pip install --user $pkg
        }
    }
}
else {
    Write-Step "pip packages" "SKIP" "No Python found"
}

# ─────────────────────────────────────────────────────────────
# 8. MANUAL-ONLY TOOLS (cannot be CLI-installed)
# ─────────────────────────────────────────────────────────────
Write-Host "`n--- Manual-install tools (verification only) ---" -ForegroundColor Yellow

# MSVC
$clExe = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe"
if (Test-Path $clExe) {
    Write-Step "MSVC cl.exe" "FOUND" "v14.44.35207 (VS 2022 Professional)"
}
else {
    Write-Step "MSVC cl.exe" "FAIL" "Not found -- install via Visual Studio Installer (see docs/tool_compilers.md)"
}

# Windows SDK
$rcExe = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\rc.exe"
if (Test-Path $rcExe) {
    Write-Step "Windows SDK" "FOUND" "10.0.26100.0"
}
else {
    Write-Step "Windows SDK" "FAIL" "Not found -- install via Visual Studio Installer (see docs/tool_compilers.md)"
}

# Qt
$qtBinDir = "C:\Qt\6.10.0\msvc2022_64\bin"
if (Test-Path "$qtBinDir\qmake.exe") {
    Write-Step "Qt 6.10.0" "FOUND" "msvc2022_64 at $qtBinDir"
}
else {
    Write-Step "Qt 6.10.0" "FAIL" "Not found -- install via Qt Online Installer (see docs/tool_compilers.md)"
}

# w64devkit
if (Test-Path "C:\w64devkit\bin\gcc.exe") {
    $ver = & "C:\w64devkit\bin\gcc.exe" --version 2>&1 | Select-Object -First 1
    Write-Step "w64devkit" "FOUND" $ver
}
else {
    Write-Step "w64devkit" "SKIP" "Not found at C:\w64devkit -- download from https://github.com/skeeto/w64devkit/releases"
}

Write-Host "`n=== Tool installation complete ===" -ForegroundColor Green
Write-Host "Run .\repair_path.ps1 next to ensure all paths are registered.`n"
