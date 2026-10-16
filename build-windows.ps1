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

# Build a Windows .ico from the user's own Wii U icon. The generated .ico/.rc
# stay under generated/ and are excluded from Git.
$WindowsResource = $null
$TitleRoot = Split-Path -Parent (Split-Path -Parent $RpxPath)
$IconTga = Join-Path $TitleRoot "meta\iconTex.tga"
if (Test-Path -LiteralPath $IconTga) {
    $WindowsAssets = Join-Path $Root "generated\windows"
    New-Item -ItemType Directory -Force $WindowsAssets | Out-Null
    $WindowsIcon = Join-Path $WindowsAssets "ttt2.ico"
    $WindowsResource = Join-Path $WindowsAssets "ttt2.rc"
    Write-Host "==> Generating Windows executable icon from meta\iconTex.tga"
    & $PythonExe (Join-Path $Root "tools\make_windows_icon.py") $IconTga $WindowsIcon
    if ($LASTEXITCODE -ne 0) {
        throw "Windows icon generation failed"
    }
    $IconForRc = $WindowsIcon.Replace("\", "/")
    Set-Content -LiteralPath $WindowsResource -Encoding ASCII -Value "1 ICON `"$IconForRc`""
} else {
    Write-Warning "meta\iconTex.tga was not found next to the selected RPX; executables will use the default Windows icon."
}

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
if ($WindowsResource) {
    # CMake writes CMAKE_RC_COMPILER into a generated .cmake file. Native
    # Windows backslashes there are parsed as CMake escapes (e.g. "\P"),
    # so always give CMake a forward-slash path.
    $LlvmRcForCMake = $LlvmRcExe.Replace("\\", "/")
    $WindowsResourceForCMake = $WindowsResource.Replace("\\", "/")
    $CommonArgs += "-DTTT2_WINDOWS_RESOURCE=$WindowsResourceForCMake"
    $CommonArgs += "-DCMAKE_RC_COMPILER=$LlvmRcForCMake"

    # Recover automatically from an older configure that cached an unescaped
    # llvm-rc path. Remove only the RC compiler metadata, never the compiled
    # game objects.
    if (Test-Path -LiteralPath $BuildPath) {
        Get-ChildItem -LiteralPath (Join-Path $BuildPath "CMakeFiles") -Directory -ErrorAction SilentlyContinue |
            ForEach-Object {
                $BadRcCache = Join-Path $_.FullName "CMakeRCCompiler.cmake"
                if (Test-Path -LiteralPath $BadRcCache) {
                    $RcText = Get-Content -LiteralPath $BadRcCache -Raw -ErrorAction SilentlyContinue
                    if ($RcText -match 'C:\\\\Program Files\\\\LLVM') {
                        Remove-Item -LiteralPath $BadRcCache -Force
                    }
                }
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
& $Recompiler $RpxPath $GeneratedPath
if ($LASTEXITCODE -ne 0) {
    throw "cafe-recomp failed"
}

Write-Host "==> Configuring final game executable"
& $CMakeExe @CommonArgs "-DTTT2_GENERATED_DIR=$GeneratedPath"
if ($LASTEXITCODE -ne 0) {
    throw "Final CMake configure failed"
}

Write-Host "==> Building ttt2.exe and Windows launcher"
& $CMakeExe --build $BuildPath --target ttt2 ttt2_launcher
if ($LASTEXITCODE -ne 0) {
    throw "Final ttt2/launcher build failed"
}

$Exe = Join-Path $BuildPath "ttt2.exe"
if (-not (Test-Path -LiteralPath $Exe)) {
    throw "Build completed but ttt2.exe was not found at $Exe"
}

$LauncherBuilt = Join-Path $BuildPath "TTT2 Launcher.exe"
if (-not (Test-Path -LiteralPath $LauncherBuilt)) {
    throw "Build completed but TTT2 Launcher.exe was not found at $LauncherBuilt"
}
$LauncherRoot = Join-Path $Root "TTT2 Launcher.exe"
Copy-Item -LiteralPath $LauncherBuilt -Destination $LauncherRoot -Force

Write-Host ""
Write-Host "SUCCESS"
Write-Host "Executable: $Exe"
Write-Host "Launcher:   $LauncherRoot"
Write-Host "Run the root launcher, or run ttt2.exe with your own .wua/extracted European v16 title folder."
