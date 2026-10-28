[CmdletBinding()]
param(
    [string]$EngineRoot,
    [ValidateRange(1,32)][int]$Inspectors = 3,
    [ValidateRange(15,600)][int]$DurationSeconds = 90,
    [ValidateRange(1,65535)][int]$Port = 17777,
    [ValidateRange(15,600)][int]$StartupTimeoutSeconds = 180,
    [string]$ReportDirectory,
    [switch]$KeepProcesses,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$engine = Resolve-MRFPSEngine $EngineRoot
$executable = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor.exe'
$project = Join-Path $root 'MRFPS.uproject'
if (-not $ReportDirectory) { $ReportDirectory = Join-Path $root ('Saved\Validation\Multiplayer-' + (Get-Date -Format yyyyMMdd-HHmmss)) }
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
$instances = [Collections.Generic.List[object]]::new()
$authDirectory = Join-Path $root ('.local\auth-fixtures\multiplayer-' + [Guid]::NewGuid().ToString('N'))
$authKeys = $null
$authTokens = $null

function Quote-MRFPSArgument([string]$Value) {
    '"' + (($Value -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"'
}

function Start-MRFPSInstance([string]$Name, [string[]]$ExtraArguments) {
    $log = Join-Path $ReportDirectory ($Name + '.log')
    if ($Name -ne 'server') {
        $tokenPath = if ($DryRun) { Join-Path $authDirectory ($Name + '.jwt') } else { $authTokens.tokens.$Name }
        if (-not $tokenPath) { throw "No one-connection token was issued for $Name." }
        $ExtraArguments += @("-MRFPSAuthTokenFile=$tokenPath", "-MRFPSAuthPublicKeyFile=$($authKeys.public_key_path)",'-MRFPSAuthAudience=mrfps')
    }
    $arguments = @($project) + $ExtraArguments + @('-nullrhi','-nohmd','-nosound','-unattended','-NoSplash','-NoLiveCoding','-stdout','-FullStdOutLogOutput',"-abslog=$log",'-MRFPSSmoke')
    $argumentLine = ($arguments | ForEach-Object { Quote-MRFPSArgument $_ }) -join ' '
    Write-Host "$Name : $executable $argumentLine"
    $process = $null
    if (-not $DryRun) {
        $priorAuth = @{}
        try {
            # Runtime clients need no SDK setup; avoid waiting on a concurrent packaging UBT mutex.
            $priorAuth['UE_SKIP_UBT_SDK_SETUP'] = [Environment]::GetEnvironmentVariable('UE_SKIP_UBT_SDK_SETUP','Process')
            $env:UE_SKIP_UBT_SDK_SETUP = '1'
            if ($Name -eq 'server') {
                foreach ($variable in @('MRFPS_AUTH_PUBLIC_KEY_B64','MRFPS_AUTH_PUBLIC_KEY_PEM','MRFPS_AUTH_AUDIENCE')) { $priorAuth[$variable] = [Environment]::GetEnvironmentVariable($variable,'Process') }
                $env:MRFPS_AUTH_PUBLIC_KEY_B64 = $authKeys.public_key_b64
                $env:MRFPS_AUTH_PUBLIC_KEY_PEM = $null
                $env:MRFPS_AUTH_AUDIENCE = 'mrfps'
            }
            $process = Start-Process -FilePath $executable -ArgumentList $argumentLine -WorkingDirectory $root -WindowStyle Hidden -PassThru
        } finally { foreach ($variable in $priorAuth.Keys) { [Environment]::SetEnvironmentVariable($variable,$priorAuth[$variable],'Process') } }
    }
    $instances.Add([PSCustomObject]@{ Name = $Name; Process = $process; Log = $log; Arguments = $arguments })
}

function Read-MRFPSLog([string]$Name) {
    $instance = $instances | Where-Object Name -EQ $Name | Select-Object -First 1
    if ($instance -and (Test-Path -LiteralPath $instance.Log)) {
        $stream = $null
        $reader = $null
        try {
            $stream = [IO.File]::Open($instance.Log,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
            $reader = New-Object IO.StreamReader($stream)
            return $reader.ReadToEnd()
        } finally { if ($reader) { $reader.Dispose() } elseif ($stream) { $stream.Dispose() } }
    }
    return ''
}

function Wait-MRFPSCondition([string]$Description, [scriptblock]$Condition) {
    if ($DryRun) { return }
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        foreach ($instance in $instances | Where-Object Name -NE 'extra-player') {
            if ($instance.Process.HasExited) { throw "$($instance.Name) exited while waiting for $Description. Inspect $($instance.Log)." }
        }
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 500
    }
    throw "Timed out after $StartupTimeoutSeconds seconds waiting for $Description. Evidence: $ReportDirectory"
}

try {
    if (-not $DryRun) {
        $map = Join-Path $root 'Content\Maps\MRFPSArena.umap'
        if (-not (Test-Path -LiteralPath $map)) { throw 'Generated arena map is missing. Complete the Editor build and -run=MRFPSSetup before starting multiplayer.' }
        if (-not (Test-Path -LiteralPath (Join-Path $root 'Binaries\Win64\UnrealEditor-MRFPS.dll'))) { throw 'The MRFPS editor game module is missing. Complete the Editor build before starting multiplayer.' }
        $socketProbe = New-Object Net.Sockets.UdpClient
        try { $socketProbe.Client.Bind((New-Object Net.IPEndPoint([Net.IPAddress]::Any, $Port))) }
        catch { throw "UDP $Port is already occupied; choose -Port." }
        finally { $socketProbe.Dispose() }
    }
    $authKeys = New-MRFPSTestAuthKeys -Directory $authDirectory -DryRun:$DryRun
    Start-MRFPSInstance 'server' @('/Game/Maps/MRFPSArena','-server',"-port=$Port",'-MRFPSAllowDesktop=1')
    Wait-MRFPSCondition 'the authenticated server world and transport fingerprint' {
        $startup = Read-MRFPSLog 'server'
        ($startup -match 'MRFPS_SERVER_READY') -and ($startup -match 'MRFPS_AUTH_TRANSPORT_READY fingerprint=[0-9a-f]{64}')
    }
    if (-not $DryRun) {
        $transport = [regex]::Match((Read-MRFPSLog 'server'),'MRFPS_AUTH_TRANSPORT_READY fingerprint=([0-9a-f]{64})')
        $authTokens = Complete-MRFPSTestAuthTokens -Directory $authDirectory -Fingerprint $transport.Groups[1].Value -Inspectors $Inspectors
    }
    $url = "127.0.0.1:$Port"
    # Load observers first so shader/startup timing cannot hide the short-lived wall.
    for ($i = 0; $i -lt $Inspectors; $i++) {
        Start-MRFPSInstance "inspector-$i" @('/Game/Maps/MRFPSArena',"-MRFPSServer=$url",'-game','-MRFPSDesktop','-MRFPSInspector')
    }
    Wait-MRFPSCondition 'every inspector to receive its observer role' {
        $ready = $true
        for ($inspectorIndex = 0; $inspectorIndex -lt $Inspectors; $inspectorIndex++) {
            if ((Read-MRFPSLog "inspector-$inspectorIndex") -notmatch 'MRFPS_SNAPSHOT players=0 role=inspector') { $ready = $false }
        }
        $ready
    }
    Start-MRFPSInstance 'player-0' @('/Game/Maps/MRFPSArena',"-MRFPSServer=$url",'-game','-MRFPSDesktop','-MRFPSAutoReady','-MRFPSSmokeRole=player')
    Wait-MRFPSCondition 'player 0 to own and calibrate slot 0' { (Read-MRFPSLog 'server') -match 'MRFPS_CALIBRATED server slot=0 ready=1' }
    Start-MRFPSInstance 'player-1' @('/Game/Maps/MRFPSArena',"-MRFPSServer=$url",'-game','-MRFPSDesktop','-MRFPSAutoReady','-MRFPSSmokeRole=shield')
    Wait-MRFPSCondition 'player 1 calibration and match start' {
        $log = Read-MRFPSLog 'server'
        ($log -match 'MRFPS_CALIBRATED server slot=1 ready=1') -and ($log -match 'MRFPS_MATCH playing')
    }
    Start-MRFPSInstance 'extra-player' @('/Game/Maps/MRFPSArena',"-MRFPSServer=$url",'-game','-MRFPSDesktop','-MRFPSAutoReady')
    if ($DryRun) { return }
    $deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        foreach ($instance in $instances | Where-Object Name -NE 'extra-player') {
            if ($instance.Process.HasExited) { throw "$($instance.Name) exited during the smoke test. Inspect $($instance.Log)." }
        }
        Start-Sleep -Seconds 1
    }
    $serverLog = Read-MRFPSLog 'server'
    $allLogs = ($instances | ForEach-Object { Read-MRFPSLog $_.Name }) -join "`n"
    $checks = [ordered]@{
        TwoPlayerSlots = (($serverLog -match 'MRFPS_JOIN role=player slot=0') -and ($serverLog -match 'MRFPS_JOIN role=player slot=1'))
        InspectorsJoined = ([regex]::Matches($serverLog, 'MRFPS_JOIN role=inspector').Count -ge $Inspectors)
        ExtraPlayerRejected = ((Read-MRFPSLog 'extra-player') -match 'ARENA_FULL')
        ExactlyTwoPlayerJoins = ([regex]::Matches($serverLog, 'MRFPS_JOIN role=player').Count -eq 2)
        MatchStarted = ($serverLog -match 'MRFPS_MATCH playing')
        PlayersCalibrated = (($serverLog -match 'MRFPS_CALIBRATED server slot=0 ready=1') -and ($serverLog -match 'MRFPS_CALIBRATED server slot=1 ready=1'))
        WallSpawned = ($serverLog -match 'MRFPS_WALL')
        AuthoritativeDamage = ($serverLog -match 'MRFPS_DAMAGE')
        ShieldBlockedShot = ($serverLog -match 'MRFPS_BLOCK')
        NoFatalError = ($allLogs -notmatch 'Fatal error:|Assertion failed:|Unhandled Exception:')
        PublicKeyAuthenticatedServer = ($serverLog -match 'MRFPS_AUTH_TRANSPORT_READY fingerprint=[0-9a-f]{64}')
        NoCredentialLeak = (Test-MRFPSCredentialPrivacy -Text $allLogs -TokenFiles @($authTokens.tokens.PSObject.Properties | ForEach-Object Value))
    }
    for ($i = 0; $i -lt $Inspectors; $i++) {
        $inspectorLog = Read-MRFPSLog "inspector-$i"
        $checks["Inspector${i}ReceivedTwoPlayers"] = ($inspectorLog -match 'MRFPS_SNAPSHOT players=2 role=inspector')
        $checks["Inspector${i}ReceivedWall"] = ($inspectorLog -match 'MRFPS_SNAPSHOT[^\r\n]*walls=[1-9][0-9]*')
        $checks["Inspector${i}ReceivedDamage"] = ($inspectorLog -match 'MRFPS_SNAPSHOT[^\r\n]*p1hp=(?:[0-9]|[1-9][0-9])\.[0-9]')
        $checks["Inspector${i}ViewedBothPlayers"] = (($inspectorLog -match 'MRFPS_VIEW inspector=player slot=0') -and ($inspectorLog -match 'MRFPS_VIEW inspector=player slot=1'))
        $checks["Inspector${i}ReturnedToFreeView"] = ($inspectorLog -match 'MRFPS_VIEW inspector=free')
    }
    $failed = @($checks.Keys | Where-Object { -not $checks[$_] })
    $report = [ordered]@{ TimestampUtc = [DateTime]::UtcNow.ToString('o'); Passed = ($failed.Count -eq 0); Scope = 'Desktop network smoke; physical MR/glove/MoBits validation is separate'; Port = $Port; InspectorCount = $Inspectors; Checks = $checks; Failures = $failed; Processes = @($instances | ForEach-Object { [ordered]@{ Name = $_.Name; Id = $_.Process.Id; Log = $_.Log; Exited = $_.Process.HasExited } }) }
    $report | ConvertTo-Json -Depth 7 | Out-File -LiteralPath (Join-Path $ReportDirectory 'report.json') -Encoding utf8
    if ($failed.Count) { throw "Multiplayer smoke failed: $($failed -join ', '). Evidence: $ReportDirectory" }
    Write-Host "Multiplayer smoke passed. Evidence: $ReportDirectory"
} catch {
    $_ | Out-String | Out-File -LiteralPath (Join-Path $ReportDirectory 'harness-error.txt') -Encoding utf8
    if (-not (Test-Path -LiteralPath (Join-Path $ReportDirectory 'report.json'))) {
        [ordered]@{ TimestampUtc = [DateTime]::UtcNow.ToString('o'); Passed = $false; Failure = $_.Exception.Message; Processes = @($instances | ForEach-Object { [ordered]@{ Name = $_.Name; Id = $(if ($_.Process) { $_.Process.Id } else { $null }); Log = $_.Log } }) } |
            ConvertTo-Json -Depth 6 | Out-File -LiteralPath (Join-Path $ReportDirectory 'report.json') -Encoding utf8
    }
    throw
} finally {
    if (-not $KeepProcesses) {
        foreach ($instance in $instances) {
            if ($instance.Process -and -not $instance.Process.HasExited) {
                $instance.Process.Kill()
                [void]$instance.Process.WaitForExit(5000)
            }
        }
    }
}
