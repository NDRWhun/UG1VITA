# One-step build of the VPK: finds the game, regenerates the recompiled code when needed, configures and builds.
# Usage: tools/build.ps1 [-Game <the game folder>] [-Config Release|Debug] [-Clean] [-Reconfigure] [-Jobs N]
#                        [-Regenerate] [-Compiler gcc|clang] [-NoLto] [-PgoGenerate] [-PgoUse <file.profdata>]
# The game folder (with Speed.exe) comes from -Game, then NFSU_GAME_DIR, then the folder used last time; it is
# remembered for the next run. Code is generated again when the executable, the audio banks (Sound/**/*.abk) or the
# recompiler changed, or with -Regenerate. Only what changed is rebuilt.
# -Compiler clang builds with clang and lld (NFSU_LLVM: the LLVM root) and ThinLTO (-NoLto: without) into
# build-<config>-clang; -PgoGenerate makes the instrumented build (build-<config>-pgogen), -PgoUse optimises with a
# merged profile (README).
param(
    [string]$Game,
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Reconfigure,
    [switch]$Regenerate,
    [int]$Jobs = [Environment]::ProcessorCount,
    [ValidateSet('gcc', 'clang')][string]$Compiler = 'gcc',
    [switch]$NoLto,
    [switch]$PgoGenerate,
    [string]$PgoUse
)
if (($NoLto -or $PgoGenerate -or $PgoUse) -and $Compiler -ne 'clang') { throw '-NoLto, -PgoGenerate and -PgoUse need -Compiler clang' }
if ($PgoGenerate -and $PgoUse) { throw '-PgoGenerate and -PgoUse exclude each other' }
if ($PgoUse) { $PgoUse = (Resolve-Path $PgoUse).Path }

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$suffix = if ($PgoGenerate) { '-pgogen' } elseif ($Compiler -eq 'clang') { '-clang' } else { '' }
$build = Join-Path $repo "build-$($Config.ToLower())$suffix"
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

# Fingerprint of the generator inputs: the executable, the audio banks and every recompiler source.
function Get-GeneratorStamp([string]$gameDir) {
    $sound = Join-Path $gameDir 'Sound'
    $banks = if (Test-Path $sound) { @(Get-ChildItem $sound -Recurse -File -Filter '*.abk' | Sort-Object FullName) } else { @() }
    $files = @(Get-Item (Join-Path $gameDir $exeName)) + $banks +
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

$clangChanged = $false
if ($Compiler -eq 'clang') {
    # Reconfigures when the clang options differ from clang-options.txt in the build folder.
    $clangOptions = @('-DNFSU_COMPILER=clang', "-DNFSU_LTO=$(if ($NoLto) { 'OFF' } else { 'ON' })",
                      "-DNFSU_PGO_GENERATE=$(if ($PgoGenerate) { 'ON' } else { 'OFF' })", "-DNFSU_PGO_USE=$PgoUse")
    $optionsFile = Join-Path $build 'clang-options.txt'
    $previous = if (Test-Path $optionsFile) { (Get-Content $optionsFile -Raw).Trim() } else { '' }
    $clangChanged = $previous -ne ($clangOptions -join ' ')
    if (-not $env:NFSU_LLVM) { Write-Host 'NFSU_LLVM not set: clang from PATH' }
}
if ($Reconfigure -or -not (Test-Path (Join-Path $build 'build.ninja')) -or $clangChanged) {
    $toolchain = if ($Compiler -eq 'clang') { "$repo/cmake/vita-clang.cmake" } else { "$env:VITASDK/share/vita.toolchain.cmake" }
    $options = @("-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DCMAKE_BUILD_TYPE=$Config")
    if ($Compiler -eq 'clang') { $options += $clangOptions }
    $options += if ($env:SCE_PSP2_SDK_DIR) { "-DSCE_PSP2_SDK_DIR=$env:SCE_PSP2_SDK_DIR" } else { '-USCE_PSP2_SDK_DIR' }
    cmake -S $repo -B $build -G Ninja @options
    if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
    if ($Compiler -eq 'clang') { Set-Content -Path $optionsFile -Value ($clangOptions -join ' ') -Encoding UTF8 }
}

cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Write-Host "Output: $(Join-Path $build 'NFSUG1VITA.vpk')"
