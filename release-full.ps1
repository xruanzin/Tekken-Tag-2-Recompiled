[CmdletBinding()]
param(
    [string]$Version = "",
    [switch]$Publish,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") { throw "This full package builder must run on Windows." }

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

function Need-Command {
    param([Parameter(Mandatory = $true)][string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }
    if ($Name -ieq "gh.exe") {
        foreach ($candidate in @(
            (Join-Path $env:ProgramFiles "GitHub CLI\gh.exe"),
            (Join-Path $env:LOCALAPPDATA "Programs\GitHub CLI\gh.exe")
        )) {
            if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { return $candidate }
        }
    }
    throw "Missing required command: $Name"
}

if (-not $Version) {
    $VersionFile = Join-Path $Root ".github\release-version.txt"
    if (Test-Path -LiteralPath $VersionFile) {
        $Version = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
    }
}
if (-not $Version) { throw "Pass -Version vX.Y.Z or set .github\release-version.txt." }
if ($Version -notmatch '^v[0-9]+\.[0-9]+\.[0-9]+[-A-Za-z0-9.]*$') {
    throw "Invalid release version: $Version"
}

$BuildDir = Join-Path $Root "build\windows"
$LauncherExe = Join-Path $BuildDir "Tekken Tag Tournament 2 Player.exe"
$QuickPlayExe = Join-Path $BuildDir "Play Tekken Tag Tournament 2.exe"
$RuntimeExe = Join-Path $BuildDir "ttt2-runtime.exe"
$CafeRecompExe = Join-Path $BuildDir "recomp\cafe-recomp.exe"
$CafeCensusExe = Join-Path $BuildDir "recomp\cafe-census.exe"
$CafeSemgenExe = Join-Path $BuildDir "recomp\cafe-semgen.exe"
$CafeShaderExe = Join-Path $BuildDir "runtime\cafe-shader.exe"

if (-not $SkipBuild) {
    $CMake = Need-Command "cmake.exe"
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDir "CMakeCache.txt") -PathType Leaf)) {
        throw "build\windows is not configured. Configure the Windows host build first."
    }

    Write-Host "==> Building complete Windows package"
    & $CMake --build $BuildDir --target ttt2_dynamic ttt2_player_launcher ttt2_play cafe-recomp cafe-census cafe-semgen cafe-shader
    if ($LASTEXITCODE -ne 0) { throw "Full package build failed with exit code $LASTEXITCODE" }
}

$Required = @(
    $LauncherExe,
    $QuickPlayExe,
    $RuntimeExe,
    $CafeRecompExe,
    $CafeCensusExe,
    $CafeSemgenExe,
    $CafeShaderExe
)
foreach ($required in $Required) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required compiled file is missing: $required"
    }
}

$DistRoot = Join-Path $Root "dist"
$PackageRoot = Join-Path $DistRoot "ttt2-windows-full"
$BinDir = Join-Path $PackageRoot "bin"
$ToolsDir = Join-Path $PackageRoot "tools"
$UserDir = Join-Path $PackageRoot "user"
$OutDir = Join-Path $PackageRoot "out"

if (Test-Path -LiteralPath $PackageRoot) {
    Remove-Item -Recurse -Force -LiteralPath $PackageRoot
}
New-Item -ItemType Directory -Force $PackageRoot, $BinDir, $ToolsDir, $UserDir, $OutDir | Out-Null

Copy-Item -LiteralPath $LauncherExe -Destination (Join-Path $PackageRoot "Tekken Tag Tournament 2.exe") -Force
Copy-Item -LiteralPath $QuickPlayExe -Destination (Join-Path $PackageRoot "Play Tekken Tag Tournament 2.exe") -Force
Copy-Item -LiteralPath $RuntimeExe -Destination (Join-Path $BinDir "ttt2-runtime.exe") -Force
Copy-Item -LiteralPath $CafeRecompExe -Destination (Join-Path $ToolsDir "cafe-recomp.exe") -Force
Copy-Item -LiteralPath $CafeCensusExe -Destination (Join-Path $ToolsDir "cafe-census.exe") -Force
Copy-Item -LiteralPath $CafeSemgenExe -Destination (Join-Path $ToolsDir "cafe-semgen.exe") -Force
Copy-Item -LiteralPath $CafeShaderExe -Destination (Join-Path $ToolsDir "cafe-shader.exe") -Force

$License = Join-Path $Root "LICENSE"
if (Test-Path -LiteralPath $License -PathType Leaf) {
    Copy-Item -LiteralPath $License -Destination (Join-Path $PackageRoot "LICENSE") -Force
}

$ThemeSource = Join-Path $Root "launcher\assets\ttt2_launcher_theme.jpg"
if (Test-Path -LiteralPath $ThemeSource -PathType Leaf) {
    $ThemeDest = Join-Path $PackageRoot "launcher\assets"
    New-Item -ItemType Directory -Force $ThemeDest | Out-Null
    Copy-Item -LiteralPath $ThemeSource -Destination (Join-Path $ThemeDest "ttt2_launcher_theme.jpg") -Force
}

$Forbidden = @(
    (Join-Path $PackageRoot "code\Tekken.rpx"),
    (Join-Path $PackageRoot "generated"),
    (Join-Path $PackageRoot "tools\source"),
    (Join-Path $PackageRoot "tools\prepare-player.ps1")
)
foreach ($path in $Forbidden) {
    if (Test-Path -LiteralPath $path) {
        throw "Unwanted game/source/build file found in Full package: $path"
    }
}

@"
TTT2 Recompiled - Windows Full Package

This is the complete precompiled Windows package.

1. Extract the ZIP to an empty folder.
2. Open "Tekken Tag Tournament 2.exe".
3. Select your own extracted EU v16 game folder containing code\Tekken.rpx.
4. Press PLAY.

Everything required from this project is already compiled and included, including:
  bin\ttt2-runtime.exe
  Tekken Tag Tournament 2.exe
  Play Tekken Tag Tournament 2.exe
  tools\cafe-recomp.exe
  tools\cafe-census.exe
  tools\cafe-semgen.exe
  tools\cafe-shader.exe
  launcher\assets\ttt2_launcher_theme.jpg

The player does not install or require CMake, Ninja, LLVM, Visual Studio Build Tools,
Python, Git or vcpkg.

No Tekken game files are included. The user supplies their own supported EU v16 dump.
"@ | Set-Content -LiteralPath (Join-Path $PackageRoot "README-FULL.txt") -Encoding UTF8

Set-Content -LiteralPath (Join-Path $UserDir "README.txt") -Encoding UTF8 -Value "Settings, saves and runtime logs are stored here."
Set-Content -LiteralPath (Join-Path $OutDir "README.txt") -Encoding UTF8 -Value "Persistent code and renderer caches are stored here."

$Git = Get-Command git.exe -ErrorAction SilentlyContinue
$Commit = if ($Git) { (& git rev-parse HEAD).Trim() } else { "unknown" }

@"
TTT2 Recompiled Windows Full
Version: $Version
Git commit: $Commit
Package type: complete precompiled Windows package
Runtime included: YES (bin\ttt2-runtime.exe)
Game files included: NO
Source/toolchain included: NO
Compiled launchers included: YES
Compiled project utilities included: YES
"@ | Set-Content -LiteralPath (Join-Path $PackageRoot "BUILD-INFO.txt") -Encoding UTF8

New-Item -ItemType Directory -Force $DistRoot | Out-Null
$Zip = Join-Path $DistRoot "TTT2-Windows-Full-$Version.zip"
if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }

Write-Host "==> Creating $([IO.Path]::GetFileName($Zip))"
Compress-Archive -Path (Join-Path $PackageRoot "*") -DestinationPath $Zip -CompressionLevel Optimal

$Checksums = Join-Path $DistRoot "SHA256SUMS-FULL.txt"
"$((Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($Zip))" |
    Set-Content -LiteralPath $Checksums -Encoding ASCII

Write-Host ""
Write-Host "COMPLETE FULL PACKAGE READY"
Write-Host "  Runtime: bin\ttt2-runtime.exe"
Write-Host "  ZIP:     $Zip"
Write-Host "  Hashes:  $Checksums"

if ($Publish) {
    $Gh = Need-Command "gh.exe"
    & $Gh auth status
    if ($LASTEXITCODE -ne 0) { throw "GitHub CLI is not authenticated." }

    $Repo = "xruanzin/Tekken-Tag-2-Recompiled"
    & $Gh release view $Version --repo $Repo --json tagName | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Release $Version does not exist yet. Publish the normal Player release first."
    }

    # Remove the obsolete no-runtime assets so users see one clear Full package.
    foreach ($oldAsset in @(
        "TTT2-Windows-Full-NoRuntime-$Version.zip",
        "SHA256SUMS-FULL-NORUNTIME.txt"
    )) {
        & $Gh release delete-asset $Version $oldAsset --repo $Repo --yes 2>$null
    }

    & $Gh release upload $Version $Zip $Checksums --repo $Repo --clobber
    if ($LASTEXITCODE -ne 0) { throw "gh release upload failed" }

    $Notes = Join-Path $DistRoot "full-release-notes.md"
@"
## TTT2 Recompiled Windows $Version

### Recommended download
Download **TTT2-Windows-Full-$Version.zip**.

It contains the launcher, the precompiled Windows runtime, the compiled project tools and the required project assets.
Extract it, open **Tekken Tag Tournament 2.exe**, choose your own extracted EU v16 game folder and press **PLAY**.

The runtime is already included at **bin\ttt2-runtime.exe**.
No compiler or first-run build is required.

No Tekken game files are included.
"@ | Set-Content -LiteralPath $Notes -Encoding UTF8

    & $Gh release edit $Version --repo $Repo --title "TTT2 Recompiled Windows $Version" --notes-file $Notes
    if ($LASTEXITCODE -ne 0) { throw "gh release edit failed" }

    Write-Host "Published complete Full ZIP: https://github.com/$Repo/releases/tag/$Version"
}
