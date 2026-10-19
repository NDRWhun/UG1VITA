# One-step build of the VPK: finds the game, regenerates the recompiled code when needed, configures and builds.
# Usage: tools/build.ps1 [-Game <the game folder>] [-Config Release|Debug] [-Clean] [-Reconfigure] [-Jobs N]
#                        [-Regenerate]
# The game folder (with Speed.exe) comes from -Game, then NFSU_GAME_DIR, then the folder used last time; it is
# remembered for the next run. Code is generated again when the executable or the recompiler changed, or with
# -Regenerate. Only what changed is rebuilt.
param(
    [string]$Game,
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Reconfigure,
    [switch]$Regenerate,
    [int]$Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $repo "build-$($Config.ToLower())"
$generated = Join-Path $repo 'src/nfsu/generated'
$exeName = 'Speed.exe'
$remembered = Join-Path $repo 'build-game-dir.txt'

function Resolve-GameDir {
    $candidates = @($Game, $env:NFSU_GAME_DIR)
    if (Test-Path $remembered) { $candidates += (Get-Content $remembered -Raw).Trim() }
    foreach ($dir in $candidates) {
        if (-not $dir) { continue }
        if (Test-Path (Join-Path $dir $exeName)) { return (Resolve-Path $dir).Path }
        if ($dir -eq $Game) { throw "$exeName not found in $Game (give the game folder)" }
    }
    return $null
}

# Fingerprint of the generator inputs: the executable and every recompiler source.
function Get-GeneratorStamp([string]$gameDir) {
    $files = @(Get-Item (Join-Path $gameDir $exeName)) +
             @(Get-ChildItem (Join-Path $PSScriptRoot 'recompiler') -Recurse -File -Include '*.py', '*.txt' |
               Where-Object { $_.FullName -notmatch '__pycache__' } | Sort-Object FullName)
    $lines = $files | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '')
}

$stampFile = Join-Path $generated '.stamp'
$haveCode = Test-Path (Join-Path $generated 'nfsu.cpp')
$gameDir = Resolve-GameDir
if ($gameDir) {
    Set-Content -Path $remembered -Value $gameDir -Encoding UTF8
    $stamp = Get-GeneratorStamp $gameDir
    $current = if (Test-Path $stampFile) { (Get-Content $stampFile -Raw).Trim() } else { '' }
    if ($Regenerate -or -not $haveCode -or $stamp -ne $current) {
        Write-Host "Generating the recompiled code from $(Join-Path $gameDir $exeName)..."
        & (Join-Path $PSScriptRoot 'generate.ps1') -Binaries $gameDir
        Set-Content -Path $stampFile -Value $stamp -Encoding ASCII
    }
} elseif (-not $haveCode) {
    throw "The game folder was not found: run tools/build.ps1 -Game '<the game folder with $exeName>'."
} else {
    Write-Host 'Game folder not found: building the existing generated code.'
}

# Native tools report failure through their exit code; Windows PowerShell 5.1 would stop on any stderr line.
$ErrorActionPreference = 'Continue'
if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force -ErrorAction Stop }

if ($Reconfigure -or -not (Test-Path (Join-Path $build 'build.ninja'))) {
    $options = @("-DCMAKE_TOOLCHAIN_FILE=$env:VITASDK/share/vita.toolchain.cmake", "-DCMAKE_BUILD_TYPE=$Config")
    $options += if ($env:SCE_PSP2_SDK_DIR) { "-DSCE_PSP2_SDK_DIR=$env:SCE_PSP2_SDK_DIR" } else { '-USCE_PSP2_SDK_DIR' }
    cmake -S $repo -B $build -G Ninja @options
    if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
}

cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Write-Host "Output: $(Join-Path $build 'NFSUG1VITA.vpk')"
