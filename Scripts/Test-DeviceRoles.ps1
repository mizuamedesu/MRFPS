[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RunDirectory,
    [string]$Executable,
    [string]$Server='mrfps.mizuame.app',
    [ValidateRange(1,65535)][int]$Port=7777,
    [string]$Container,
    [string]$Fingerprint,
    [string]$AdminDirectory=(Join-Path $env:LOCALAPPDATA 'MRFPS\Admin'),
    [ValidateRange(2,8)][int]$InspectorCount=2,
    [ValidateSet('full','player-limit','admin-limit','inspector-multi','admin-inspector','solo-bot')][string]$Scenario='full',
    [ValidateRange(0,1800)][int]$HoldSeconds=0,
    [switch]$Editor,
    [switch]$AllowExistingPlayer,
    [switch]$Stop,
    [switch]$InternalWorker
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$run=[IO.Path]::GetFullPath($RunDirectory)
$python=Join-Path $root 'tools\client\role_probe.py'
$runtime=''
if ($env:MRFPS_UE_ROOT) { $runtime=Join-Path $env:MRFPS_UE_ROOT 'Engine\Binaries\ThirdParty\Python3\Win64\python.exe' }
if (-not $runtime -or -not (Test-Path -LiteralPath $runtime)) { $runtime='C:\UnrealEngine\UE5.8\Engine\Binaries\ThirdParty\Python3\Win64\python.exe' }
if (-not (Test-Path -LiteralPath $runtime)) { throw 'Set MRFPS_UE_ROOT to a UE5.8 installation with its bundled Python.' }
if ($Stop) { & $runtime $python stop --run-dir $run; exit $LASTEXITCODE }
if (-not $Executable -or -not $Container -or $Fingerprint -notmatch '^[0-9a-f]{64}$') { throw 'Executable, Container, and a 64-digit lowercase Fingerprint are required.' }
if ($Server -notmatch '^[A-Za-z0-9.-]{1,253}$' -or $Container -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,62}$') { throw 'Invalid server or container.' }
$arguments=@('run','--run-dir',$run,'--executable',([IO.Path]::GetFullPath($Executable)),'--server',$Server,'--port',"$Port",
    '--container',$Container,'--fingerprint',$Fingerprint,'--admin-dir',([IO.Path]::GetFullPath($AdminDirectory)),
    '--inspectors',"$InspectorCount",'--hold-seconds',"$HoldSeconds",'--scenario',$Scenario)
if ($Editor) { $arguments+='--editor' }
if ($AllowExistingPlayer) { $arguments+='--allow-existing-player' }
if ($InternalWorker) { & $runtime $python @arguments; exit $LASTEXITCODE }
$existing=Join-Path $run 'report.json'
if (Test-Path -LiteralPath $existing) {
    $prior=Get-Content -Raw -Encoding UTF8 -LiteralPath $existing | ConvertFrom-Json
    [ordered]@{status='already_started'; priorStatus=$prior.status; report=$existing} | ConvertTo-Json -Compress
    exit 0
}
$forward=@('-InternalWorker','-RunDirectory',$run,'-Executable',([IO.Path]::GetFullPath($Executable)),'-Server',$Server,'-Port',"$Port",
    '-Container',$Container,'-Fingerprint',$Fingerprint,'-AdminDirectory',([IO.Path]::GetFullPath($AdminDirectory)),
    '-InspectorCount',"$InspectorCount",'-HoldSeconds',"$HoldSeconds",'-Scenario',$Scenario)
if ($Editor) { $forward+='-Editor' }
if ($AllowExistingPlayer) { $forward+='-AllowExistingPlayer' }
& (Join-Path $PSScriptRoot 'Invoke-MRFPSResourceLimited.ps1') -ScriptFile $PSCommandPath -ScriptArguments $forward `
    -MinimumFreeGB 10 -MaximumCommitPercent 75 -ReportPath ($run+'-guard\report.json')
exit $LASTEXITCODE
