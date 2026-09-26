param([string]$FirmwarePath)

$ErrorActionPreference = 'Stop'
$pythonScript = Join-Path $PSScriptRoot 'scripts\emulator.py'
$forwardArguments = @()
if (-not [string]::IsNullOrWhiteSpace($FirmwarePath)) { $forwardArguments += $FirmwarePath }

$pythonLauncher = Get-Command 'py.exe' -ErrorAction SilentlyContinue
if ($pythonLauncher) {
  & $pythonLauncher.Source -3 $pythonScript @forwardArguments
} else {
  $python = Get-Command 'python.exe' -ErrorAction SilentlyContinue
  if (-not $python) { throw 'Python 3 is required to run the emulator. Install Python 3 and try again.' }
  & $python.Source $pythonScript @forwardArguments
}

if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
