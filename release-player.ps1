[CmdletBinding()]
param(
    [string]$Version = "",
    [switch]$Publish,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") { throw "This player release builder must run on Windows." }

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

if (-not $SkipBuild) {
    $CMake = Need-Command "cmake.exe"

    # Re-use the configured Windows build tree, but make sure the generic
    # zero-install runtime target is enabled. No game RPX is required here.
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDir "CMakeCache.txt") -PathType Leaf)) {
        throw "build\windows is not configured. Configure the Windows host build first."
    }

    Write-Host "==> Enabling generic zero-install runtime"
    & $CMake -S $Root -B $BuildDir -DTTT2_GENERATED_DIR= -DTTT2_EXPERIMENTAL_DYNAMIC_HOST=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake reconfigure failed with exit code $LASTEXITCODE" }

    Write-Host "==> Building zero-install Windows Player"
    & $CMake --build $BuildDir --target ttt2_dynamic ttt2_player_launcher ttt2_play
    if ($LASTEXITCODE -ne 0) { throw "Player build failed with exit code $LASTEXITCODE" }
}

foreach ($required in @($LauncherExe, $QuickPlayExe, $RuntimeExe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required zero-install Player file is missing: $required"
    }
}

$DistRoot = Join-Path $Root "dist"
$PackageRoot = Join-Path $DistRoot "ttt2-windows-player"
$BinDir = Join-Path $PackageRoot "bin"
$UserDir = Join-Path $PackageRoot "user"
$OutDir = Join-Path $PackageRoot "out"

$Git = Get-Command git.exe -ErrorAction SilentlyContinue
$Commit = if ($Git) { (& git rev-parse HEAD).Trim() } else { "unknown" }

if (Test-Path -LiteralPath $PackageRoot) {
    Remove-Item -Recurse -Force -LiteralPath $PackageRoot
}
New-Item -ItemType Directory -Force $PackageRoot, $BinDir, $UserDir, $OutDir | Out-Null

Copy-Item -LiteralPath $LauncherExe -Destination (Join-Path $PackageRoot "Tekken Tag Tournament 2.exe") -Force
Copy-Item -LiteralPath $QuickPlayExe -Destination (Join-Path $PackageRoot "Play Tekken Tag Tournament 2.exe") -Force
Copy-Item -LiteralPath $RuntimeExe -Destination (Join-Path $BinDir "ttt2-runtime.exe") -Force

foreach ($file in @("LICENSE")) {
    $source = Join-Path $Root $file
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $PackageRoot $file) -Force
    }
}

$ThemeSource = Join-Path $Root "launcher\assets\ttt2_launcher_theme.jpg"
if (Test-Path -LiteralPath $ThemeSource -PathType Leaf) {
    $ThemeDest = Join-Path $PackageRoot "launcher\assets"
    New-Item -ItemType Directory -Force $ThemeDest | Out-Null
    Copy-Item -LiteralPath $ThemeSource -Destination (Join-Path $ThemeDest "ttt2_launcher_theme.jpg") -Force
}

# The Player is intentionally game-independent. It ships our generic runtime,
# never the user's RPX or generated Full-AOT output.
$forbidden = @(
    (Join-Path $PackageRoot "code\Tekken.rpx"),
    (Join-Path $PackageRoot "generated"),
    (Join-Path $PackageRoot "tools\prepare-player.ps1"),
    (Join-Path $PackageRoot "tools\source")
)
foreach ($path in $forbidden) {
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to create zero-install Player ZIP because an unwanted build/game file was found: $path"
    }
}

@"
TTT2 Recompiled - Windows Player

1. Extract this ZIP to a normal folder.
2. Open "Tekken Tag Tournament 2.exe".
3. Choose your own extracted EU v16 game folder (the folder containing code\Tekken.rpx).
4. Press PLAY.

That is all. This release already includes the generic Windows runtime at:
  bin\ttt2-runtime.exe

The player does NOT install CMake, Ninja, LLVM, Visual Studio, Python, Git or vcpkg.
The player does NOT perform a Full-AOT compilation before the game starts.

The runtime reads the supported Tekken.rpx from your selected game directory and executes
PowerPC code through the built-in dynamic host. Native x64 block JIT and persistent code
caches are produced automatically while the game runs.

No Tekken game files are included in this ZIP.
"@ | Set-Content -LiteralPath (Join-Path $PackageRoot "README-PLAYER.txt") -Encoding UTF8

Set-Content -LiteralPath (Join-Path $UserDir "README.txt") -Encoding UTF8 -Value "Settings, saves and runtime logs are stored here."
Set-Content -LiteralPath (Join-Path $OutDir "README.txt") -Encoding UTF8 -Value "Persistent code and renderer caches are stored here."

@"
TTT2 Recompiled Windows Player
Version: $Version
Git commit: $Commit
Package type: zero-install generic Windows runtime
Supported game: TEKKEN TAG TOURNAMENT 2 Wii U EDITION EU v16
Expected Tekken.rpx SHA-256: fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019
Generic runtime included: YES (bin\ttt2-runtime.exe)
Game-derived Full-AOT runtime included: NO
Compiler/toolchain required by player: NO
"@ | Set-Content -LiteralPath (Join-Path $PackageRoot "BUILD-INFO.txt") -Encoding UTF8

New-Item -ItemType Directory -Force $DistRoot | Out-Null
$Zip = Join-Path $DistRoot "TTT2-Windows-$Version.zip"
if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }

Write-Host "==> Creating $([IO.Path]::GetFileName($Zip))"
Compress-Archive -Path (Join-Path $PackageRoot "*") -DestinationPath $Zip -CompressionLevel Optimal

$Checksums = Join-Path $DistRoot "SHA256SUMS-PLAYER.txt"
"$((Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($Zip))" |
    Set-Content -LiteralPath $Checksums -Encoding ASCII

$Notes = Join-Path $DistRoot "player-release-notes.md"
@"
## TTT2 Recompiled Windows $Version

Download **TTT2-Windows-$Version.zip**, extract it, open **Tekken Tag Tournament 2.exe**,
choose your own extracted EU v16 game folder and press **PLAY**.

This is a zero-install Player build. The generic Windows runtime is already included, so the
player does not install compilers and does not perform a Full-AOT compilation on first launch.

The runtime validates the selected Tekken.rpx and uses the built-in dynamic PowerPC host with
optimized dynamic block execution and persistent caches while the game runs.

No game files are included in the download.
"@ | Set-Content -LiteralPath $Notes -Encoding UTF8

Write-Host ""
Write-Host "ZERO-INSTALL PLAYER PACKAGE READY"
Write-Host "  Runtime: bin\ttt2-runtime.exe"
Write-Host "  ZIP:     $Zip"
Write-Host "  Hashes:  $Checksums"

if ($Publish) {
    $Gh = Need-Command "gh.exe"
    & $Gh auth status
    if ($LASTEXITCODE -ne 0) { throw "GitHub CLI is not authenticated." }

    $Repo = "xruanzin/Tekken-Tag-2-Recompiled"

    $PreviousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $Gh release view $Version --repo $Repo --json tagName 2>$null | Out-Null
        $ReleaseViewExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $PreviousErrorActionPreference
    }
    $Exists = ($ReleaseViewExitCode -eq 0)

    if ($Exists) {
        & $Gh release upload $Version $Zip $Checksums --repo $Repo --clobber
        if ($LASTEXITCODE -ne 0) { throw "gh release upload failed" }
        & $Gh release edit $Version --repo $Repo --title "TTT2 Recompiled Windows $Version" --notes-file $Notes
        if ($LASTEXITCODE -ne 0) { throw "gh release edit failed" }
    } else {
        & $Gh release create $Version $Zip $Checksums --repo $Repo --target windows-host --title "TTT2 Recompiled Windows $Version" --notes-file $Notes
        if ($LASTEXITCODE -ne 0) { throw "gh release create failed" }
    }
    Write-Host "Published zero-install Player ZIP: https://github.com/$Repo/releases/tag/$Version"
}
