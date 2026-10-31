# One-step build of the VPK: finds the game, regenerates the recompiled code when needed, configures and builds.
# Usage: tools/build.ps1 [-Game <the game folder>] [-Config Release|Debug] [-Clean] [-Reconfigure] [-Jobs N]
#                        [-Regenerate] [-Compiler clang|gcc] [-NoLto] [-PgoGenerate [-PgoList <file>]]
#                        [-PgoUse <profile>[,<profile>...]] [-NoPgo] [-FlagAudit]
# The game folder (with Speed.exe) comes from -Game, then NFSU_GAME_DIR, then the folder used last time; it is
# remembered for the next run. Code is generated again when the executable, the audio banks (Sound/**/*.abk) or the
# recompiler changed, or with -Regenerate. Only what changed is rebuilt.
# The default compiler is clang with lld (NFSU_LLVM: the LLVM root) and ThinLTO (-NoLto: without), built into
# build-<config>-clang. -PgoUse optimises with a merged .profdata (build-<config>-clangpgo, README); without -PgoUse
# the profile named by NFSU_PGO_PROFILE is used when that file exists (-NoPgo: none). -PgoGenerate makes the
# instrumented build (build-<config>-pgogen; -PgoList: only the functions and sources a clang -fprofile-list file
# selects). -Compiler gcc builds with VitaSDK's gcc into build-<config>; there -PgoGenerate builds into
# build-<config>-gcov and -PgoUse takes one or more nfsu.gcov files from instrumented runs (build-<config>-pgo).
param(
    [string]$Game,
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Reconfigure,
    [switch]$Regenerate,
    [switch]$FlagAudit,
    [int]$Jobs = [Environment]::ProcessorCount,
    [ValidateSet('gcc', 'clang')][string]$Compiler = 'clang',
    [switch]$NoLto,
    [switch]$PgoGenerate,
    [string]$PgoList,
    [string[]]$PgoUse,
    [switch]$NoPgo
)
if ($NoPgo -and $PgoUse) { throw '-NoPgo and -PgoUse exclude each other' }
if ($NoLto -and $Compiler -ne 'clang') { throw '-NoLto needs -Compiler clang' }
if ($PgoList -and -not ($PgoGenerate -and $Compiler -eq 'clang')) { throw '-PgoList needs -Compiler clang -PgoGenerate' }
if ($PgoList) { $PgoList = (Resolve-Path $PgoList).Path -replace '\\', '/' }
if ($PgoGenerate -and $PgoUse) { throw '-PgoGenerate and -PgoUse exclude each other' }
if ($PgoUse -and $Compiler -eq 'clang' -and $PgoUse.Count -ne 1) { throw '-PgoUse with clang takes one merged .profdata' }
$PgoUse = @($PgoUse | Where-Object { $_ } | ForEach-Object { (Resolve-Path $_).Path -replace '\\', '/' })

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
if ($Compiler -eq 'clang') {
    $clang = if ($env:NFSU_LLVM) { Join-Path $env:NFSU_LLVM 'bin/clang*' } else { $null }
    if (-not ($clang -and (Test-Path $clang)) -and -not (Get-Command clang -ErrorAction SilentlyContinue)) {
        throw ('clang not found: set NFSU_LLVM to the LLVM root (environment or tools/env.local.ps1), or build ' +
               'with -Compiler gcc.')
    }
    # The profile of NFSU_PGO_PROFILE when no profile was given and none is being made.
    if (-not $PgoUse -and -not $PgoGenerate -and -not $NoPgo -and $env:NFSU_PGO_PROFILE) {
        if (Test-Path $env:NFSU_PGO_PROFILE) {
            $PgoUse = @((Resolve-Path $env:NFSU_PGO_PROFILE).Path -replace '\\', '/')
            Write-Host "PGO profile from NFSU_PGO_PROFILE: $($PgoUse[0])"
        } else {
            Write-Host "NFSU_PGO_PROFILE=$env:NFSU_PGO_PROFILE not found: building without a profile"
        }
    }
}
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ($Compiler -eq 'clang') { $suffix = if ($PgoGenerate) { '-pgogen' } elseif ($PgoUse) { '-clangpgo' } else { '-clang' } }
else { $suffix = if ($PgoGenerate) { '-gcov' } elseif ($PgoUse) { '-pgo' } else { '' } }
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
    $lines = @($files | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash })
    if ($FlagAudit) { $lines += 'flag-audit' }
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
        & (Join-Path $PSScriptRoot 'generate.ps1') -Binaries $gameDir -FlagAudit:$FlagAudit
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

# Reconfigures when the compiler options (and the profiles' contents) differ from build-options.txt in the build folder.
$compilerOptions = @("-DNFSU_COMPILER=$Compiler", "-DNFSU_PGO_GENERATE=$(if ($PgoGenerate) { 'ON' } else { 'OFF' })",
                     "-DNFSU_PGO_USE=$($PgoUse -join ';')")
if ($Compiler -eq 'clang') {
    $compilerOptions += "-DNFSU_LTO=$(if ($NoLto) { 'OFF' } else { 'ON' })", "-DNFSU_PGO_LIST=$PgoList"
    if (-not $env:NFSU_LLVM) { Write-Host 'NFSU_LLVM not set: clang from PATH' }
}
$stampLines = $compilerOptions + @(@($PgoUse) + @($PgoList) | Where-Object { $_ } |
                                   ForEach-Object { (Get-FileHash $_ -Algorithm SHA256).Hash })
$optionsFile = Join-Path $build 'build-options.txt'
$previous = if (Test-Path $optionsFile) { (Get-Content $optionsFile -Raw).Trim() } else { '' }
$optionsChanged = $previous -ne ($stampLines -join ' ')
if ($Reconfigure -or -not (Test-Path (Join-Path $build 'build.ninja')) -or $optionsChanged) {
    $toolchain = if ($Compiler -eq 'clang') { "$repo/cmake/vita-clang.cmake" } else { "$env:VITASDK/share/vita.toolchain.cmake" }
    $options = @("-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DCMAKE_BUILD_TYPE=$Config") + $compilerOptions
    $options += if ($env:SCE_PSP2_SDK_DIR) { "-DSCE_PSP2_SDK_DIR=$env:SCE_PSP2_SDK_DIR" } else { '-USCE_PSP2_SDK_DIR' }
    cmake -S $repo -B $build -G Ninja @options
    if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
    Set-Content -Path $optionsFile -Value ($stampLines -join ' ') -Encoding UTF8
}

cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Write-Host "Output: $(Join-Path $build 'NFSUG1VITA.vpk')"
