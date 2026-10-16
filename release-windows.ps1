[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Game,
    [string]$Version = "",
    [switch]$Publish,
    [switch]$SkipBuild,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

if ($env:OS -ne "Windows_NT") { throw "This release builder must be run on Windows." }

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

function Resolve-RpxPath {
    param([Parameter(Mandatory = $true)][string]$InputPath)
    $resolved = (Resolve-Path -LiteralPath $InputPath).Path
    if (Test-Path -LiteralPath $resolved -PathType Leaf) {
        if ([System.IO.Path]::GetFileName($resolved) -ine "Tekken.rpx") { throw "Expected Tekken.rpx or the extracted game folder." }
        return $resolved
    }
    foreach ($candidate in @((Join-Path $resolved "code\Tekken.rpx"), (Join-Path $resolved "Tekken.rpx"))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "Could not find code\Tekken.rpx under $resolved"
}

function Resolve-GameRoot {
    param([Parameter(Mandatory = $true)][string]$RpxPath)
    $code = Split-Path -Parent $RpxPath
    if ((Split-Path -Leaf $code) -ieq "code") { return Split-Path -Parent $code }
    return $code
}

function Need-Command {
    param([Parameter(Mandatory = $true)][string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }

    # GitHub CLI may already be installed by winget/MSI even when this
    # PowerShell session has not refreshed PATH yet.
    if ($Name -ieq "gh.exe") {
        $programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
        $candidates = @(
            (Join-Path $env:ProgramFiles "GitHub CLI\gh.exe"),
            (if ($programFilesX86) { Join-Path $programFilesX86 "GitHub CLI\gh.exe" }),
            (Join-Path $env:LOCALAPPDATA "Programs\GitHub CLI\gh.exe")
        )
        foreach ($candidate in $candidates) {
            if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }

        throw "GitHub CLI was not found. Install it once with: winget install --id GitHub.cli -e --source winget"
    }

    throw "Missing required command: $Name"
}

$RpxPath = Resolve-RpxPath $Game
$GameRoot = Resolve-GameRoot $RpxPath
$ExpectedRpxSha256 = "fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019"
$ActualRpxSha256 = (Get-FileHash -LiteralPath $RpxPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ActualRpxSha256 -ne $ExpectedRpxSha256) {
    throw "Unsupported Tekken.rpx. Expected EU v16 SHA-256 $ExpectedRpxSha256; found $ActualRpxSha256"
}

if (-not $Version) {
    $VersionFile = Join-Path $Root ".github\release-version.txt"
    if (Test-Path -LiteralPath $VersionFile) { $Version = (Get-Content -LiteralPath $VersionFile -Raw).Trim() }
}
if (-not $Version) { throw "Pass -Version vX.Y.Z or set .github\release-version.txt." }
if ($Version -notmatch '^v[0-9]+\.[0-9]+\.[0-9]+[-A-Za-z0-9.]*$') { throw "Invalid release version: $Version" }

$BuildDir = Join-Path $Root "build\windows"
$RuntimeExe = Join-Path $BuildDir "ttt2.exe"
$LauncherExe = Join-Path $BuildDir "Tekken Tag Tournament 2.exe"
$QuickPlayExe = Join-Path $BuildDir "Play Tekken Tag Tournament 2.exe"

if (-not $SkipBuild) {
    Write-Host "==> Building the static AOT player runtime"
    $buildArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $Root "build-windows.ps1"), "-Rpx", $RpxPath, "-FullAot")
    if ($Clean) { $buildArgs += "-Clean" }
    & powershell.exe @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "build-windows.ps1 failed with exit code $LASTEXITCODE" }
} else {
    # Skip the expensive game-code rebuild, but always refresh the lightweight
    # launcher binaries so UI/validation fixes cannot be hidden by stale EXEs.
    $CMakeExe = Need-Command "cmake.exe"
    Write-Host "==> Refreshing Windows launchers (static runtime build skipped)"
    & $CMakeExe --build $BuildDir --target ttt2_launcher ttt2_play
    if ($LASTEXITCODE -ne 0) { throw "Launcher refresh failed with exit code $LASTEXITCODE" }
}

foreach ($required in @($RuntimeExe, $LauncherExe, $QuickPlayExe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required release binary was not produced: $required" }
}

$DistRoot = Join-Path $Root "dist"
$PackageRoot = Join-Path $DistRoot "ttt2-windows"
$BinDir = Join-Path $PackageRoot "bin"
$UserDir = Join-Path $PackageRoot "user"
$OutDir = Join-Path $PackageRoot "out"
if (Test-Path -LiteralPath $PackageRoot) { Remove-Item -Recurse -Force -LiteralPath $PackageRoot }
New-Item -ItemType Directory -Force $BinDir, $UserDir, $OutDir | Out-Null

Copy-Item -LiteralPath $LauncherExe -Destination (Join-Path $PackageRoot "Tekken Tag Tournament 2.exe") -Force
Copy-Item -LiteralPath $QuickPlayExe -Destination (Join-Path $PackageRoot "Play Tekken Tag Tournament 2.exe") -Force
Copy-Item -LiteralPath $RuntimeExe -Destination (Join-Path $BinDir "ttt2-runtime.exe") -Force
Copy-Item -LiteralPath (Join-Path $Root "README-Windows.txt") -Destination (Join-Path $PackageRoot "README-Windows.txt") -Force
Copy-Item -LiteralPath (Join-Path $Root "README.md") -Destination (Join-Path $PackageRoot "README.md") -Force
Copy-Item -LiteralPath (Join-Path $Root "LICENSE") -Destination (Join-Path $PackageRoot "LICENSE") -Force
Set-Content -LiteralPath (Join-Path $UserDir "README.txt") -Encoding UTF8 -Value "Player saves, settings and last_run.log are stored here."
Set-Content -LiteralPath (Join-Path $OutDir "README.txt") -Encoding UTF8 -Value "Reusable generated caches are stored here."

# Prove this is an ahead-of-time player build from the generated metadata
# itself. Do not depend on runtime log creation: a WINDOWS-subsystem executable
# may legitimately complete --verify without creating a console-visible log.
$GeneratedImports = Join-Path $Root "generated\ttt2\imports.cpp"
if (-not (Test-Path -LiteralPath $GeneratedImports -PathType Leaf)) {
    throw "Refusing to package: generated\ttt2\imports.cpp was not found. Run the static build first."
}
$GeneratedText = Get-Content -LiteralPath $GeneratedImports -Raw
$ProgramInfoPattern = 'cafe_program_info\s*=\s*\{[\s\S]*?"([0-9a-fA-F]{64})"[\s\S]*?,\s*([0-9]+),\s*([0-9]+)\s*\};'
$ProgramInfoMatch = [regex]::Match($GeneratedText, $ProgramInfoPattern)
if (-not $ProgramInfoMatch.Success) {
    throw "Refusing to package: could not read static ProgramInfo from generated\ttt2\imports.cpp."
}
$GeneratedDigest = $ProgramInfoMatch.Groups[1].Value.ToLowerInvariant()
$CompiledEntries = [int64]$ProgramInfoMatch.Groups[2].Value
$TotalEntries = [int64]$ProgramInfoMatch.Groups[3].Value
if ($GeneratedDigest -ne $ExpectedRpxSha256 -or $CompiledEntries -le 0 -or $TotalEntries -le 0) {
    throw "Refusing to package: generated code is not the supported static AOT EU v16 build."
}
if ($CompiledEntries -ne $TotalEntries) {
    throw "Refusing to package an incomplete AOT runtime: $CompiledEntries/$TotalEntries guest entries compiled. Rebuild without -SkipBuild so release-windows.ps1 can generate a full AOT runtime."
}
Write-Host "==> Full static AOT metadata verified: $CompiledEntries/$TotalEntries guest entries compiled"

# The runtime's own --verify still validates the player's game source. Its exit
# code is authoritative; log creation is not required for this packaging check.
$verifyUser = Join-Path $DistRoot "verify-user"
if (Test-Path -LiteralPath $verifyUser) { Remove-Item -Recurse -Force -LiteralPath $verifyUser }
New-Item -ItemType Directory -Force $verifyUser | Out-Null
$oldUser = $env:TTT2_USER_DIR
try {
    $env:TTT2_USER_DIR = $verifyUser
    Write-Host "==> Verifying packaged runtime against EU v16 game source"
    & (Join-Path $BinDir "ttt2-runtime.exe") --verify $GameRoot
    if ($LASTEXITCODE -ne 0) { throw "Packaged runtime verification failed with exit code $LASTEXITCODE" }
} finally { $env:TTT2_USER_DIR = $oldUser }
Write-Host "==> Runtime source verification passed"

$GitExe = Get-Command git.exe -ErrorAction SilentlyContinue
$Commit = if ($GitExe) { (& git rev-parse HEAD).Trim() } else { "unknown" }
$RuntimeHash = (Get-FileHash -LiteralPath (Join-Path $BinDir "ttt2-runtime.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
$LauncherHash = (Get-FileHash -LiteralPath (Join-Path $PackageRoot "Tekken Tag Tournament 2.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath (Join-Path $PackageRoot "BUILD-INFO.txt") -Encoding UTF8 -Value "TTT2 Recompiled Windows`nVersion: $Version`nGit commit: $Commit`n`nPlayer runtime: static ahead-of-time PowerPC -> x86-64 recompilation`nSupported game: TEKKEN TAG TOURNAMENT 2 Wii U EDITION EU v16`nExpected Tekken.rpx SHA-256: $ExpectedRpxSha256`n`nttt2-runtime.exe SHA-256:`n$RuntimeHash`n`nTekken Tag Tournament 2.exe SHA-256:`n$LauncherHash"

New-Item -ItemType Directory -Force $DistRoot | Out-Null
$Zip = Join-Path $DistRoot "TTT2-Windows-$Version.zip"
if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
Compress-Archive -Path $PackageRoot -DestinationPath $Zip -CompressionLevel Optimal

$ReleaseLauncher = Join-Path $DistRoot "Tekken Tag Tournament 2.exe"
$ReleaseQuickPlay = Join-Path $DistRoot "Play Tekken Tag Tournament 2.exe"
$ReleaseRuntime = Join-Path $DistRoot "ttt2-runtime.exe"
Copy-Item -LiteralPath $LauncherExe -Destination $ReleaseLauncher -Force
Copy-Item -LiteralPath $QuickPlayExe -Destination $ReleaseQuickPlay -Force
Copy-Item -LiteralPath $RuntimeExe -Destination $ReleaseRuntime -Force

$Checksums = Join-Path $DistRoot "SHA256SUMS.txt"
@(
    "$((Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($Zip))",
    "$((Get-FileHash -LiteralPath $ReleaseLauncher -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($ReleaseLauncher))",
    "$((Get-FileHash -LiteralPath $ReleaseQuickPlay -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($ReleaseQuickPlay))",
    "$((Get-FileHash -LiteralPath $ReleaseRuntime -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($ReleaseRuntime))"
) | Set-Content -LiteralPath $Checksums -Encoding ASCII

$Notes = Join-Path $DistRoot "release-notes.md"
$NotesText = @"
## TTT2 Recompiled Windows $Version

This release uses the **static ahead-of-time recompiled Windows runtime**.

1. Extract the ZIP.
2. Open **Tekken Tag Tournament 2.exe**.
3. Select your own supported EU v16 game folder or .wua.
4. Press **PLAY / JOGAR**.

The player's PC does not run cafe-recomp, CMake, Ninja, Clang, Python or Visual Studio Build Tools.

Package: launcher, quick-play executable, prebuilt bin/ttt2-runtime.exe, user/ and out/ directories.
The package does not contain Tekken.rpx or game assets; the runtime validates the user's EU v16 source.

Original Linux port/static recompilation architecture: **Petexy and upstream contributors**.
Windows port, launcher and Windows-specific integration: **xruanzin**.
AI-assisted code review/debugging/documentation: **OpenAI ChatGPT**.
"@
Set-Content -LiteralPath $Notes -Encoding UTF8 -Value $NotesText

Write-Host ""
Write-Host "PLAYER PACKAGE READY"
Write-Host "  ZIP:      $Zip"
Write-Host "  Launcher: $ReleaseLauncher"
Write-Host "  Runtime:  $ReleaseRuntime"
Write-Host "  Hashes:   $Checksums"

if ($Publish) {
    $GhExe = Need-Command "gh.exe"
    & $GhExe auth status
    if ($LASTEXITCODE -ne 0) { throw "GitHub CLI is not authenticated." }
    $Repo = "xruanzin/Tekken-Tag-2-Recompiled"
    Write-Host "==> Publishing GitHub release $Version"

    # A missing release is an expected first-publish condition, not a script
    # failure. Suppress gh's "release not found" stderr so PowerShell 5.1 with
    # ErrorActionPreference=Stop can inspect LASTEXITCODE normally.
    & $GhExe release view $Version --repo $Repo --json tagName 2>$null | Out-Null
    $ReleaseExists = ($LASTEXITCODE -eq 0)

    if ($ReleaseExists) {
        Write-Host "==> Existing release found; replacing assets"
        & $GhExe release upload $Version $Zip $ReleaseLauncher $ReleaseQuickPlay $ReleaseRuntime $Checksums --repo $Repo --clobber
        if ($LASTEXITCODE -ne 0) { throw "gh release upload failed" }
        & $GhExe release edit $Version --repo $Repo --title "TTT2 Recompiled Windows $Version" --notes-file $Notes
        if ($LASTEXITCODE -ne 0) { throw "gh release edit failed" }
    } else {
        Write-Host "==> Creating new GitHub release"
        & $GhExe release create $Version $Zip $ReleaseLauncher $ReleaseQuickPlay $ReleaseRuntime $Checksums --repo $Repo --target windows-host --title "TTT2 Recompiled Windows $Version" --notes-file $Notes
        if ($LASTEXITCODE -ne 0) { throw "gh release create failed" }
    }
    Write-Host "Published: https://github.com/$Repo/releases/tag/$Version"
}
