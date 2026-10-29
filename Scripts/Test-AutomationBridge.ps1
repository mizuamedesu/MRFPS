[CmdletBinding()]
param(
    [string]$EngineRoot,
    [string]$ReportDirectory = (Join-Path $PSScriptRoot '..\Saved\Validation\AutomationBridge'),
    [switch]$InternalWorker
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
if (-not $InternalWorker) {
    $runDirectory = Join-Path $ReportDirectory ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
    $arguments = @('-InternalWorker','-ReportDirectory',$runDirectory)
    if ($EngineRoot) { $arguments += @('-EngineRoot',$EngineRoot) }
    & (Join-Path $PSScriptRoot 'Invoke-MRFPSResourceLimited.ps1') -ScriptFile $PSCommandPath -ScriptArguments $arguments -MinimumFreeGB 10 -MaximumCommitPercent 75 -ReportPath (Join-Path $runDirectory 'guard\report.json')
    return
}
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$engine = Resolve-MRFPSEngine $EngineRoot
$editor = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$project = Join-Path $root 'MRFPS.uproject'
$python = Join-Path $root '.local\auth-tools\Scripts\python.exe'
$control = Join-Path $root 'tools\automation\control.py'
if (-not (Test-Path -LiteralPath $python)) { throw 'Run Scripts/Initialize-AuthTools.ps1 first.' }
New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
$name = 'MRFPS-AutomationTest-' + [Guid]::NewGuid().ToString('N').Substring(0,12)
$arguments = @(('"' + $project + '"'),'/Game/Maps/MRFPSArena','-game','-nullrhi','-nohmd','-nosound',
    '-unattended','-NoSplash','-NoLiveCoding',"-MRFPSWindowName=$name",('-abslog="' + (Join-Path $ReportDirectory 'client.log') + '"'))
$child = Start-Process -FilePath $editor -ArgumentList $arguments -WorkingDirectory $root -PassThru -WindowStyle Hidden
$created = $child.StartTime.ToUniversalTime()
try {
    $statusId = [Guid]::NewGuid().ToString()
    $raw = & $python $control --name $name --operation-id $statusId --operation status --wait-seconds 45
    if ($LASTEXITCODE -ne 0) { throw 'The client did not return its local automation status.' }
    $state = $raw | ConvertFrom-Json
    if ($state.status -ne 'ok' -or $state.data.connected -ne $false -or $state.data.admin -ne $false) { throw 'Unexpected standalone status or role.' }
    $again = & $python $control --name $name --operation-id $statusId --operation status --wait-seconds 2
    if ($LASTEXITCODE -ne 0 -or $again -ne $raw) { throw 'Retry did not return the identical receipt.' }
    $params = Join-Path $ReportDirectory 'admin-request.json'
    @{ action='pause'; layout_revision=1; control_revision=1 } | ConvertTo-Json -Compress | Set-Content -LiteralPath $params -Encoding UTF8
    $adminId = [Guid]::NewGuid().ToString()
    $denial = & $python $control --name $name --operation-id $adminId --operation admin.action --parameters-file $params --wait-seconds 8
    $adminResult = $denial | ConvertFrom-Json
    if ($LASTEXITCODE -eq 0 -or $adminResult.code -ne 'ADMIN_REQUIRED') { throw 'Standalone client unexpectedly accepted an admin operation.' }
    [ordered]@{ passed=$true; standalone_status='ok'; repeated_operation_id='same_receipt'; unauthorized_admin='ADMIN_REQUIRED'; physical_devices_used=$false } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $ReportDirectory 'result.json') -Encoding UTF8
    Write-Output 'Automation bridge read-only and role-rejection checks passed.'
} finally {
    $current = Get-Process -Id $child.Id -ErrorAction SilentlyContinue
    if ($current -and $current.StartTime.ToUniversalTime() -eq $created) { Stop-Process -Id $current.Id -ErrorAction Stop }
}
