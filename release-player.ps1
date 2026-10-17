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
    if (Test-Path -LiteralPath $VersionFile) { $Version = (Get-Content -LiteralPath $VersionFile -Raw).Trim() }
}
if (-not $Version) { throw "Pass -Version vX.Y.Z or set .github\release-version.txt." }
if ($Version -notmatch '^v[0-9]+\.[0-9]+\.[0-9]+[-A-Za-z0-9.]*$') { throw "Invalid release version: $Version" }

$BuildDir = Join-Path $Root "build\windows"
$LauncherExe = Join-Path $BuildDir "Tekken Tag Tournament 2 Player.exe"
$QuickPlayExe = Join-Path $BuildDir "Play Tekken Tag Tournament 2.exe"

if (-not $SkipBuild) {
    $CMake = Need-Command "cmake.exe"
    Write-Host "==> Building generic player launchers"
    & $CMake --build $BuildDir --target ttt2_player_launcher ttt2_play
    if ($LASTEXITCODE -ne 0) { throw "Launcher build failed with exit code $LASTEXITCODE" }
}

foreach ($required in @($LauncherExe, $QuickPlayExe, (Join-Path $Root "tools\prepare-player.ps1"))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required player file is missing: $required"
    }
}

$DistRoot = Join-Path $Root "dist"
$PackageRoot = Join-Path $DistRoot "ttt2-windows-player"
$ToolsDir = Join-Path $PackageRoot "tools"
$SourceDir = Join-Path $ToolsDir "source"
$BinDir = Join-Path $PackageRoot "bin"
$UserDir = Join-Path $PackageRoot "user"
$OutDir = Join-Path $PackageRoot "out"

if (Test-Path -LiteralPath $PackageRoot) { Remove-Item -Recurse -Force -LiteralPath $PackageRoot }
New-Item -ItemType Directory -Force $PackageRoot, $ToolsDir, $SourceDir, $BinDir, $UserDir, $OutDir | Out-Null

Copy-Item -LiteralPath $LauncherExe -Destination (Join-Path $PackageRoot "Tekken Tag Tournament 2.exe") -Force
Copy-Item -LiteralPath $QuickPlayExe -Destination (Join-Path $PackageRoot "Play Tekken Tag Tournament 2.exe") -Force
Copy-Item -LiteralPath (Join-Path $Root "tools\prepare-player.ps1") -Destination (Join-Path $ToolsDir "prepare-player.ps1") -Force

foreach ($file in @("LICENSE")) {
    $source = Join-Path $Root $file
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $PackageRoot $file) -Force
    }
}

$ZArchiveCMake = Join-Path $Root "third_party\ZArchive\CMakeLists.txt"
if (-not (Test-Path -LiteralPath $ZArchiveCMake -PathType Leaf)) {
    throw "third_party\ZArchive is missing. Clone/update the repository with submodules before building the Player ZIP."
}

Write-Host "==> Packaging generic source needed for local preparation"
$Robo = Join-Path $env:SystemRoot "System32\robocopy.exe"
$excludeDirs = @(
    ".git", "build", "generated", "dist", ".tools", ".vcpkg-cache",
    "user", "out", ".vs", ".idea"
)
$roboArgs = @(
    $Root, $SourceDir, "/E", "/NFL", "/NDL", "/NJH", "/NJS", "/NP", "/R:1", "/W:1",
    "/XD"
) + $excludeDirs
& $Robo @roboArgs | Out-Null
$RoboCode = $LASTEXITCODE
if ($RoboCode -gt 7) { throw "Source packaging failed with robocopy exit code $RoboCode" }

# Never ship any locally generated/compiled game-derived runtime.
$forbidden = @(
    (Join-Path $PackageRoot "bin\ttt2-runtime.exe"),
    (Join-Path $SourceDir "generated\ttt2-full"),
    (Join-Path $SourceDir "generated\ttt2")
)
foreach ($path in $forbidden) {
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to create generic Player ZIP because a game-derived runtime/output was found: $path"
    }
}

@"
TTT2 Recompiled - Windows Player

1. Extract this ZIP to a normal folder.
2. Open "Tekken Tag Tournament 2.exe".
3. Choose your own extracted EU v16 game folder (the folder containing code\Tekken.rpx).
4. Press PLAY.

On the first launch the launcher prepares a local native runtime from your own Tekken.rpx.
A progress bar shows the current preparation stage. The generated runtime stays only on your PC
under bin\ttt2-runtime.exe. Later launches start directly.

If compiler components are missing, the preparer attempts to install the required Windows build
tools automatically through winget. This one-time setup can be large and can take several minutes.

No Tekken game files or precompiled game-derived AOT runtime are included in this ZIP.
"@ | Set-Content -LiteralPath (Join-Path $PackageRoot "README-PLAYER.txt") -Encoding UTF8

Set-Content -LiteralPath (Join-Path $BinDir "README.txt") -Encoding UTF8 -Value "The first-run preparer creates your local ttt2-runtime.exe here."
Set-Content -LiteralPath (Join-Path $UserDir "README.txt") -Encoding UTF8 -Value "Settings, saves, preparation log and persistent caches are stored here."
Set-Content -LiteralPath (Join-Path $OutDir "README.txt") -Encoding UTF8 -Value "Reusable runtime caches are stored here."

$Git = Get-Command git.exe -ErrorAction SilentlyContinue
$Commit = if ($Git) { (& git rev-parse HEAD).Trim() } else { "unknown" }
@"
TTT2 Recompiled Windows Player
Version: $Version
Git commit: $Commit
Package type: generic first-run local AOT preparer
Supported game: TEKKEN TAG TOURNAMENT 2 Wii U EDITION EU v16
Expected Tekken.rpx SHA-256: fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019
Game-derived AOT runtime included: NO
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

The first start performs a one-time local PowerPC -> x86-64 preparation from the player's own
Tekken.rpx. The launcher displays the current stage and a progress bar. Future starts use the
prepared local runtime directly.

No game files and no precompiled game-derived AOT runtime are included in the download.
"@ | Set-Content -LiteralPath $Notes -Encoding UTF8

Write-Host ""
Write-Host "GENERIC PLAYER PACKAGE READY"
Write-Host "  ZIP:    $Zip"
Write-Host "  Hashes: $Checksums"

if ($Publish) {
    $Gh = Need-Command "gh.exe"
    & $Gh auth status
    if ($LASTEXITCODE -ne 0) { throw "GitHub CLI is not authenticated." }

    $Repo = "xruanzin/Tekken-Tag-2-Recompiled"

    # A missing release is the expected first-publish case. Windows PowerShell
    # 5.1 can turn native stderr into a terminating RemoteException when
    # ErrorActionPreference is Stop, so probe with Continue and inspect the
    # native exit code explicitly.
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
    Write-Host "Published generic Player ZIP: https://github.com/$Repo/releases/tag/$Version"
}
