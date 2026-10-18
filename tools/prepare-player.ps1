[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Game
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# Windows PowerShell 5.1 otherwise emits redirected text as UTF-16LE on some
# systems. The native launcher consumes this stream for progress updates, so
# force UTF-8 and keep the preparation log in one encoding.
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $Utf8NoBom
$OutputEncoding = $Utf8NoBom

function Write-Stage {
    param([int]$Percent, [string]$Message)
    $line = "TTT2_PROGRESS|$Percent|$Message"
    Write-Output $line
    if ($script:LogFile) {
        Add-Content -LiteralPath $script:LogFile -Encoding UTF8 -Value $line -ErrorAction SilentlyContinue
    }
}

function Resolve-Exe {
    param([string]$Name, [string[]]$Candidates = @())
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function Get-VsBuildTools {
    $pf86 = [Environment]::GetFolderPath("ProgramFilesX86")
    if (-not $pf86) { return $null }
    $vswhere = Join-Path $pf86 "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($LASTEXITCODE -eq 0 -and $install) { return $install }
    return $null
}

function Install-WingetPackage {
    param(
        [Parameter(Mandatory = $true)][string[]]$Ids,
        [string]$Override = ""
    )
    $winget = Resolve-Exe "winget.exe" @(
        (Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\winget.exe")
    )
    if (-not $winget) {
        throw "Este PC nao possui winget. O pacote Player precisa de uma toolchain portatil ou do App Installer do Windows."
    }

    foreach ($id in $Ids) {
        Write-Output "prepare: tentando instalar $id"
        $args = @("install", "--id", $id, "-e", "--source", "winget",
                  "--accept-package-agreements", "--accept-source-agreements",
                  "--silent", "--disable-interactivity")
        if ($Override) { $args += @("--override", $Override) }
        & $winget @args
        if ($LASTEXITCODE -eq 0) { return }
    }
    throw "Nao foi possivel instalar automaticamente: $($Ids -join ', ')"
}

$Root = [System.IO.Path]::GetFullPath($Root)
$Game = [System.IO.Path]::GetFullPath($Game)
$UserDir = Join-Path $Root "user"
$BinDir = Join-Path $Root "bin"
New-Item -ItemType Directory -Force $UserDir, $BinDir | Out-Null
$script:LogFile = Join-Path $UserDir "prepare.log"
Set-Content -LiteralPath $script:LogFile -Encoding UTF8 -Value "TTT2 first-run preparation"

Write-Stage 2 "Validando a pasta do jogo"

if (-not (Test-Path -LiteralPath $Game -PathType Container)) {
    throw "A preparacao inicial requer a pasta extraida do jogo."
}
$Rpx = Join-Path $Game "code\Tekken.rpx"
if (-not (Test-Path -LiteralPath $Rpx -PathType Leaf)) {
    throw "Nao foi encontrado code\Tekken.rpx na pasta selecionada."
}

$Expected = "fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019"
$Actual = (Get-FileHash -LiteralPath $Rpx -Algorithm SHA256).Hash.ToLowerInvariant()
if ($Actual -ne $Expected) {
    throw "Tekken.rpx nao suportado. Este Player requer a versao europeia v16."
}

Write-Stage 8 "Jogo EU v16 verificado"

$Source = Join-Path $Root "tools\source"
if (-not (Test-Path -LiteralPath (Join-Path $Source "build-windows.ps1") -PathType Leaf)) {
    if (Test-Path -LiteralPath (Join-Path $Root "build-windows.ps1") -PathType Leaf) {
        $Source = $Root
    } else {
        throw "Os arquivos de preparacao do Player nao foram encontrados."
    }
}

Write-Stage 10 "Verificando ferramentas de compilacao"

$missing = @()
if (-not (Resolve-Exe "git.exe" @("C:\Program Files\Git\cmd\git.exe"))) { $missing += "git" }
if (-not (Resolve-Exe "cmake.exe" @(
    "C:\Program Files\CMake\bin\cmake.exe",
    (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links\cmake.exe")
))) { $missing += "cmake" }
if (-not (Resolve-Exe "ninja.exe" @((Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links\ninja.exe")))) { $missing += "ninja" }
if (-not (Resolve-Exe "clang-cl.exe" @("C:\Program Files\LLVM\bin\clang-cl.exe"))) { $missing += "llvm" }
if (-not (Resolve-Exe "python.exe") -and -not (Resolve-Exe "py.exe")) { $missing += "python" }

if ($missing.Count -gt 0) {
    Write-Stage 12 "Baixando ferramentas necessarias (somente na primeira vez)"
    if ($missing -contains "git") { Install-WingetPackage @("Git.Git") }
    if ($missing -contains "cmake") { Install-WingetPackage @("Kitware.CMake") }
    if ($missing -contains "ninja") { Install-WingetPackage @("Ninja-build.Ninja") }
    if ($missing -contains "llvm") { Install-WingetPackage @("LLVM.LLVM") }
    if ($missing -contains "python") { Install-WingetPackage @("Python.Python.3.12", "Python.Python.3.11") }
}

if (-not (Get-VsBuildTools)) {
    Write-Stage 16 "Instalando componentes C++ do Windows (somente na primeira vez)"
    $override = "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    Install-WingetPackage @("Microsoft.VisualStudio.2026.BuildTools", "Microsoft.VisualStudio.2022.BuildTools") $override
}

Write-Stage 20 "Toolchain pronta"

$PowerShell = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
$BuildScript = Join-Path $Source "build-windows.ps1"
$Runtime = Join-Path $BinDir "ttt2-runtime.exe"

$oldProgress = $env:TTT2_PLAYER_PROGRESS
try {
    $env:TTT2_PLAYER_PROGRESS = "1"
    $args = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", $BuildScript,
        "-Rpx", $Rpx,
        "-BuildDir", "player-build/windows",
        "-GeneratedDir", "player-generated/ttt2-full",
        "-FullAot",
        "-GameOnly",
        "-OutputExe", $Runtime
    )

    & $PowerShell @args 2>&1 | ForEach-Object {
        $line = [string]$_
        Add-Content -LiteralPath $script:LogFile -Encoding UTF8 -Value $line
        Write-Output $line
    }
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw "A preparacao local falhou com codigo $code."
    }
} finally {
    $env:TTT2_PLAYER_PROGRESS = $oldProgress
}

if (-not (Test-Path -LiteralPath $Runtime -PathType Leaf)) {
    throw "A preparacao terminou sem gerar bin\ttt2-runtime.exe."
}

Write-Stage 98 "Verificando o runtime preparado"
$env:TTT2_USER_DIR = $UserDir
& $Runtime --verify $Game 2>&1 | ForEach-Object {
    $line = [string]$_
    Add-Content -LiteralPath $script:LogFile -Encoding UTF8 -Value $line
    Write-Output $line
}
if ($LASTEXITCODE -ne 0) {
    throw "O runtime local foi gerado, mas a verificacao final falhou."
}

$marker = Join-Path $UserDir "prepared-runtime.txt"
@(
    "TTT2 local player runtime",
    "RPX-SHA256=$Expected",
    "Prepared=$(Get-Date -Format o)"
) | Set-Content -LiteralPath $marker -Encoding UTF8

Write-Stage 100 "Jogo preparado. Iniciando..."
exit 0
