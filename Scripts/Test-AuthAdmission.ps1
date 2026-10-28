[CmdletBinding()]
param([string]$EngineRoot, [ValidateRange(1,65535)][int]$Port = 17877,
    [ValidateRange(15,600)][int]$StartupTimeoutSeconds = 180, [string]$ReportDirectory, [switch]$DryRun)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$engine = Resolve-MRFPSEngine $EngineRoot
$executable = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor.exe'
$project = Join-Path $root 'MRFPS.uproject'
if (-not $ReportDirectory) { $ReportDirectory = Join-Path $root ('Saved\Validation\AuthAdmission-' + (Get-Date -Format yyyyMMdd-HHmmss)) }
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
$credentialDirectory = Join-Path $root ('.local\auth-fixtures\admission-' + [Guid]::NewGuid().ToString('N'))
$instances = [Collections.Generic.List[object]]::new()
$cases = [Collections.Generic.List[object]]::new()
$keys = $null
$credentials = $null

function Quote-AuthArgument([string]$Value) { '"' + (($Value -replace '(\\*)"','$1$1\"') -replace '(\\+)$','$1$1') + '"' }
function Read-AuthLog([string]$Name) {
    $instance = $instances | Where-Object Name -EQ $Name | Select-Object -First 1
    if (-not $instance -or -not (Test-Path -LiteralPath $instance.Log)) { return '' }
    $reader = $null
    try {
        $stream = [IO.File]::Open($instance.Log,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader = New-Object IO.StreamReader($stream)
        return $reader.ReadToEnd()
    } finally { if ($reader) { $reader.Dispose() } }
}
function Wait-AuthEvidence([string]$Description, [scriptblock]$Condition) {
    if ($DryRun) { return }
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($instances[0].Process.HasExited) { throw 'Auth server exited before the test completed.' }
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 500
    }
    throw "Timed out waiting for $Description. Inspect $ReportDirectory."
}
function Start-AuthInstance([string]$Name, [string]$Role, [string]$TokenName) {
    $log = Join-Path $ReportDirectory ($Name + '.log')
    if ($Name -eq 'server') { $arguments = @($project,'/Game/Maps/MRFPSArena','-server',"-port=$Port") }
    else {
        $token = if ($TokenName -like 'missing-*' -or $DryRun) { Join-Path $credentialDirectory ($TokenName + '.jwt') } else { $credentials.tokens.$TokenName }
        # Enter through the same credential-validation path as the product connect UI.
        $arguments = @($project,'/Game/Maps/MRFPSArena','-game',"-MRFPSServer=127.0.0.1:$Port",'-MRFPSDesktop',"-MRFPSAuthTokenFile=$token","-MRFPSAuthPublicKeyFile=$($keys.public_key_path)",'-MRFPSAuthAudience=mrfps')
        if ($Role -eq 'inspector') { $arguments += '-MRFPSInspector' }
    }
    $arguments += @('-nullrhi','-nohmd','-nosound','-unattended','-NoSplash','-NoLiveCoding','-stdout','-FullStdOutLogOutput',"-abslog=$log")
    $line = ($arguments | ForEach-Object { Quote-AuthArgument $_ }) -join ' '
    Write-Host "$Name : $executable $line"
    $process = $null
    if (-not $DryRun) {
        $prior = @{}
        try {
            # Runtime clients need no SDK setup; avoid waiting on a concurrent packaging UBT mutex.
            $prior['UE_SKIP_UBT_SDK_SETUP'] = [Environment]::GetEnvironmentVariable('UE_SKIP_UBT_SDK_SETUP','Process')
            $env:UE_SKIP_UBT_SDK_SETUP = '1'
            if ($Name -eq 'server') {
                foreach ($variable in @('MRFPS_AUTH_PUBLIC_KEY_B64','MRFPS_AUTH_PUBLIC_KEY_PEM','MRFPS_AUTH_AUDIENCE')) { $prior[$variable] = [Environment]::GetEnvironmentVariable($variable,'Process') }
                $env:MRFPS_AUTH_PUBLIC_KEY_B64 = $keys.public_key_b64
                $env:MRFPS_AUTH_PUBLIC_KEY_PEM = $null
                $env:MRFPS_AUTH_AUDIENCE = 'mrfps'
            }
            $process = Start-Process -FilePath $executable -ArgumentList $line -WorkingDirectory $root -WindowStyle Hidden -PassThru
        } finally { foreach ($variable in $prior.Keys) { [Environment]::SetEnvironmentVariable($variable,$prior[$variable],'Process') } }
    }
    $instances.Add([PSCustomObject]@{ Name=$Name; Process=$process; Log=$log })
}

try {
    if (-not $DryRun) {
        foreach ($required in @('Content\Maps\MRFPSArena.umap','Binaries\Win64\UnrealEditor-MRFPS.dll','Plugins\MRFPSAuth\Binaries\Win64\UnrealEditor-MRFPSAuth.dll')) {
            if (-not (Test-Path -LiteralPath (Join-Path $root $required))) { throw 'Build the authenticated Editor and generate its arena before running auth admission tests.' }
        }
        $probe = New-Object Net.Sockets.UdpClient
        try { $probe.Client.Bind((New-Object Net.IPEndPoint([Net.IPAddress]::Any,$Port))) } finally { $probe.Dispose() }
    }
    $keys = New-MRFPSTestAuthKeys -Directory $credentialDirectory -DryRun:$DryRun
    Start-AuthInstance 'server' '' ''
    Wait-AuthEvidence 'the server world and public DTLS fingerprint' {
        $startup = Read-AuthLog 'server'
        ($startup -match 'MRFPS_SERVER_READY') -and ($startup -match 'MRFPS_AUTH_TRANSPORT_READY fingerprint=[0-9a-f]{64}')
    }
    if (-not $DryRun) {
        $fingerprint = [regex]::Match((Read-AuthLog 'server'),'MRFPS_AUTH_TRANSPORT_READY fingerprint=([0-9a-f]{64})').Groups[1].Value
        $credentials = Complete-MRFPSTestAuthTokens -Directory $credentialDirectory -Fingerprint $fingerprint -Inspectors 2
    }
    foreach ($role in @('player','inspector')) {
        Start-AuthInstance "valid-$role" $role "valid-$role"
        Wait-AuthEvidence "valid $role admission" { (Read-AuthLog 'server') -match "MRFPS_JOIN role=$role" }
    }
    $expected = @{ missing='AUTH_MISSING'; fake='AUTH_INVALID'; expired='AUTH_EXPIRED'; audience='AUTH_AUDIENCE'; role='AUTH_ROLE'; replay='AUTH_REPLAY'; transport='AUTH_TRANSPORT|DTLSCertVerifyCallback: Fingerprint validation failure' }
    foreach ($role in @('player','inspector')) {
        foreach ($case in @('missing','fake','expired','audience','role','replay','transport')) {
            $name = "$case-$role"
            $serverBefore = if ($DryRun) { '' } else { Read-AuthLog 'server' }
            $baseline = if ($DryRun) { 2 } else { [regex]::Matches($serverBefore,'MRFPS_JOIN role=').Count }
            Start-AuthInstance $name $role $name
            $code = $expected[$case]
            Wait-AuthEvidence "$name to reject with $code" { (Read-AuthLog $name) -match $code }
            if ($DryRun) { continue }
            Start-Sleep -Seconds 1
            $serverLog = Read-AuthLog 'server'
            $noJoin = [regex]::Matches($serverLog,'MRFPS_JOIN role=').Count -eq $baseline
            $serverDelta = $serverLog.Substring([Math]::Min($serverBefore.Length,$serverLog.Length))
            $serverObserved = $serverDelta -match $code
            $passed = $noJoin -and ((Read-AuthLog $name) -match $code) -and ($case -ne 'replay' -or $serverObserved)
            $nativePinRejected = (Read-AuthLog $name) -match 'DTLSCertVerifyCallback: Fingerprint validation failure'
            $jwtReachedServer = $serverDelta -match 'MRFPS_AUTH_REJECT code=AUTH_TRANSPORT|MRFPS_AUTH_ACCEPT[^\r\n]*subject=auth-transport-'
            if ($case -eq 'transport') { $passed = $passed -and $nativePinRejected -and -not $jwtReachedServer }
            $scope = if ($case -eq 'transport') { 'Trusted-issuer valid JWT with wrong signed DTLS pin: native TLS fingerprint rejection before admission; no claim that the server parsed its JWT' } elseif ($serverObserved) { 'Actual UE client denied; matching server rejection marker observed' } else { 'Actual UE client denied before admission; no server receipt of the invalid JWT is claimed' }
            $cases.Add([PSCustomObject]@{ Name=$name; Role=$role; ExpectedCode=$code; Passed=$passed; NoAdditionalServerJoin=$noJoin; ServerRejectionCodeObserved=$serverObserved; NativeTlsPinFailureObserved=$nativePinRejected; ServerJwtTransportRejectionObserved=$jwtReachedServer; Scope=$scope })
            $instance = $instances | Where-Object Name -EQ $name | Select-Object -First 1
            if (-not $instance.Process.HasExited) { $instance.Process.Kill(); [void]$instance.Process.WaitForExit(5000) }
        }
    }
    if ($DryRun) { return }
    $allLogs = ($instances | ForEach-Object { Read-AuthLog $_.Name }) -join "`n"
    $safe = Test-MRFPSCredentialPrivacy -Text $allLogs -TokenFiles @($credentials.tokens.PSObject.Properties | ForEach-Object Value)
    $noFatal = $allLogs -notmatch 'Fatal error:|Assertion failed:|Unhandled Exception:'
    $validAlive = @($instances | Where-Object { $_.Name -like 'valid-*' -and $_.Process.HasExited }).Count -eq 0
    $passed = $cases.Count -eq 14 -and @($cases | Where-Object Passed -EQ $false).Count -eq 0 -and $safe -and $noFatal -and $validAlive -and [regex]::Matches((Read-AuthLog 'server'),'MRFPS_JOIN role=').Count -eq 2
    $report = [ordered]@{ TimestampUtc=[DateTime]::UtcNow.ToString('o'); Passed=$passed; Scope='Actual UE client admission and replay; client-side and server-side rejection observations are distinguished'; ValidRoles=@('player','inspector'); Cases=@($cases.ToArray()); NoCredentialLeak=$safe; NoFatalError=$noFatal; ValidClientsRemainAlive=$validAlive; Processes=@($instances | ForEach-Object { [ordered]@{Name=$_.Name; Id=$_.Process.Id; Log=$_.Log} }) }
    $report | ConvertTo-Json -Depth 7 | Out-File -LiteralPath (Join-Path $ReportDirectory 'report.json') -Encoding utf8
    if (-not $passed) { throw "Auth admission checks failed. Evidence: $ReportDirectory" }
    Write-Host "Actual UE auth admission checks passed. Evidence: $ReportDirectory"
} catch {
    $_ | Out-String | Out-File -LiteralPath (Join-Path $ReportDirectory 'harness-error.txt') -Encoding utf8
    if (-not (Test-Path -LiteralPath (Join-Path $ReportDirectory 'report.json'))) {
        [ordered]@{ TimestampUtc=[DateTime]::UtcNow.ToString('o'); Passed=$false; Failure=$_.Exception.Message; Cases=@($cases.ToArray()); Scope='Incomplete auth admission run' } | ConvertTo-Json -Depth 6 | Out-File -LiteralPath (Join-Path $ReportDirectory 'report.json') -Encoding utf8
    }
    throw
} finally {
    foreach ($instance in $instances) { if ($instance.Process -and -not $instance.Process.HasExited) { $instance.Process.Kill(); [void]$instance.Process.WaitForExit(5000) } }
}
