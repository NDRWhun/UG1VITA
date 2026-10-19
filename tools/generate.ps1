# Generates the C++ recompilation of the game into src/nfsu/generated with tools/recompiler.
# Usage: tools/generate.ps1 -Binaries <dir with Speed.exe> [-Out <dir>] [-Analysis <file.json>] [-FpuCheck]
# Needs Python 3; capstone is fetched by tools/recompiler/bootstrap.py when it is missing. Only files whose content
# changed are replaced, so the build recompiles just those; generated files of earlier runs are deleted.
param(
    [Parameter(Mandatory = $true)][string]$Binaries,
    [string]$Out,
    [string]$Analysis,
    [switch]$FpuCheck
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Out) { $Out = Join-Path $root '../src/nfsu/generated' }

# The first python on PATH that runs Python 3; the Microsoft Store's python alias only prints an install hint.
function Test-Python3([string]$exe) {
    $ErrorActionPreference = 'Continue'
    try { return (& $exe -c 'import sys; print(sys.version_info[0])' 2>$null) -eq '3' } catch { return $false }
}
$python = $null
foreach ($name in 'python', 'python3') {
    $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command -and (Test-Python3 $command.Source)) { $python = $command; break }
}
if (-not $python) {
    throw ('Python 3 not found on PATH (https://www.python.org/downloads/). On Windows, the "python" app execution ' +
           'alias of the Microsoft Store is not Python: install Python, or turn the alias off in Settings.')
}

# Runs Python and returns its exit code; failure is judged by the exit code, since Windows PowerShell 5.1 would stop
# on any stderr line.
function Invoke-Python {
    $ErrorActionPreference = 'Continue'
    & $python.Source @args | Write-Host
    return $LASTEXITCODE
}

New-Item -ItemType Directory -Force $Out | Out-Null
$arguments = @((Join-Path $root 'recompiler/generate.py'), '--binaries', $Binaries, '--out', (Resolve-Path $Out).Path)
if ($Analysis) { $arguments += @('--analysis', $Analysis) }
if ($FpuCheck) { $arguments += '--fpu-check' }
if ((Invoke-Python @arguments) -ne 0) { throw 'generation failed' }
