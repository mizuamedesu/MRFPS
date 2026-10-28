[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$environment = Join-Path $root '.local\video-tools'
$python = Join-Path $environment 'Scripts\python.exe'
$uv = (Get-Command uv -ErrorAction Stop).Source
if (-not (Test-Path -LiteralPath $python)) { Invoke-MRFPSCommand $uv @('venv','--python','3.12',$environment) }
Invoke-MRFPSCommand $uv @('pip','install','--python',$python,'-r',(Join-Path $root 'tools\video\requirements.txt'))
