[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Rpx,
    [string]$BuildDir = "build/windows",
    [string]$GeneratedDir = "generated/ttt2",
    [switch]$Clean,
    [switch]$GameOnly,
    [switch]$FullAot,
    [string]$OutputExe = ""
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

$GitExe = Resolve-ToolPath "git.exe" @(
    "C:\Program Files\Git\cmd\git.exe"
)
$CMakeExe = Resolve-ToolPath "cmake.exe" @(
    "C:\Program Files\CMake\bin\cmake.exe",
    (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links\cmake.exe")
)
$NinjaExe = Resolve-ToolPath "ninja.exe" @(
    (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links\ninja.exe")
)
$ClangClExe = Resolve-ToolPath "clang-cl.exe" @(
    "C:\Program Files\LLVM\bin\clang-cl.exe"
)
$LlvmRcExe = Resolve-ToolPath "llvm-rc.exe" @(
    "C:\Program Files\LLVM\bin\llvm-rc.exe"
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
    $PythonRoot = Join-Path $env:LOCALAPPDATA "Programs\Python"
    if (Test-Path -LiteralPath $PythonRoot) {
        $PythonCandidates = Get-ChildItem -Path $PythonRoot -Filter python.exe -File -Recurse -ErrorAction SilentlyContinue | Sort-Object FullName -Descending
        if ($PythonCandidates) { $PythonExe = $PythonCandidates[0].FullName }
    }
}
if (-not $PythonExe) {
    throw "Python 3 was not found."
}

# Keep Windows system tools visible regardless of what other installers put in PATH.
$ToolDirs = @(
    $PowerShellDir,
    $System32,
    (Split-Path -Parent $GitExe),
    (Split-Path -Parent $CMakeExe),
    (Split-Path -Parent $NinjaExe),
    (Split-Path -Parent $ClangClExe),
    (Split-Path -Parent $LlvmRcExe),
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
Write-Host "    LLVM RC: $LlvmRcExe"
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
$ExpectedRpxSha256 = "fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019"
$ActualRpxSha256 = (Get-FileHash -LiteralPath $RpxPath -Algorithm SHA256).Hash.ToLowerInvariant()

if ($ActualRpxSha256 -ne $ExpectedRpxSha256) {
    throw @"
Unsupported Tekken.rpx.

This port requires the European v16 executable:
  Base title ID:   000500001010F800
  Update title ID: 0005000E1010F800
  Expected SHA256: $ExpectedRpxSha256
  Your SHA256:     $ActualRpxSha256
"@
}

Write-Host "==> RPX verified: European v16"

# Build Windows resources. Launcher artwork is embedded independently of
# the optional Wii U icon, so WUA/extracted layouts cannot lose the Home art.
$WindowsResource = $null
$TitleRoot = Split-Path -Parent (Split-Path -Parent $RpxPath)
$IconTga = Join-Path $TitleRoot "meta\iconTex.tga"
$WindowsAssets = Join-Path $Root "generated\windows"
New-Item -ItemType Directory -Force $WindowsAssets | Out-Null
$WindowsRc = Join-Path $WindowsAssets "ttt2.rc"
$WindowsResourceCandidate = Join-Path $WindowsAssets "ttt2.res"
$ResourceLines = @()

if (Test-Path -LiteralPath $IconTga) {
    $WindowsIcon = Join-Path $WindowsAssets "ttt2.ico"
    Write-Host "==> Generating Windows executable icon from meta\iconTex.tga"
    & $PythonExe (Join-Path $Root "launcher\make_windows_icon.py") $IconTga $WindowsIcon
    if ($LASTEXITCODE -ne 0) { throw "Windows icon generation failed" }
    $IconForRc = $WindowsIcon.Replace("\", "/")
    $ResourceLines += "1 ICON `"$IconForRc`""
} else {
    Write-Warning "meta\iconTex.tga was not found next to the selected RPX; executables will use the default Windows icon."
}

$LauncherTheme = Join-Path $Root "launcher\assets\ttt2_launcher_theme.jpg"
if (Test-Path -LiteralPath $LauncherTheme) {
    $ThemeForRc = $LauncherTheme.Replace("\", "/")
    $ResourceLines += "101 RCDATA `"$ThemeForRc`""
    Write-Host "==> Embedding high-quality launcher artwork"
}

if ($ResourceLines.Count -gt 0) {
    Set-Content -LiteralPath $WindowsRc -Encoding ASCII -Value $ResourceLines
    Write-Host "==> Compiling Windows launcher resources"
    & $LlvmRcExe "/fo" $WindowsResourceCandidate $WindowsRc
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $WindowsResourceCandidate)) {
        throw "Windows resource compilation failed"
    }
    $WindowsResource = $WindowsResourceCandidate
}

$BuildPath = Join-Path $Root $BuildDir
$GeneratedPath = Join-Path $Root $GeneratedDir
if ($FullAot -and $GeneratedDir -eq "generated/ttt2") {
    # Keep complete release code separate from the older reachable-only output.
    # This avoids Windows locks/read-only attributes on partial shard files and
    # prevents a stale 79k/205k tree from contaminating a 205k/205k build.
    $GeneratedPath = Join-Path $Root "generated\ttt2-full"
}

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
if ($WindowsResource) {
    $WindowsResourceForCMake = $WindowsResource.Replace([char]92, [char]47)
    $CommonArgs += "-DTTT2_WINDOWS_RESOURCE=$WindowsResourceForCMake"
}

# A previous launcher revision set CMAKE_RC_COMPILER and CMake may have
# invalidated the configure cache. Remove only the cache/Ninja metadata; keep
# CMakeFiles/*/*.obj and all generated game code intact.
$CacheFile = Join-Path $BuildPath "CMakeCache.txt"
if (Test-Path -LiteralPath $CacheFile) {
    $CacheText = Get-Content -LiteralPath $CacheFile -Raw -ErrorAction SilentlyContinue
    if ($CacheText -match 'CMAKE_RC_COMPILER') {
        Write-Host "==> Repairing CMake cache from the old RC compiler configuration"
        Remove-Item -LiteralPath $CacheFile -Force
        foreach ($Name in @("build.ninja", "rules.ninja", "cmake_install.cmake")) {
            $Metadata = Join-Path $BuildPath $Name
            if (Test-Path -LiteralPath $Metadata) { Remove-Item -LiteralPath $Metadata -Force }
        }
    }
}

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
$recompArgs = @($RpxPath, $GeneratedPath)
if ($FullAot) {
    Write-Host "==> Full AOT enabled: compiling all guest function entries"
    $recompArgs += "--all"
}
& $Recompiler @recompArgs
if ($LASTEXITCODE -ne 0) {
    throw "cafe-recomp failed"
}

Write-Host "==> Configuring final game executable"
& $CMakeExe @CommonArgs "-DTTT2_GENERATED_DIR=$GeneratedPath"
if ($LASTEXITCODE -ne 0) {
    throw "Final CMake configure failed"
}

if ($GameOnly) {
    Write-Host "==> Building ttt2.exe"
    & $CMakeExe --build $BuildPath --target ttt2
} else {
    Write-Host "==> Building static ttt2.exe and Windows launchers"
    & $CMakeExe --build $BuildPath --target ttt2 ttt2_launcher ttt2_play
}
if ($LASTEXITCODE -ne 0) {
    throw "Final game build failed"
}

$Exe = Join-Path $BuildPath "ttt2.exe"
if (-not (Test-Path -LiteralPath $Exe)) {
    throw "Build completed but ttt2.exe was not found at $Exe"
}

$FinalExe = $Exe
if ($OutputExe) {
    $OutputPath = [System.IO.Path]::GetFullPath($OutputExe)
    $OutputParent = Split-Path -Parent $OutputPath
    if ($OutputParent) { New-Item -ItemType Directory -Force $OutputParent | Out-Null }
    Copy-Item -LiteralPath $Exe -Destination $OutputPath -Force
    $FinalExe = $OutputPath
}

$LauncherRoot = $null
$QuickPlayRoot = $null
if (-not $GameOnly) {
    $LauncherBuilt = Join-Path $BuildPath "Tekken Tag Tournament 2.exe"
    $QuickPlayBuilt = Join-Path $BuildPath "Play Tekken Tag Tournament 2.exe"
    if (-not (Test-Path -LiteralPath $LauncherBuilt)) {
        throw "Build completed but Tekken Tag Tournament 2.exe was not found at $LauncherBuilt"
    }
    if (-not (Test-Path -LiteralPath $QuickPlayBuilt)) {
        throw "Build completed but Play Tekken Tag Tournament 2.exe was not found at $QuickPlayBuilt"
    }
    $LauncherRoot = Join-Path $Root "Tekken Tag Tournament 2.exe"
    $QuickPlayRoot = Join-Path $Root "Play Tekken Tag Tournament 2.exe"
    Copy-Item -LiteralPath $LauncherBuilt -Destination $LauncherRoot -Force
    Copy-Item -LiteralPath $QuickPlayBuilt -Destination $QuickPlayRoot -Force
}

Write-Host ""
Write-Host "SUCCESS"
Write-Host "Static runtime: $FinalExe"
if ($LauncherRoot) { Write-Host "Launcher:       $LauncherRoot" }
if ($QuickPlayRoot) { Write-Host "Quick play:     $QuickPlayRoot" }
Write-Host "This build is ready for player packaging; end users do not run the recompiler."
