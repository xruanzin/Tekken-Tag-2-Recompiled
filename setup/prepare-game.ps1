[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Rpx,
    [Parameter(Mandatory = $true)]
    [string]$OutputExe
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Logs = Join-Path $Root "logs"
$StateFile = Join-Path $Logs "setup-state.txt"
$LogFile = Join-Path $Logs "setup-last-run.txt"
New-Item -ItemType Directory -Force $Logs | Out-Null

function Set-State([string]$Text) {
    Set-Content -LiteralPath $StateFile -Value $Text -Encoding UTF8
    Write-Output "==> $Text"
}

function Refresh-Path {
    $known = @(
        "C:\Program Files\Git\cmd",
        "C:\Program Files\CMake\bin",
        "C:\Program Files\LLVM\bin",
        (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links")
    )
    $pythonRoot = Join-Path $env:LOCALAPPDATA "Programs\Python"
    if (Test-Path -LiteralPath $pythonRoot) {
        $pythonDirs = Get-ChildItem -Path $pythonRoot -Directory -ErrorAction SilentlyContinue | Sort-Object FullName -Descending
        foreach ($p in $pythonDirs) {
            $known += $p.FullName
            $known += (Join-Path $p.FullName "Scripts")
        }
    }
    $env:Path = (($known + ($env:Path -split ';')) | Where-Object {
        $_ -and (Test-Path -LiteralPath $_)
    } | Select-Object -Unique) -join ';'
}

function Has-Command([string]$Name) {
    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Install-WingetPackage([string]$Id, [string]$Label, [string]$Override = "") {
    Set-State "Instalando requisito: $Label..."
    $args = @("install", "--id", $Id, "-e", "--accept-source-agreements", "--accept-package-agreements", "--silent", "--disable-interactivity")
    if ($Override) { $args += @("--override", $Override) }
    & winget.exe @args
    if ($LASTEXITCODE -ne 0) { throw "Falha ao instalar $Label (winget exit $LASTEXITCODE)." }
    Refresh-Path
}

function Ensure-Prerequisites {
    Set-State "Verificando ferramentas da primeira execução..."
    if (-not (Has-Command "winget.exe")) { throw "Windows Package Manager (winget) não está disponível." }
    Refresh-Path
    if (-not (Has-Command "git.exe")) { Install-WingetPackage "Git.Git" "Git" }
    if (-not (Has-Command "cmake.exe")) { Install-WingetPackage "Kitware.CMake" "CMake" }
    if (-not (Has-Command "ninja.exe")) { Install-WingetPackage "Ninja-build.Ninja" "Ninja" }
    if (-not (Has-Command "clang-cl.exe") -or -not (Has-Command "llvm-rc.exe")) { Install-WingetPackage "LLVM.LLVM" "LLVM/Clang" }
    if (-not (Has-Command "py.exe") -and -not (Has-Command "python.exe")) { Install-WingetPackage "Python.Python.3.12" "Python" }

    $vswhere = Join-Path ([Environment]::GetFolderPath("ProgramFilesX86")) "Microsoft Visual Studio\Installer\vswhere.exe"
    $vcReady = $false
    if (Test-Path -LiteralPath $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $vcReady = -not [string]::IsNullOrWhiteSpace($install)
    }
    if (-not $vcReady) {
        Install-WingetPackage "Microsoft.VisualStudio.2022.BuildTools" "Visual Studio C++ Build Tools" "--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    }
    Refresh-Path
}

try {
    Start-Transcript -LiteralPath $LogFile -Force | Out-Null
    $resolvedRpx = (Resolve-Path -LiteralPath $Rpx).Path
    Set-State "Validando TEKKEN TAG TOURNAMENT 2 EU v16..."
    $expected = "fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019"
    $actual = (Get-FileHash -LiteralPath $resolvedRpx -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) { throw "Tekken.rpx não corresponde à versão europeia v16 suportada." }

    Ensure-Prerequisites
    Set-State "Preparando código nativo. A primeira execução pode demorar alguns minutos..."
    $build = Join-Path $Root "build-windows.ps1"
    if (-not (Test-Path -LiteralPath $build)) { throw "build-windows.ps1 não foi encontrado no pacote." }

    $buildArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $build, "-Rpx", $resolvedRpx, "-GameOnly", "-OutputExe", $OutputExe)
    & powershell.exe @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "A compilação terminou com código $LASTEXITCODE." }
    if (-not (Test-Path -LiteralPath $OutputExe)) { throw "A preparação terminou sem produzir ttt2.exe." }

    Set-State "Pronto para jogar."
    Stop-Transcript | Out-Null
    exit 0
}
catch {
    Set-State ("Falha na preparação: " + $_.Exception.Message)
    Write-Error $_
    try { Stop-Transcript | Out-Null } catch {}
    exit 1
}
