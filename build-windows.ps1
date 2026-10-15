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

function Resolve-ToolPath {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string[]]$Candidates = @()
    )

    $Command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($Command -and $Command.Source) {
        return $Command.Source
    }

    foreach ($Candidate in $Candidates) {
        if ($Candidate -and (Test-Path -LiteralPath $Candidate)) {
            return (Resolve-Path -LiteralPath $Candidate).Path
        }
    }

    throw "Missing required command: $Name"
}

$System32 = Join-Path $env:SystemRoot "System32"
$PowerShellDir = Join-Path $System32 "WindowsPowerShell\v1.0"
$PowerShellExe = Join-Path $PowerShellDir "powershell.exe"

if (-not (Test-Path -LiteralPath $PowerShellExe)) {
    throw "Windows PowerShell 5.1 was not found at $PowerShellExe"
}

$OriginalPath = $env:Path

$GitExe = Resolve-ToolPath "git.exe"
$CMakeExe = Resolve-ToolPath "cmake.exe"
$NinjaExe = Resolve-ToolPath "ninja.exe"
$ClangClExe = Resolve-ToolPath "clang-cl.exe" @(
    "C:\Program Files\LLVM\bin\clang-cl.exe"
)

$PythonExe = $null
$Py = Get-Command "py.exe" -ErrorAction SilentlyContinue
if ($Py -and $Py.Source) {
    $PythonExe = $Py.Source
} else {
    $Python = Get-Command "python.exe" -ErrorAction SilentlyContinue
    if ($Python -and $Python.Source) {
        $PythonExe = $Python.Source
    }
}
if (-not $PythonExe) {
    throw "Python 3 was not found. Install Python and make python.exe available."
}

# Keep Windows system tools visible regardless of what other installers put in PATH.
$ToolDirs = @(
    $PowerShellDir,
    $System32,
    (Split-Path -Parent $GitExe),
    (Split-Path -Parent $CMakeExe),
    (Split-Path -Parent $NinjaExe),
    (Split-Path -Parent $ClangClExe),
    (Split-Path -Parent $PythonExe)
)
$PathParts = @($ToolDirs + ($OriginalPath -split ';')) |
    Where-Object { $_ -and (Test-Path -LiteralPath $_) } |
    Select-Object -Unique
$env:Path = $PathParts -join ';'

Write-Host "==> Toolchain"
Write-Host "    CMake:   $CMakeExe"
Write-Host "    Ninja:   $NinjaExe"
Write-Host "    ClangCL: $ClangClExe"
Write-Host "    Python:  $PythonExe"

# Verify that the Microsoft C++ Build Tools are installed. clang-cl can locate
# their headers/libraries through the normal Visual Studio installation metadata;
# we deliberately do not import VsDevCmd.bat because it can rewrite PATH.
$ProgramFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
$VsWhere = if ($ProgramFilesX86) {
    Join-Path $ProgramFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
} else {
    $null
}
if (-not $VsWhere -or -not (Test-Path -LiteralPath $VsWhere)) {
    throw "Visual Studio Build Tools were not found (vswhere.exe is missing)."
}
$VsInstall = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsInstall) {
    throw "Visual Studio C++ Build Tools are not installed."
}
Write-Host "    VS tools: $VsInstall"

$RpxPath = (Resolve-Path -LiteralPath $Rpx).Path
$BaseV16RpxSha256 = "fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019"
$UpdateV32RpxSha256 = "c3afc72f30aa1af84901bfc0775aeae7a359d396923a387cfbe71034175886ab"
$ActualRpxSha256 = (Get-FileHash -LiteralPath $RpxPath -Algorithm SHA256).Hash.ToLowerInvariant()

if ($ActualRpxSha256 -eq $BaseV16RpxSha256) {
    $RpxVersion = "European base v16"
} elseif ($ActualRpxSha256 -eq $UpdateV32RpxSha256) {
    $RpxVersion = "European update v32"
} else {
    throw @"
Unsupported Tekken.rpx.

Known European executables:
  Base v16 SHA256:   $BaseV16RpxSha256
  Update v32 SHA256: $UpdateV32RpxSha256
  Your SHA256:       $ActualRpxSha256
"@
}

Write-Host "==> RPX verified: $RpxVersion"

$BuildPath = Join-Path $Root $BuildDir
$GeneratedPath = Join-Path $Root $GeneratedDir

if ($Clean -and (Test-Path -LiteralPath $BuildPath)) {
    Write-Host "==> Cleaning previous CMake build"
    Remove-Item -Recurse -Force -LiteralPath $BuildPath
}

Write-Host "==> Fetching pinned project dependencies"
& $PythonExe (Join-Path $Root "tools\setup_deps.py")
if ($LASTEXITCODE -ne 0) {
    throw "tools/setup_deps.py failed"
}

$VcpkgBaseline = "c52f7bd748185ef0e0daf033d9f2ddfc8a3449ea"
$VcpkgRoot = Join-Path $Root ".tools\vcpkg"
$VcpkgGit = Join-Path $VcpkgRoot ".git"
$VcpkgHeadFile = Join-Path $VcpkgGit "HEAD"

if (-not (Test-Path -LiteralPath $VcpkgGit)) {
    New-Item -ItemType Directory -Force $VcpkgRoot | Out-Null
    Write-Host "==> Initializing pinned vcpkg"
    & $GitExe -C $VcpkgRoot init --quiet
    if ($LASTEXITCODE -ne 0) { throw "vcpkg git init failed" }
    & $GitExe -C $VcpkgRoot remote add origin https://github.com/microsoft/vcpkg
    if ($LASTEXITCODE -ne 0) { throw "vcpkg git remote setup failed" }
}

$CurrentVcpkg = ""
if (Test-Path -LiteralPath $VcpkgHeadFile) {
    $HeadText = (Get-Content -LiteralPath $VcpkgHeadFile -Raw).Trim()
    if ($HeadText -match '^[0-9a-fA-F]{40}$') {
        $CurrentVcpkg = $HeadText.ToLowerInvariant()
    }
}

if ($CurrentVcpkg -ne $VcpkgBaseline) {
    Write-Host "==> Fetching pinned vcpkg $VcpkgBaseline"
    & $GitExe -C $VcpkgRoot fetch --depth 1 origin $VcpkgBaseline
    if ($LASTEXITCODE -ne 0) { throw "vcpkg fetch failed" }
    & $GitExe -C $VcpkgRoot checkout --detach FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw "vcpkg checkout failed" }
} else {
    Write-Host "==> Pinned vcpkg already present"
}

$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path -LiteralPath $VcpkgExe)) {
    $BootstrapPs1 = Join-Path $VcpkgRoot "scripts\bootstrap.ps1"
    if (-not (Test-Path -LiteralPath $BootstrapPs1)) {
        throw "vcpkg bootstrap script was not found at $BootstrapPs1"
    }

    Write-Host "==> Bootstrapping vcpkg directly with Windows PowerShell"
    & $PowerShellExe -NoProfile -ExecutionPolicy Bypass -File $BootstrapPs1 -disableMetrics
    if ($LASTEXITCODE -ne 0) {
        throw "vcpkg bootstrap failed"
    }
}

if (-not (Test-Path -LiteralPath $VcpkgExe)) {
    throw "vcpkg.exe was not produced."
}

$env:VCPKG_ROOT = $VcpkgRoot
$VcpkgCache = Join-Path $Root ".vcpkg-cache"
New-Item -ItemType Directory -Force $VcpkgCache | Out-Null
$env:VCPKG_BINARY_SOURCES = "clear;files,$VcpkgCache,readwrite"
$env:VCPKG_DISABLE_METRICS = "1"

$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $Toolchain)) {
    throw "vcpkg CMake toolchain was not found."
}

$CommonArgs = @(
    "-S", $Root,
    "-B", $BuildPath,
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$NinjaExe",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=$ClangClExe",
    "-DCMAKE_CXX_COMPILER=$ClangClExe",
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static"
)

Write-Host "==> Configuring Windows host tools"
& $CMakeExe @CommonArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed"
}

Write-Host "==> Building cafe-recomp and host runtime"
& $CMakeExe --build $BuildPath --target cafe-recomp cafe_runtime
if ($LASTEXITCODE -ne 0) {
    throw "Host tools build failed"
}

$Recompiler = Join-Path $BuildPath "recomp\cafe-recomp.exe"
if (-not (Test-Path -LiteralPath $Recompiler)) {
    throw "cafe-recomp.exe was not produced at $Recompiler"
}

New-Item -ItemType Directory -Force $GeneratedPath | Out-Null

Write-Host "==> Generating local C++ from your RPX"
& $Recompiler $RpxPath $GeneratedPath
if ($LASTEXITCODE -ne 0) {
    throw "cafe-recomp failed"
}

Write-Host "==> Configuring final game executable"
& $CMakeExe @CommonArgs "-DTTT2_GENERATED_DIR=$GeneratedPath"
if ($LASTEXITCODE -ne 0) {
    throw "Final CMake configure failed"
}

Write-Host "==> Building ttt2.exe"
& $CMakeExe --build $BuildPath --target ttt2
if ($LASTEXITCODE -ne 0) {
    throw "Final ttt2 build failed"
}

$Exe = Join-Path $BuildPath "ttt2.exe"
if (-not (Test-Path -LiteralPath $Exe)) {
    throw "Build completed but ttt2.exe was not found at $Exe"
}

Write-Host ""
Write-Host "SUCCESS"
Write-Host "Executable: $Exe"
Write-Host "Run it with your own .wua, or with extracted BASE and optional UPDATE folders."
