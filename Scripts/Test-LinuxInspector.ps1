# Invoke through Invoke-MRFPSResourceLimited.ps1 (10 GiB free / 75% commit).
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$TokenFile,
    [Parameter(Mandatory)][string]$PublicKeyFile,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')][string]$ExpectedSubject,
    [Parameter(Mandatory)][ValidatePattern('^[a-f0-9]{64}$')][string]$ExpectedFingerprint,
    [string]$Server = '172.23.166.33', [int]$Port = 17779,
    [string]$Container = 'linux-mrfps-1', [string]$WslDistribution = 'Ubuntu',
    [Parameter(Mandatory)][string]$ReportDirectory,
    [int]$StartupTimeoutSeconds = 90
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$TokenFile = Resolve-MRFPSCredentialFile $TokenFile
$PublicKeyFile = Resolve-MRFPSCredentialFile $PublicKeyFile -Kind PublicKey
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$clientLog = Join-Path $ReportDirectory 'inspector.log'
$stdoutLog = Join-Path $ReportDirectory 'inspector.stdout.log'
$stderrLog = Join-Path $ReportDirectory 'inspector.stderr.log'
$client = $null
$checks = [ordered]@{}
$record = [ordered]@{ atUtc=[DateTime]::UtcNow.ToString('o'); passed=$false; scope='One Windows NullRHI inspector connected to an existing Linux game container; no fighter, rendered-view, or physical MR claim'; subject=$ExpectedSubject; endpoint="$($Server):$Port"; expectedFingerprint=$ExpectedFingerprint; clientId=$null; clientCleanedUp=$false; failure=$null; checks=$checks }
function Read-ProbeLog([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = New-Object IO.StreamReader($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function Read-ContainerState {
    $raw = & wsl -d $WslDistribution -u root --exec docker inspect $Container
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect the existing game container.' }
    return @($raw | ConvertFrom-Json)[0]
}
function Read-ContainerLog {
    $priorPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $lines = @(& wsl -d $WslDistribution -u root --exec docker logs $Container 2>&1 | ForEach-Object { $_.ToString() })
        if ($LASTEXITCODE -ne 0) { throw 'Cannot read game-container logs.' }
        return $lines -join "`n"
    } finally { $ErrorActionPreference = $priorPreference }
}
try {
    $initial = Read-ContainerState
    $beforeLog = Read-ContainerLog
    if (-not $initial.State.Running -or $initial.State.Health.Status -ne 'healthy' -or $beforeLog -notmatch [regex]::Escape("MRFPS_AUTH_TRANSPORT_READY fingerprint=$ExpectedFingerprint")) { throw 'The expected pinned server is not healthy.' }
    $record.initialContainer = [ordered]@{ id=$initial.Id; startedAt=$initial.State.StartedAt; health=$initial.State.Health.Status; restarts=$initial.RestartCount }
    $executable = Join-Path (Resolve-MRFPSEngine) 'Engine\Binaries\Win64\UnrealEditor.exe'
    $values = @((Join-Path $root 'MRFPS.uproject'),'/Game/Maps/MRFPSArena','-game',"-MRFPSServer=$($Server):$Port",'-MRFPSDesktop','-MRFPSInspector','-MRFPSSmoke',"-MRFPSAuthTokenFile=$TokenFile","-MRFPSAuthPublicKeyFile=$PublicKeyFile",'-MRFPSAuthAudience=mrfps','-nullrhi','-nohmd','-nosound','-unattended','-NoSplash','-NoLiveCoding','-stdout','-FullStdOutLogOutput','-ExecCmds=t.MaxFPS 30',"-abslog=$clientLog")
    $arguments = ($values | ForEach-Object { '"' + (($_ -replace '(\\*)"','$1$1\"') -replace '(\\+)$','$1$1') + '"' }) -join ' '
    $priorSdk = [Environment]::GetEnvironmentVariable('UE_SKIP_UBT_SDK_SETUP','Process')
    try {
        $env:UE_SKIP_UBT_SDK_SETUP = '1'
        $client = Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
        [void]$client.Handle
    } finally { [Environment]::SetEnvironmentVariable('UE_SKIP_UBT_SDK_SETUP',$priorSdk,'Process') }
    $record.clientId = $client.Id
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($client.HasExited) { throw "Inspector exited before replicated role evidence (exit $($client.ExitCode))." }
        if ((Read-ProbeLog $clientLog) -match 'MRFPS_SNAPSHOT players=[0-2] role=inspector') { break }
        Start-Sleep -Seconds 1
    }
    $clientText = Read-ProbeLog $clientLog
    if ($clientText -notmatch 'MRFPS_SNAPSHOT players=[0-2] role=inspector') { throw 'Inspector did not receive its replicated role before the timeout.' }
    Start-Sleep -Seconds 5
    if ($client.HasExited) { throw 'Inspector exited during the observation period.' }
    $serverText = Read-ContainerLog
    $clientText = Read-ProbeLog $clientLog
    $allText = $serverText + "`n" + $clientText + "`n" + (Read-ProbeLog $stdoutLog) + "`n" + (Read-ProbeLog $stderrLog)
    $final = Read-ContainerState
    $subject = [regex]::Escape($ExpectedSubject)
    $checks.ClientReplicatedInspectorRole = $clientText -match 'MRFPS_SNAPSHOT players=[0-2] role=inspector'
    $checks.ServerAcceptedFreshInspectorSubject = $serverText -match "MRFPS_AUTH_ACCEPT role=inspector subject=$subject expires="
    $checks.ServerJoinedFreshInspectorSubject = $serverText -match "MRFPS_JOIN role=inspector slot=-1 name=$subject(?:\r?\n|$)"
    $checks.PinnedTransportReady = $serverText -match [regex]::Escape("MRFPS_AUTH_TRANSPORT_READY fingerprint=$ExpectedFingerprint")
    $checks.ClientVerifiedNativeCertificate = $clientText -match 'DTLSCertVerifyCallback: Verified'
    $checks.ClientNativeHandshakeComplete = $clientText -match 'LogDTLSHandler: UpdateHandshake:\s+Handshaking completed'
    $checks.ServerNativeHandshakeComplete = $serverText -match 'LogDTLSHandler: UpdateHandshake:\s+Handshaking completed'
    $checks.NativeServerReady = $serverText -match 'MRFPS_SERVER_READY'
    $checks.NoFatalError = $allText -notmatch 'Fatal error:|Assertion failed:|Unhandled Exception:'
    $checks.NoCredentialLeak = Test-MRFPSCredentialPrivacy -Text $allText -TokenFiles @($TokenFile)
    $checks.SameHealthyContainer = $final.Id -eq $initial.Id -and $final.State.StartedAt -eq $initial.State.StartedAt -and $final.State.Running -and $final.State.Health.Status -eq 'healthy' -and $final.RestartCount -eq $initial.RestartCount -and -not $final.State.OOMKilled
    $checks.RuntimeResourceCaps = $final.HostConfig.Memory -eq 1073741824 -and $final.HostConfig.MemorySwap -eq 1073741824 -and $final.HostConfig.NanoCpus -eq 2000000000
    $record.finalContainer = [ordered]@{ id=$final.Id; startedAt=$final.State.StartedAt; health=$final.State.Health.Status; restarts=$final.RestartCount; oomKilled=$final.State.OOMKilled }
    $record.tlsEvidence = 'Actual native certificate verification and handshake-completion logs, plus fresh subject acceptance and replicated inspector state. This probe does not capture network packets.'
    $record.nativeTlsLogMarkers = @(([regex]::Matches($allText,'[^\r\n]*(?:DTLS[^\r\n]*(?:handshake|Handshake)|(?:handshake|Handshake)[^\r\n]*DTLS)[^\r\n]*') | ForEach-Object Value) | Select-Object -Unique)
    if ($checks.NoCredentialLeak) { [IO.File]::WriteAllText((Join-Path $ReportDirectory 'server.log'),$serverText) }
    $record.passed = @($checks.Values | Where-Object { $_ -ne $true }).Count -eq 0
} catch {
    $record.failure = $_.Exception.Message
    $record.passed = $false
} finally {
    if ($client) {
        if (-not $client.HasExited) { $client.Kill(); [void]$client.WaitForExit(5000) }
        $record.clientCleanedUp = $client.HasExited
        $client.Dispose()
    }
    $record.finishedAtUtc = [DateTime]::UtcNow.ToString('o')
    $record | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $ReportDirectory 'report.json') -Encoding UTF8
}
if (-not $record.passed) { throw "Linux inspector probe failed; inspect $ReportDirectory/report.json." }
Write-Host "Linux inspector probe passed; evidence: $ReportDirectory"
