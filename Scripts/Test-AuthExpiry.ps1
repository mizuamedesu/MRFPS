[CmdletBinding()]
param(
    [string]$EngineRoot,
    [ValidateRange(1,65535)][int]$Port = 17920,
    [ValidateRange(30,120)][int]$LifetimeSeconds = 30,
    [ValidateRange(30,600)][int]$StartupTimeoutSeconds = 120,
    [string]$ReportDirectory
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$engine = Resolve-MRFPSEngine $EngineRoot
$executable = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor.exe'
$project = Join-Path $root 'MRFPS.uproject'
if (-not $ReportDirectory) { $ReportDirectory = Join-Path $root ('Saved\Validation\AuthExpiry-' + (Get-Date -Format yyyyMMdd-HHmmss)) }
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
$credentialDirectory = Join-Path $root ('.local\auth-fixtures\expiry-' + [Guid]::NewGuid().ToString('N'))
$instances = [Collections.Generic.List[object]]::new()
$tokens = [Collections.Generic.List[string]]::new()
$keys = $null

function Quote-ExpiryArgument([string]$Value) { '"' + (($Value -replace '(\\*)"','$1$1\"') -replace '(\\+)$','$1$1') + '"' }
function Read-ExpiryLog([string]$Name) {
    $instance = $instances | Where-Object Name -EQ $Name | Select-Object -First 1
    if (-not $instance -or -not (Test-Path -LiteralPath $instance.Log)) { return '' }
    $reader = $null
    try {
        $stream = [IO.File]::Open($instance.Log,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader = New-Object IO.StreamReader($stream)
        return $reader.ReadToEnd()
    } finally { if ($reader) { $reader.Dispose() } }
}
function Wait-ExpiryEvidence([string]$Description,[scriptblock]$Condition,[int]$Timeout = $StartupTimeoutSeconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Timeout)
    while ([DateTime]::UtcNow -lt $deadline) {
        foreach ($instance in $instances) { if ($instance.Process.HasExited) { throw "$($instance.Name) exited while waiting for $Description." } }
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for $Description. Evidence: $ReportDirectory"
}
function Start-ExpiryInstance([string]$Name,[string]$Token) {
    $log = Join-Path $ReportDirectory ($Name + '.log')
    if ($Name -eq 'server') { $arguments = @($project,'/Game/Maps/MRFPSArena','-server',"-port=$Port") }
    else {
        $arguments = @($project,'/Game/Maps/MRFPSArena','-game',"-MRFPSServer=127.0.0.1:$Port",'-MRFPSDesktop',"-MRFPSAuthTokenFile=$Token","-MRFPSAuthPublicKeyFile=$($keys.public_key_path)",'-MRFPSAuthAudience=mrfps')
        if ($Name -eq 'inspector') { $arguments += '-MRFPSInspector' }
    }
    $arguments += @('-nullrhi','-nohmd','-nosound','-unattended','-NoSplash','-NoLiveCoding','-stdout','-FullStdOutLogOutput','-ExecCmds=t.MaxFPS 30',"-abslog=$log")
    $prior = @{}
    try {
        $prior['UE_SKIP_UBT_SDK_SETUP'] = [Environment]::GetEnvironmentVariable('UE_SKIP_UBT_SDK_SETUP','Process')
        $env:UE_SKIP_UBT_SDK_SETUP = '1'
        if ($Name -eq 'server') {
            foreach ($variable in @('MRFPS_AUTH_PUBLIC_KEY_B64','MRFPS_AUTH_PUBLIC_KEY_PEM','MRFPS_AUTH_AUDIENCE')) { $prior[$variable] = [Environment]::GetEnvironmentVariable($variable,'Process') }
            $env:MRFPS_AUTH_PUBLIC_KEY_B64 = $keys.public_key_b64
            $env:MRFPS_AUTH_PUBLIC_KEY_PEM = $null
            $env:MRFPS_AUTH_AUDIENCE = 'mrfps'
        }
        $process = Start-Process -FilePath $executable -ArgumentList (($arguments | ForEach-Object { Quote-ExpiryArgument $_ }) -join ' ') -WorkingDirectory $root -WindowStyle Hidden -PassThru
    } finally { foreach ($variable in $prior.Keys) { [Environment]::SetEnvironmentVariable($variable,$prior[$variable],'Process') } }
    $instances.Add([PSCustomObject]@{Name=$Name; Process=$process; Log=$log})
}
try {
    $probe = New-Object Net.Sockets.UdpClient
    try { $probe.Client.Bind((New-Object Net.IPEndPoint([Net.IPAddress]::Any,$Port))) } finally { $probe.Dispose() }
    $keys = New-MRFPSTestAuthKeys -Directory $credentialDirectory
    Start-ExpiryInstance 'server' ''
    Wait-ExpiryEvidence 'the authenticated server' { (Read-ExpiryLog 'server') -match 'MRFPS_AUTH_TRANSPORT_READY fingerprint=[0-9a-f]{64}' -and (Read-ExpiryLog 'server') -match 'MRFPS_SERVER_READY' }
    $fingerprint = [regex]::Match((Read-ExpiryLog 'server'),'MRFPS_AUTH_TRANSPORT_READY fingerprint=([0-9a-f]{64})').Groups[1].Value
    $python = Join-Path $root '.local\auth-tools\Scripts\python.exe'
    foreach ($role in @('player','inspector')) {
        $token = Join-Path $credentialDirectory ($role + '.jwt')
        Invoke-MRFPSCommand $python @((Join-Path $root 'tools\auth\mrfps_tokens.py'),'issue','--private-key',(Join-Path $credentialDirectory 'issuer-private.pem'),'--dev-unencrypted','--subject',('expiry-' + $role),'--role',$role,'--dtls-fingerprint',$fingerprint,'--ttl',"$LifetimeSeconds",'--output',$token) | Out-Null
        $tokens.Add($token)
        Start-ExpiryInstance $role $token
    }
    foreach ($role in @('player','inspector')) {
        Wait-ExpiryEvidence "$role authentication and completed encrypted travel" {
            (Read-ExpiryLog 'server') -match "MRFPS_AUTH_ACCEPT role=$role subject=expiry-$role" -and (Read-ExpiryLog $role) -match 'TravelCompleted Pending net game travel completed'
        }
    }
    Wait-ExpiryEvidence 'both authenticated sessions to expire and disconnect' {
        [regex]::Matches((Read-ExpiryLog 'server'),'MRFPS_AUTH_SESSION_EXPIRED').Count -eq 2 -and
        (Read-ExpiryLog 'player') -match 'Browse Started Browse: "/Game/Maps/MRFPSArena\?closed' -and
        (Read-ExpiryLog 'inspector') -match 'Browse Started Browse: "/Game/Maps/MRFPSArena\?closed'
    } ($LifetimeSeconds + 45)
    $allLogs = ($instances | ForEach-Object { Read-ExpiryLog $_.Name }) -join "`n"
    $checks = [ordered]@{
        PlayerAccepted = (Read-ExpiryLog 'server') -match 'MRFPS_AUTH_ACCEPT role=player subject=expiry-player'
        InspectorAccepted = (Read-ExpiryLog 'server') -match 'MRFPS_AUTH_ACCEPT role=inspector subject=expiry-inspector'
        TwoServerExpirations = [regex]::Matches((Read-ExpiryLog 'server'),'MRFPS_AUTH_SESSION_EXPIRED').Count -eq 2
        PlayerDisconnected = (Read-ExpiryLog 'player') -match 'Browse Started Browse: "/Game/Maps/MRFPSArena\?closed'
        InspectorDisconnected = (Read-ExpiryLog 'inspector') -match 'Browse Started Browse: "/Game/Maps/MRFPSArena\?closed'
        ServerStillRunning = -not $instances[0].Process.HasExited
        NoCredentialLeak = Test-MRFPSCredentialPrivacy -Text $allLogs -TokenFiles $tokens.ToArray()
        NoFatalError = $allLogs -notmatch 'Fatal error:|Assertion failed:|Unhandled Exception:'
    }
    $failed = @($checks.Keys | Where-Object { -not $checks[$_] })
    [ordered]@{TimestampUtc=[DateTime]::UtcNow.ToString('o'); Passed=$failed.Count -eq 0; Scope='Actual UE player and inspector sessions accepted with signed pinned tokens, then disconnected by server at expiry'; LifetimeSeconds=$LifetimeSeconds; Checks=$checks; Failures=$failed; Processes=@($instances | ForEach-Object { [ordered]@{Name=$_.Name;Id=$_.Process.Id;Log=$_.Log} })} | ConvertTo-Json -Depth 6 | Out-File -LiteralPath (Join-Path $ReportDirectory 'report.json') -Encoding utf8
    if ($failed.Count) { throw "Auth expiry checks failed: $($failed -join ', ')" }
    Write-Host "Actual UE session expiry checks passed. Evidence: $ReportDirectory"
} catch {
    $_ | Out-String | Out-File -LiteralPath (Join-Path $ReportDirectory 'harness-error.txt') -Encoding utf8
    if (-not (Test-Path -LiteralPath (Join-Path $ReportDirectory 'report.json'))) { [ordered]@{TimestampUtc=[DateTime]::UtcNow.ToString('o');Passed=$false;Failure=$_.Exception.Message;Scope='Incomplete actual UE session expiry run'} | ConvertTo-Json | Out-File -LiteralPath (Join-Path $ReportDirectory 'report.json') -Encoding utf8 }
    throw
} finally {
    foreach ($instance in $instances) { if ($instance.Process -and -not $instance.Process.HasExited) { $instance.Process.Kill(); [void]$instance.Process.WaitForExit(5000) } }
}
