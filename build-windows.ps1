[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Rpx,
    [string]$BuildDir = "build/windows",
    [string]$GeneratedDir = "generated/ttt2",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

if ($env:OS -ne "Windows_NT") {
    throw "This script must be run on Windows."
}

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Missing required command: $Name"
    }
}

Require-Command git
Require-Command cmake
Require-Command ninja
Require-Command clang-cl
Require-Command py

$RpxPath = (Resolve-Path $Rpx).Path
$BuildPath = Join-Path $Root $BuildDir
$GeneratedPath = Join-Path $Root $GeneratedDir

if ($Clean -and (Test-Path $BuildPath)) {
    Remove-Item -Recurse -Force $BuildPath
}

Write-Host "==> Fetching pinned project dependencies"
& py tools/setup_deps.py
if ($LASTEXITCODE -ne 0) { throw "tools/setup_deps.py failed" }

if ($env:VCPKG_ROOT) {
    $VcpkgRoot = $env:VCPKG_ROOT
} else {
    $VcpkgRoot = Join-Path $Root ".tools/vcpkg"
    if (-not (Test-Path (Join-Path $VcpkgRoot ".git"))) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $VcpkgRoot) | Out-Null
        Write-Host "==> Cloning vcpkg"
        & git clone --depth 1 https://github.com/microsoft/vcpkg $VcpkgRoot
        if ($LASTEXITCODE -ne 0) { throw "vcpkg clone failed" }
    }
}

$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path $VcpkgExe)) {
    Write-Host "==> Bootstrapping vcpkg"
    & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }
}

$Toolchain = Join-Path $VcpkgRoot "scripts/buildsystems/vcpkg.cmake"
$CommonArgs = @(
    "-S", $Root,
    "-B", $BuildPath,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=clang-cl",
    "-DCMAKE_CXX_COMPILER=clang-cl",
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static"
)

Write-Host "==> Configuring Windows host tools"
& cmake @CommonArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

Write-Host "==> Building cafe-recomp and host runtime"
& cmake --build $BuildPath --target cafe-recomp cafe_runtime
if ($LASTEXITCODE -ne 0) { throw "Host tools build failed" }

$Recompiler = Join-Path $BuildPath "recomp/cafe-recomp.exe"
if (-not (Test-Path $Recompiler)) {
    throw "cafe-recomp.exe was not produced at $Recompiler"
}

New-Item -ItemType Directory -Force $GeneratedPath | Out-Null
Write-Host "==> Generating local C++ from your RPX"
& $Recompiler $RpxPath $GeneratedPath
if ($LASTEXITCODE -ne 0) { throw "cafe-recomp failed" }

Write-Host "==> Configuring final game executable"
& cmake @CommonArgs "-DTTT2_GENERATED_DIR=$GeneratedPath"
if ($LASTEXITCODE -ne 0) { throw "Final CMake configure failed" }

Write-Host "==> Building ttt2.exe"
& cmake --build $BuildPath --target ttt2
if ($LASTEXITCODE -ne 0) { throw "Final ttt2 build failed" }

$Exe = Join-Path $BuildPath "ttt2.exe"
if (-not (Test-Path $Exe)) {
    throw "Build completed but ttt2.exe was not found at $Exe"
}

Write-Host ""
Write-Host "SUCCESS"
Write-Host "Executable: $Exe"
Write-Host "Run it with your own .wua or extracted title path, for example:"
Write-Host ('  & "' + $Exe + '" "D:\Games\TEKKEN TAG 2 Wii U EDITION (EU).wua"')
