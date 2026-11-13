<#
.SYNOPSIS
  All-in-one build script for the modified DwarFS (global chunk-level
  de-duplication) on Windows.

.DESCRIPTION
  This script:
    - locates Visual Studio 2022 and CMake
    - clones/updates vcpkg (pinned to the manifest baseline) and builds all
      third-party dependencies via vcpkg manifest mode
    - configures and builds mkdwarfs.exe, dwarfsextract.exe and dwarfsck.exe
      (plus the dwarfs.exe FUSE driver if the WinFsp SDK is installed)
    - runs a smoke test that verifies the --dedup-global option works and
      the resulting image extracts correctly

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\build_mkdwarfs_mod.ps1

.EXAMPLE
  # Build fully static binaries (no runtime DLLs)
  powershell -ExecutionPolicy Bypass -File .\build_mkdwarfs_mod.ps1 -Static

.EXAMPLE
  # Custom build/vcpkg locations, build tests too
  powershell -ExecutionPolicy Bypass -File .\build_mkdwarfs_mod.ps1 `
      -BuildDir D:\builds\dwarfs-mod -VcpkgRoot D:\tools\vcpkg -WithTests

.LICENSE
  SPDX-License-Identifier: AGPL-3.0-only
  Copyright (C) 2026 Edward Sloter

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as published
  by the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
#>

[CmdletBinding()]
param(
    # Build directory (default: <repo>\build-mod)
    [string]$BuildDir = "",

    # vcpkg installation to use/create (default: <repo>\.tools\vcpkg)
    [string]$VcpkgRoot = "",

    # CMake build configuration (Release, RelWithDebInfo, Debug, ...)
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug', 'MinSizeRel')]
    [string]$Config = 'Release',

    # Also build and run the test suite
    [switch]$WithTests,

    # Skip the smoke test at the end
    [switch]$SkipSmokeTest,

    # Force building the dwarfs.exe FUSE driver (requires WinFsp SDK)
    [switch]$ForceFuseDriver,

    # Disable building the dwarfs.exe FUSE driver
    [switch]$NoFuseDriver,

    # Build fully static binaries (no runtime DLLs needed)
    [switch]$Static
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ---------------------------------------------------------------------------
# locate repo root and derive defaults
# ---------------------------------------------------------------------------

$RepoRoot = $PSScriptRoot
if (-not $RepoRoot) { $RepoRoot = (Get-Location).Path }

if (-not $BuildDir) {
    if ($Static) {
        $BuildDir = Join-Path $RepoRoot 'build-mod-static'
    } else {
        $BuildDir = Join-Path $RepoRoot 'build-mod'
    }
}
if (-not $VcpkgRoot) { $VcpkgRoot = Join-Path $RepoRoot '.tools\vcpkg' }

# Clean the build directory for a fresh build every time
if (Test-Path $BuildDir) {
    Write-Host "  Cleaning build directory: $BuildDir"
    Remove-Item -Recurse -Force $BuildDir -ErrorAction SilentlyContinue
}

function Write-Step([string]$msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}

function Test-Command([string]$name) {
    return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------------------
# check prerequisites
# ---------------------------------------------------------------------------

Write-Step 'Checking prerequisites...'

if (-not (Test-Command 'git.exe')) {
    throw 'git not found in PATH. Please install Git for Windows: https://git-scm.com/download/win'
}

if (-not (Test-Command 'cmake.exe')) {
    throw 'cmake not found in PATH. Please install CMake: https://cmake.org/download/'
}

# locate Visual Studio 2022
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw 'vswhere.exe not found. Please install Visual Studio 2022 (with C++ workload): https://visualstudio.microsoft.com/'
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw 'Visual Studio 2022 with the "Desktop development with C++" workload was not found.'
}
Write-Host "  Visual Studio: $vsPath"

# extract the vcpkg baseline revision from vcpkg.json
$manifest = Get-Content (Join-Path $RepoRoot 'vcpkg.json') -Raw | ConvertFrom-Json
$VcpkgBaseline = $manifest.'builtin-baseline'
if (-not $VcpkgBaseline) { throw 'failed to read builtin-baseline from vcpkg.json' }
Write-Host "  vcpkg baseline: $VcpkgBaseline"

# ---------------------------------------------------------------------------
# set up vcpkg
# ---------------------------------------------------------------------------

Write-Step 'Setting up vcpkg...'

if (Test-Path (Join-Path $VcpkgRoot '.git')) {
    # reuse existing checkout; make sure it is on the baseline commit
    Write-Host "  Using existing vcpkg checkout: $VcpkgRoot"
    $curRev = & git -C $VcpkgRoot rev-parse HEAD 2>$null
    if ($curRev -ne $VcpkgBaseline) {
        Write-Host "  Fetching and checking out baseline $VcpkgBaseline"
        & git -C $VcpkgRoot fetch --quiet origin $VcpkgBaseline 2>$null
        if ($LASTEXITCODE -ne 0) {
            # shallow checkouts may lack objects; unshallow first
            & git -C $VcpkgRoot fetch --unshallow --quiet 2>$null
            & git -C $VcpkgRoot fetch --quiet origin $VcpkgBaseline 2>$null
        }
        & git -C $VcpkgRoot checkout --quiet $VcpkgBaseline
        if ($LASTEXITCODE -ne 0) { throw "failed to checkout vcpkg baseline $VcpkgBaseline" }
    }
} else {
    Write-Host "  Cloning vcpkg to $VcpkgRoot (this may take a few minutes)"
    & git clone --quiet https://github.com/microsoft/vcpkg.git $VcpkgRoot
    if ($LASTEXITCODE -ne 0) { throw 'failed to clone vcpkg' }
    & git -C $VcpkgRoot checkout --quiet $VcpkgBaseline
    if ($LASTEXITCODE -ne 0) { throw "failed to checkout vcpkg baseline $VcpkgBaseline" }
}

$vcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path $vcpkgExe)) {
    Write-Host '  Bootstrapping vcpkg...'
    Push-Location $VcpkgRoot
    try {
        & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed' }
    } finally {
        Pop-Location
    }
}

$ToolchainFile = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
Write-Host "  vcpkg ready: $vcpkgExe"

# ---------------------------------------------------------------------------
# configure
# ---------------------------------------------------------------------------

Write-Step 'Configuring CMake...'

# FUSE driver on Windows needs the WinFsp SDK
$buildFuse = $false

$fuseFlag = 'OFF'
if ($buildFuse) { $fuseFlag = 'ON' }

# Use the release-only vcpkg triplet. With the default triplet, CMake's
# pkg-config integration can pick up *debug* libraries (the debug pkgconfig
# directory comes first in CMAKE_PREFIX_PATH), which would silently link
# debug runtime DLLs into the release binaries.
$Triplet = 'x64-windows-release'
$OverlayTripletDir = ""

if ($Static) {
    # Create a custom vcpkg triplet: static CRT (/MT) + release-only.
    # vcpkg has x64-windows-static but no release-only variant; we overlay
    # a custom triplet file to avoid building debug variants of every lib.
    $Triplet = 'x64-windows-static-release'
    $OverlayTripletDir = Join-Path $BuildDir '_vcpkg_overlay_triplets'
    New-Item -ItemType Directory -Force -Path $OverlayTripletDir | Out-Null
    @(
        'set(VCPKG_TARGET_ARCHITECTURE x64)'
        'set(VCPKG_CRT_LINKAGE static)'
        'set(VCPKG_LIBRARY_LINKAGE static)'
        'set(VCPKG_BUILD_TYPE release)'
    ) | Set-Content (Join-Path $OverlayTripletDir "$Triplet.cmake") -Encoding UTF8

    Write-Host "  Static build: custom triplet '$Triplet' created in $OverlayTripletDir"
}

$cmakeArgs = @(
    '-S', $RepoRoot
    '-B', $BuildDir
    '-G', 'Visual Studio 17 2022'
    '-A', 'x64'
    "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
    "-DVCPKG_TARGET_TRIPLET=$Triplet"
    '-DWITH_TOOLS=ON'
)

if ($Static) {
    $cmakeArgs += "-DVCPKG_OVERLAY_TRIPLETS=$OverlayTripletDir"
    $cmakeArgs += '-DSTATIC_BUILD_DO_NOT_USE=ON'
    Write-Host "  Static build enabled: STATIC_BUILD_DO_NOT_USE=ON, triplet=$Triplet"
}

if ($WithTests) {
    $cmakeArgs += '-DWITH_TESTS=ON'
}

# manpage generation (--man option) requires python3 + mistletoe; disable
# it if they are not available
$withMan = 'OFF'
if (Test-Command 'python.exe') {
    & python -c "import mistletoe" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  Installing mistletoe (needed for manpage generation)...'
        & python -m pip install --quiet mistletoe 2>$null
    }
    & python -c "import mistletoe" 2>$null
    if ($LASTEXITCODE -eq 0) { $withMan = 'ON' }
}
if ($withMan -eq 'OFF') {
    Write-Host '  python/mistletoe not available - building without --man option.'
}
$cmakeArgs += "-DWITH_MAN_OPTION=$withMan"

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }

# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------

Write-Step "Building ($Config)..."

$targets = @('mkdwarfs')
if ($buildFuse) { $targets += 'dwarfs-bin' } # CMake target name; output is dwarfs.exe

& cmake --build $BuildDir --config $Config --target @targets -j
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$BinDir = Join-Path $BuildDir $Config
$vcpkgBin = Join-Path $BuildDir "vcpkg_installed\$Triplet\bin"

# the tools need the vcpkg runtime DLLs (boost, zstd, ...) in static builds
# there are no runtime DLLs to worry about
if (-not $Static -and (Test-Path $vcpkgBin)) {
    $env:PATH = "$vcpkgBin;$env:PATH"
}

function Get-BinPath([string]$name) {
    $p = Join-Path $BinDir $name
    if (-not (Test-Path $p)) { throw "expected binary not found: $p" }
    return $p
}

$mkdwarfs = Get-BinPath 'mkdwarfs.exe'

# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------

Write-Step 'Build complete.'

# verify the modification is actually in this binary (--dedup-global is
# listed in the long help output)
$helpText = & $mkdwarfs -H 2>&1 | Out-String
if ($LASTEXITCODE -ne 0 -or $helpText -notmatch '--dedup-global') {
    throw "the built mkdwarfs does not seem to support --dedup-global!"
}
Write-Host '  --dedup-global support verified.' -ForegroundColor Green

Write-Host ""
Write-Host "Binaries:" -ForegroundColor Yellow
$binaryNames = @('mkdwarfs.exe')
if ($buildFuse) { $binaryNames += 'dwarfs.exe' }
foreach ($name in $binaryNames) {
    $p = Get-BinPath $name
    Write-Host ("  {0,-20} {1,10:N1} MB" -f $name, ((Get-Item $p).Length / 1MB))
}
Write-Host ""
if ($Static) {
    Write-Host "These are fully static binaries - no runtime DLLs needed." -ForegroundColor Green
} else {
    Write-Host "To run the tools, make sure the vcpkg runtime DLLs are in PATH:"
    Write-Host "  $vcpkgBin"
}

# ---------------------------------------------------------------------------
# smoke test
# ---------------------------------------------------------------------------

if (-not $SkipSmokeTest) {
    Write-Step 'Running smoke test (--dedup-global round trip)...'

    $smoke = Join-Path ([System.IO.Path]::GetTempPath()) "dwarfs-mod-smoke-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path "$smoke\in" | Out-Null

    $rng = [System.Random]::new(1234)
    $dup = New-Object byte[] (512 * 1024)
    $rng.NextBytes($dup)
    $filler = New-Object byte[] (512 * 1024)
    $rng.NextBytes($filler)

    # two files that share 512 KiB of embedded duplicate data
    $ms = [System.IO.MemoryStream]::new()
    $ms.Write($dup, 0, $dup.Length)
    $ms.Write($filler, 0, $filler.Length)
    [System.IO.File]::WriteAllBytes("$smoke\in\a.bin", $ms.ToArray())
    $ms2 = [System.IO.MemoryStream]::new()
    $ms2.Write($filler, 0, $filler.Length)
    $ms2.Write($dup, 0, $dup.Length)
    [System.IO.File]::WriteAllBytes("$smoke\in\b.bin", $ms2.ToArray())
    $ms.Dispose(); $ms2.Dispose()

    & $mkdwarfs -i "$smoke\in" -o "$smoke\image.dwarfs" -C null --no-history --no-progress --dedup-global 4k | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'smoke test: mkdwarfs failed' }

    if (-not (Test-Path -Path "$smoke\image.dwarfs")) {
        throw 'smoke test: image not created'
    }

    Write-Host '  Smoke test passed: --dedup-global build verified.' -ForegroundColor Green

    Remove-Item -Recurse -Force $smoke -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------------------
# optional test suite
# ---------------------------------------------------------------------------

if ($WithTests) {
    Write-Step 'Building and running test suite...'

    & cmake --build $BuildDir --config $Config --target tool_main_test dwarfs_unit_tests -j
    if ($LASTEXITCODE -ne 0) { throw 'test build failed' }

    $env:PATH = "$vcpkgBin;$env:PATH"

    & (Get-BinPath 'tool_main_test.exe') --gtest_filter='*global_dedup*'
    if ($LASTEXITCODE -ne 0) { throw 'global dedup tests failed' }

    & (Get-BinPath 'dwarfs_unit_tests.exe') --gtest_filter='cdc_chunker_test.*'
    if ($LASTEXITCODE -ne 0) { throw 'CDC chunker unit tests failed' }

    Write-Host '  Global dedup tests passed.' -ForegroundColor Green
}
