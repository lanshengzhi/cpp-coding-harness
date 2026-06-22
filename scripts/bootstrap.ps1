param(
    [switch]$NoBuild,
    [switch]$Test,
    [switch]$Release,
    [string]$VcpkgRoot
)

$ErrorActionPreference = "Stop"

function Require-Command($Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "required command '$Name' was not found on PATH"
    }
}

Require-Command git
Require-Command cmake

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    if (-not [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
        $VcpkgRoot = $env:VCPKG_ROOT
    } else {
        $VcpkgRoot = Join-Path $RepoRoot ".deps\vcpkg"
    }
}

if (-not [System.IO.Path]::IsPathRooted($VcpkgRoot)) {
    $VcpkgRoot = Join-Path $RepoRoot $VcpkgRoot
}
$VcpkgRoot = [System.IO.Path]::GetFullPath($VcpkgRoot)

if (-not (Test-Path (Join-Path $VcpkgRoot ".git"))) {
    New-Item -ItemType Directory -Force -Path (Split-Path $VcpkgRoot) | Out-Null
    Write-Host "Bootstrapping vcpkg into $VcpkgRoot"
    git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
}

$vcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path $vcpkgExe)) {
    Write-Host "Building vcpkg"
    & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
}

$env:VCPKG_ROOT = $VcpkgRoot

$preset = if ($Release) { "vcpkg-release" } else { "vcpkg" }

# If a prior system-package configure left a cache in build/, CMake will keep
# ignoring the vcpkg toolchain. Remove only the cache metadata, not build outputs.
$cacheFile = Join-Path $RepoRoot "build\CMakeCache.txt"
if ((Test-Path $cacheFile) -and -not (Select-String -Path $cacheFile -SimpleMatch "scripts/buildsystems/vcpkg.cmake" -Quiet)) {
    Write-Host "Removing stale non-vcpkg CMake cache from build/"
    Remove-Item -Force $cacheFile
    Remove-Item -Recurse -Force (Join-Path $RepoRoot "build\CMakeFiles") -ErrorAction SilentlyContinue
}

Push-Location $RepoRoot
try {
    Write-Host "Configuring with CMake preset '$preset'"
    cmake --preset $preset

    if (-not $NoBuild -or $Test) {
        Write-Host "Building with CMake preset '$preset'"
        cmake --build --preset $preset
    }

    if ($Test) {
        Write-Host "Running tests with CTest preset '$preset'"
        ctest --preset $preset
    }
} finally {
    Pop-Location
}

Write-Host "Bootstrap complete. VCPKG_ROOT=$env:VCPKG_ROOT"
