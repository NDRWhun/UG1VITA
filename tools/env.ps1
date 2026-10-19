# Sets up the toolchain environment for this shell. Dot-source it: . tools/env.ps1
# Machine-specific settings come from the environment or from tools/env.local.ps1 (ignored by git):
#   $env:VITASDK          = '<VitaSDK root>'                    required (https://vitasdk.org)
#   $env:NFSU_GAME_DIR    = '<the game folder with Speed.exe>'  optional: else tools/build.ps1 -Game
#   $env:SCE_PSP2_SDK_DIR = '<official PSP2 SDK root>'          optional: recompiles shaders with psp2cgc
# Python 3 must be installed; CMake and Ninja are installed with pip when they are missing.

$local = Join-Path $PSScriptRoot 'env.local.ps1'
if (Test-Path $local) { . $local }

if (-not $env:VITASDK -or -not (Test-Path (Join-Path $env:VITASDK 'share/vita.toolchain.cmake'))) {
    throw ('VitaSDK not found. Install it (https://vitasdk.org) and set VITASDK to its root, as an environment ' +
           'variable or in tools/env.local.ps1.')
}
$bin = Join-Path $env:VITASDK 'bin'
if (($env:PATH -split [IO.Path]::PathSeparator) -notcontains $bin) { $env:PATH = $bin + [IO.Path]::PathSeparator + $env:PATH }

# The first python on PATH that runs Python 3; the Microsoft Store's python alias only prints an install hint.
function Get-Python3 {
    foreach ($name in 'python', 'python3') {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $command) { continue }
        $ErrorActionPreference = 'Continue'
        try { if ((& $command.Source -c 'import sys; print(sys.version_info[0])' 2>$null) -eq '3') { return $command.Source } }
        catch { }
    }
    throw ('Python 3 not found on PATH (https://www.python.org/downloads/). On Windows, the "python" app execution ' +
           'alias of the Microsoft Store is not Python: install Python, or turn the alias off in Settings.')
}

# Runs Python and returns its exit code; failure is judged by the exit code, since Windows PowerShell 5.1 would stop
# on any stderr line.
function Invoke-Python {
    $ErrorActionPreference = 'Continue'
    & (Get-Python3) @args | Write-Host
    return $LASTEXITCODE
}

# CMake and Ninja from pip when they are not on PATH; pip's script folders are added to PATH for this shell.
$missing = @('cmake', 'ninja') | Where-Object { -not (Get-Command $_ -ErrorAction SilentlyContinue) }
if ($missing) {
    Write-Host "Installing $($missing -join ' and ') with pip..."
    if ((Invoke-Python -m pip install --user @missing) -ne 0) { throw "pip install $($missing -join ' ') failed" }
    $ErrorActionPreference = 'Continue'
    $scripts = & (Get-Python3) -c "import os, sysconfig; print(sysconfig.get_path('scripts')); print(sysconfig.get_path('scripts', os.name + '_user'))"
    foreach ($dir in $scripts) {
        if ($dir -and (Test-Path $dir) -and (($env:PATH -split [IO.Path]::PathSeparator) -notcontains $dir)) {
            $env:PATH = $dir + [IO.Path]::PathSeparator + $env:PATH
        }
    }
    $still = @('cmake', 'ninja') | Where-Object { -not (Get-Command $_ -ErrorAction SilentlyContinue) }
    if ($still) { throw "Installed with pip but not found on PATH: $($still -join ', ')" }
}

Write-Host "VITASDK=$env:VITASDK"
if ($env:SCE_PSP2_SDK_DIR) { Write-Host "SCE_PSP2_SDK_DIR=$env:SCE_PSP2_SDK_DIR (shaders are compiled)" }
else { Write-Host 'SCE_PSP2_SDK_DIR not set: using the prebuilt shaders in shaders/gxp' }
