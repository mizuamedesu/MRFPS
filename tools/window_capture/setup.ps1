param(
    [string]$Python = 'C:\UnrealEngine\UE5.8\Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
)
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT' -or -not [IO.Path]::IsPathRooted($Python) -or -not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    throw 'Specify an existing absolute path to a 64-bit Python 3.11 executable.'
}
$version = & $Python -c 'import sys; print(int(sys.version_info[:2] == (3,11) and sys.maxsize > 2**32))'
if ($LASTEXITCODE -ne 0 -or $version -ne '1') { throw 'The pinned wheels require 64-bit Python 3.11.' }
& $Python (Join-Path $PSScriptRoot 'capture.py') init | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the private capture directory.' }
$root = Join-Path $env:LOCALAPPDATA 'MRFPS\WindowCapture'
$venv = Join-Path $root 'venv'
if ((Test-Path -LiteralPath $venv) -and ((Get-Item -LiteralPath $venv -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'The capture environment must not be a reparse point.'
}
& $Python -m venv $venv
if ($LASTEXITCODE -ne 0) { throw 'Could not create the isolated capture environment.' }
$isolatedPython = Join-Path $venv 'Scripts\python.exe'
& $isolatedPython -m pip --isolated install --only-binary=:all: --require-hashes --no-deps -r (Join-Path $PSScriptRoot 'requirements.txt')
if ($LASTEXITCODE -ne 0) { throw 'Could not install the pinned capture dependencies.' }
& $isolatedPython -m pip check
if ($LASTEXITCODE -ne 0) { throw 'The installed capture dependencies are inconsistent.' }
Write-Output 'Capture environment ready.'
