# Sets up the toolchain environment for this shell. Dot-source it: . tools/env.ps1
# Machine-specific settings come from the environment or from tools/env.local.ps1 (ignored by git):
#   $env:VITASDK          = '<VitaSDK root>'                    optional: else the pinned VitaSDK in .deps/
#   $env:NFSU_GAME_DIR    = '<the game folder with Speed.exe>'  optional: else tools/build.ps1 -Game
#   $env:NFSU_LLVM        = '<LLVM root, with bin/clang>'       optional: else the pinned LLVM in .deps/ (clang build)
#   $env:NFSU_PGO_PROFILE = '<merged .profdata>'                optional: the clang build's default PGO profile
# Python 3 must be installed; CMake and Ninja are installed with pip when they are missing, VitaSDK and LLVM are
# downloaded by tools/deps.py.

$local = Join-Path $PSScriptRoot 'env.local.ps1'
if (Test-Path $local) { . $local }

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

# Folder of a pinned dependency from tools/deps.py (downloaded on first use).
function Get-Dependency([string]$Name) {
    $ErrorActionPreference = 'Continue'
    $folder = & (Get-Python3) -I (Join-Path $PSScriptRoot 'deps.py') $Name 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) { Write-Host $_.Exception.Message } else { $_ }
    } | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0 -or -not $folder -or -not (Test-Path $folder)) { throw "Downloading $Name failed" }
    return $folder
}

if (-not $env:VITASDK -or -not (Test-Path (Join-Path $env:VITASDK 'share/vita.toolchain.cmake'))) {
    if ($env:VITASDK) { Write-Host "VITASDK=$env:VITASDK has no share/vita.toolchain.cmake: using the pinned VitaSDK" }
    $env:VITASDK = Get-Dependency 'vitasdk'
}
$bin = Join-Path $env:VITASDK 'bin'
if (($env:PATH -split [IO.Path]::PathSeparator) -notcontains $bin) { $env:PATH = $bin + [IO.Path]::PathSeparator + $env:PATH }

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
