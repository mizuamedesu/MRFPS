# Run through Invoke-MRFPSResourceLimited.ps1 with MinimumFreeGB=10 and
# MaximumCommitPercent=75. This script connects only to an EXISTING server.
# It neither launches a server/player nor captures or controls any window.
# Each run needs a fresh directory of Count distinct inspector *.jwt files.
# Create ReportDirectory\STOP (or -StopFile) to end the live review safely.
[CmdletBinding()]
param(
    [string]$Server = '127.0.0.1',
    [ValidateRange(1,65535)][int]$Port = 7777,
    [Parameter(Mandatory)][string]$PublicKeyFile,
    [Parameter(Mandatory)][string]$TokenDirectory,
    [ValidateRange(1,3)][int]$Count = 1,
    [ValidateSet('inspector','admin')][string]$Role = 'inspector',
    [Parameter(Mandatory)][string]$ReportDirectory,
    [ValidateRange(1,3600)][int]$MaximumSeconds = 600,
    [string]$EngineRoot,
    [string]$Executable,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}$')][string]$Audience = 'mrfps',
    [string]$StopFile,
    [switch]$DryRun
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$utf8 = [Text.UTF8Encoding]::new($false)
$instances = [Collections.Generic.List[object]]::new()
$ownedProcesses = @{}
$lifecycle = [PSCustomObject]@{ Failure=$null; StopReason=$null; CleanupComplete=$false; PrivacyOk=$true; Finished=$false; ReportWriteDeferrals=0; FailureType=$null; FailureHResult=$null; FailureLine=$null }
$startedAt = [DateTime]::UtcNow
$clock = [Diagnostics.Stopwatch]::StartNew()

function Quote-LiveArgument([string]$Value) {
    '"' + (($Value -replace '(\\*)"','$1$1\"') -replace '(\\+)$','$1$1') + '"'
}
function Test-LiveCreationMatch([DateTime]$Left, [DateTime]$Right) {
    # CIM exposes microseconds while Process.StartTime retains 100 ns ticks.
    # Compare the same microsecond, never a broad seconds-based PID tolerance.
    ($Left.Ticks-($Left.Ticks % 10)) -eq ($Right.Ticks-($Right.Ticks % 10))
}
function Write-LiveDurableJson([string]$Path, [object]$Value) {
    $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $bytes = $utf8.GetBytes(($Value | ConvertTo-Json -Depth 10))
    $stream = [IO.FileStream]::new($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None,4096,[IO.FileOptions]::WriteThrough)
    try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
    try {
        # Ordinary report readers can briefly omit FILE_SHARE_DELETE. Their
        # handle must not terminate the game clients. Keep the old complete
        # report on a sustained sharing lock and retry on the next poll.
        # The temporary data was already flushed before the atomic rename.
        for ($attempt=0; $attempt -lt 10; $attempt++) {
            try {
                if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($temporary,$Path,[NullString]::Value) }
                else { [IO.File]::Move($temporary,$Path) }
                return
            } catch {
                $cause=$_.Exception.GetBaseException()
                $nativeCode=$cause.HResult -band 0xffff
                if ($cause -isnot [IO.IOException] -or $nativeCode -notin @(32,33)) { throw }
                if ($attempt -eq 9) { $lifecycle.ReportWriteDeferrals++; return }
                Start-Sleep -Milliseconds 25
            }
        }
    } finally { if (Test-Path -LiteralPath $temporary) { [IO.File]::Delete($temporary) } }
}
function Get-MRFPSCredentialPrivacy([string]$Text) {
    # The existing module exports Test-MRFPSCredentialPrivacy. Keep all scans
    # on that common implementation; never include offending text in a report.
    Test-MRFPSCredentialPrivacy -Text $Text -TokenFiles $tokenFiles
}
function Get-LiveTokenPreflight([string]$Path) {
    # This is an UNTRUSTED scheduling/role preflight, not signature validation.
    # The native client still verifies issuer key, DTLS pin, audience and time;
    # the remote server still owns single-use admission and expiry enforcement.
    $raw = $null; $parts = $null; $payload = $null
    try {
        $raw = [IO.File]::ReadAllText($Path).Trim()
        $parts = $raw.Split('.')
        $segment = $parts[1].Replace('-','+').Replace('_','/')
        $segment += '=' * ((4 - $segment.Length % 4) % 4)
        $payload = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($segment)) | ConvertFrom-Json
        foreach ($field in @('role','aud','iat','nbf','exp','jti')) {
            if (-not $payload.PSObject.Properties[$field]) { throw 'AUTH_PREFLIGHT_INVALID' }
        }
        if ($payload.role -cne $Role) { throw 'AUTH_PREFLIGHT_ROLE' }
        if ($payload.aud -cne $Audience) { throw 'AUTH_PREFLIGHT_AUDIENCE' }
        foreach ($field in @('iat','nbf','exp')) {
            if ($payload.$field -isnot [long] -and $payload.$field -isnot [int]) { throw 'AUTH_PREFLIGHT_INVALID' }
        }
        if ($payload.jti -cnotmatch '^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$') { throw 'AUTH_PREFLIGHT_INVALID' }
        $expires = [DateTimeOffset]::FromUnixTimeSeconds([long]$payload.exp).UtcDateTime
        $notBefore = [DateTimeOffset]::FromUnixTimeSeconds([long]$payload.nbf).UtcDateTime
        $issued = [DateTimeOffset]::FromUnixTimeSeconds([long]$payload.iat).UtcDateTime
        $fresh = $expires -gt [DateTime]::UtcNow.AddSeconds(5) -and $notBefore -le [DateTime]::UtcNow.AddSeconds(5)
        if (-not $DryRun -and -not $fresh) { throw 'AUTH_PREFLIGHT_TIME' }
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $digest = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($raw))).Replace('-','') }
        finally { $sha.Dispose() }
        [PSCustomObject]@{ Digest=$digest; TokenId=$payload.jti; Role=$Role; IssuedAtUtc=$issued.ToString('o'); ExpiresAtUtc=$expires.ToString('o'); FreshAtPreparation=$fresh }
    } catch {
        $code = $_.Exception.Message
        if ($code -notmatch '^AUTH_PREFLIGHT_(?:INVALID|ROLE|AUDIENCE|TIME)$') { $code = 'AUTH_PREFLIGHT_INVALID' }
        throw $code
    } finally { $raw=$null; $parts=$null; $payload=$null; $segment=$null }
}
function New-LiveCursor([string]$Path) {
    [PSCustomObject]@{ Path=$Path; Offset=[long]0; BytesChecked=[long]0; Tail=''; Remainder=''; CaughtUp=$false; Existed=$false; Buffer=[byte[]]::new(65536) }
}
function Update-LiveEvidence([object]$Instance, [string]$Text) {
    # Store fixed marker names and typed metadata, never arbitrary log lines.
    $stampPattern='\[(?<time>\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\]\[\s*\d+\]'
    $style=[Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
    foreach ($event in [regex]::Matches($Text,('(?m)^'+$stampPattern+'[^\r\n]*(?<marker>DTLSCertVerifyCallback: Verified|LogDTLSHandler: UpdateHandshake:\s+Handshaking completed)'))) {
        $at=[DateTime]::ParseExact($event.Groups['time'].Value,'yyyy.MM.dd-HH.mm.ss:fff',[Globalization.CultureInfo]::InvariantCulture,$style)
        if ($at -lt $Instance.LaunchedAt.AddSeconds(-1)) { continue }
        if ($event.Groups['marker'].Value -eq 'DTLSCertVerifyCallback: Verified') {
            $Instance.CertificateVerified=$true
            if (-not $Instance.CertificateAt -or $at -lt $Instance.CertificateAt) { $Instance.CertificateAt=$at }
        } else {
            $Instance.HandshakeComplete=$true
            if (-not $Instance.HandshakeAt -or $at -lt $Instance.HandshakeAt) { $Instance.HandshakeAt=$at }
        }
    }
    if ($Text -match 'LogShaderCompilers:|LogShaderPipelineCache:') { $Instance.ShaderWarmupObserved=$true }
    if ($Text -match 'LogD3D12RHI:|LogD3D11RHI:|LogVulkanRHI:') { $Instance.RendererLogObserved=$true }
    if ($Text -match 'Fatal error:|Assertion failed:|Unhandled Exception:|Ensure condition failed:') { $Instance.Failure='RUNTIME_FAILURE' }
    if ($Text -match 'MRFPS_NETWORK_FAILURE') { $Instance.Failure='NETWORK_FAILURE' }
    if ($Text -match 'MRFPS_AUTH_STARTUP_FAILED|MRFPS_AUTH_CLIENT_REJECT|MRFPS_AUTH_REJECT') {
        $Instance.Failure='AUTH_REJECTED'
        $safe = [regex]::Match($Text,'MRFPS_AUTH_(?:STARTUP_FAILED|CLIENT_REJECT|REJECT)[^\r\n]{0,80}\bcode=(AUTH_[A-Z_]{1,48})\b')
        if ($safe.Success) { $Instance.AuthSafeCode=$safe.Groups[1].Value }
    }
    $pattern = '(?m)^\[(?<time>\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\]\[\s*(?<frame>\d+)\]LogMRFPS: Display: MRFPS_SNAPSHOT players=(?<players>[0-2]) role=(?<role>inspector|player)\b'
    foreach ($match in [regex]::Matches($Text,$pattern)) {
        $timestamp = [DateTime]::ParseExact($match.Groups['time'].Value,'yyyy.MM.dd-HH.mm.ss:fff',[Globalization.CultureInfo]::InvariantCulture,$style)
        if ($timestamp -lt $Instance.LaunchedAt.AddSeconds(-1)) { continue }
        # The startup arena is a local standalone world with role=player while
        # the native certificate activates. It is not remote admission evidence.
        # Timestamp fences also reject delayed stdout copies read after the
        # client.log has already reached its authenticated inspector world.
        if (-not $Instance.CertificateAt -or -not $Instance.HandshakeAt -or
            $timestamp -lt $Instance.CertificateAt -or $timestamp -lt $Instance.HandshakeAt) { continue }
        if ($Instance.Snapshot -and $timestamp -le $Instance.Snapshot.AtUtc) { continue }
        # Even just after handshake the old standalone world can still tick
        # before network travel commits. Wait for an actual inspector snapshot;
        # a subsequent new player snapshot is a real role regression.
        if (-not $Instance.Snapshot -and $match.Groups['role'].Value -ne 'inspector') { continue }
        $Instance.Snapshot=[PSCustomObject]@{ AtUtc=$timestamp; Frame=[long]$match.Groups['frame'].Value; Players=[int]$match.Groups['players'].Value; Role=$match.Groups['role'].Value }
        $Instance.SnapshotCount++
        if ($Instance.Snapshot.Role -ne 'inspector') { $Instance.Failure='ROLE_MISMATCH' }
    }
}
function Read-LiveLogDelta([object]$Instance, [object]$Cursor) {
    if (-not (Test-Path -LiteralPath $Cursor.Path -PathType Leaf)) { return }
    $stream = $null
    try {
        $stream=[IO.File]::Open($Cursor.Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $Cursor.Existed=$true
        if ($stream.Length -lt $Cursor.Offset) { $Instance.Failure='LOG_TRUNCATED'; return }
        [void]$stream.Seek($Cursor.Offset,[IO.SeekOrigin]::Begin)
        # At most 512 KiB per file per poll; no whole-log loading during warmup.
        $budget=524288
        while ($budget -gt 0) {
            $read=$stream.Read($Cursor.Buffer,0,[Math]::Min($budget,$Cursor.Buffer.Length))
            if ($read -eq 0) { break }
            $Cursor.Offset+=$read; $Cursor.BytesChecked+=$read; $budget-=$read
            $text=$utf8.GetString($Cursor.Buffer,0,$read)
            $privacyText=$Cursor.Tail+$text
            if (-not (Get-MRFPSCredentialPrivacy $privacyText)) { $Instance.Failure='CREDENTIAL_LEAK'; $lifecycle.PrivacyOk=$false; $lifecycle.Failure='CREDENTIAL_LEAK' }
            if ($text.IndexOf([char]0) -ge 0) { $Instance.Failure='LOG_NUL_DATA' }
            $Cursor.Tail=$privacyText.Substring([Math]::Max(0,$privacyText.Length-4096))
            $lines=$Cursor.Remainder+$text
            $end=$lines.LastIndexOf("`n")
            if ($end -ge 0) {
                Update-LiveEvidence $Instance ($lines.Substring(0,$end+1))
                $Cursor.Remainder=$lines.Substring($end+1)
            } else { $Cursor.Remainder=$lines }
            if ($Cursor.Remainder.Length -gt 8192) { $Cursor.Remainder=$Cursor.Remainder.Substring($Cursor.Remainder.Length-8192) }
        }
        $Cursor.CaughtUp=$Cursor.Offset -eq $stream.Length
    } catch { $Instance.Failure='LOG_READ_FAILED' }
    finally { if ($stream) { $stream.Dispose() } }
}
function Get-LivePhase([object]$Instance) {
    if ($Instance.Failure) { return 'failed' }
    if (-not $Instance.Process) { return 'prepared_not_started' }
    if ($Instance.Process.HasExited) { return 'exited' }
    if (-not $Instance.HandshakeComplete) {
        if ($Instance.CertificateVerified) { return 'native_dtls_handshaking' }
        if ($Instance.ShaderWarmupObserved) { return 'initializing_shader_warmup_or_connecting' }
        return 'initializing_or_waiting_for_native_dtls'
    }
    if (-not $Instance.CertificateVerified) { return 'waiting_for_certificate_evidence' }
    if (-not $Instance.Snapshot) { return 'waiting_for_inspector_snapshot' }
    $age=([DateTime]::UtcNow-$Instance.Snapshot.AtUtc).TotalSeconds
    if ($age -lt -1 -or $age -gt 3) { return 'replication_stale' }
    if ($Instance.Snapshot.Role -ne 'inspector') { return 'unexpected_role' }
    if ($Instance.Snapshot.Players -lt 1) { return 'inspector_connected_waiting_for_player' }
    return 'network_ready_visual_review_pending'
}
function Get-LiveInstanceMetadata([object]$Instance) {
    $processAlive=$false; $exitCode=$null
    if ($Instance.Process) {
        $processAlive=-not $Instance.Process.HasExited
        if (-not $processAlive) { $exitCode=$Instance.Process.ExitCode }
    }
    $snapshot=$null
    if ($Instance.Snapshot) {
        $snapshot=[ordered]@{ AtUtc=$Instance.Snapshot.AtUtc.ToString('o'); AgeSeconds=[Math]::Round(([DateTime]::UtcNow-$Instance.Snapshot.AtUtc).TotalSeconds,3); Frame=$Instance.Snapshot.Frame; Players=$Instance.Snapshot.Players; Role=$Instance.Snapshot.Role }
    }
    [ordered]@{
        Name=$Instance.Name; Phase=(Get-LivePhase $Instance); ProcessId=$Instance.ProcessId; ProcessCreationTimeUtc=$Instance.CreationTimeUtc
        LaunchRequestedAtUtc=$Instance.LaunchedAt.ToString('o'); Alive=$processAlive; ExitCode=$exitCode
        Log=$Instance.Log; StdoutLog=$Instance.Stdout; StderrLog=$Instance.Stderr
        TokenFile=$Instance.TokenFile; PublicKeyFile=$PublicKeyFile; UntrustedTokenPreflight=[ordered]@{ Role=$Instance.Preflight.Role; IssuedAtUtc=$Instance.Preflight.IssuedAtUtc; ExpiresAtUtc=$Instance.Preflight.ExpiresAtUtc; FreshAtPreparation=$Instance.Preflight.FreshAtPreparation; SignatureVerifiedByThisScript=$false; NeverPreviouslyConsumedIsNotProven=$true }
        CertificateVerified=$Instance.CertificateVerified; NativeDtlsHandshakeComplete=$Instance.HandshakeComplete
        CertificateVerifiedAtUtc=$(if ($Instance.CertificateAt) { $Instance.CertificateAt.ToString('o') } else { $null }); NativeHandshakeCompleteAtUtc=$(if ($Instance.HandshakeAt) { $Instance.HandshakeAt.ToString('o') } else { $null })
        ShaderWarmupObserved=$Instance.ShaderWarmupObserved; RendererLogObserved=$Instance.RendererLogObserved
        SnapshotCount=$Instance.SnapshotCount; LatestSnapshot=$snapshot; AuthSafeCode=$Instance.AuthSafeCode; Failure=$Instance.Failure
        LogPrivacyCaughtUp=(@($Instance.Cursors | Where-Object { $_.Existed -and -not $_.CaughtUp }).Count -eq 0)
        LogBytesChecked=[long](($Instance.Cursors | Measure-Object BytesChecked -Sum).Sum)
        RealVideoVerified=$false; QuestPassthroughVerified=$false; WindowsVisibleFrameVerified=$false; VisualReview='pending_external_review'
    }
}
function Write-LiveReport([string]$Status) {
    $metadata=@($instances | ForEach-Object { Get-LiveInstanceMetadata $_ })
    foreach ($instance in $instances) { Write-LiveDurableJson $instance.Metadata (Get-LiveInstanceMetadata $instance) }
    $report=[ordered]@{
        SchemaVersion=1; RunId=$runId; StartedAtUtc=$startedAt.ToString('o'); UpdatedAtUtc=[DateTime]::UtcNow.ToString('o')
        Finished=$lifecycle.Finished; Status=$Status; Passed=$false; DryRun=[bool]$DryRun
        Scope='Visible Windows inspectors connected to the supplied existing server. Actual Quest passthrough frames and screen visibility require independent visual evidence.'
        Endpoint=$endpoint; Count=$Count; RequestedSignedRole=$Role; LaunchMode=$launchMode; Executable=$Executable; Resolution='1280x720'; MaximumFramesPerSecond=90
        MaximumSeconds=$MaximumSeconds; ElapsedSeconds=[Math]::Round($clock.Elapsed.TotalSeconds,3); StopFile=$StopFile
        CallerResourceGuardRequired=[ordered]@{ MinimumFreeGB=10; MaximumCommitPercent=75 }; StopReason=$lifecycle.StopReason; Failure=$lifecycle.Failure
        ReportWriteDeferrals=$lifecycle.ReportWriteDeferrals
        FailureType=$lifecycle.FailureType; FailureHResult=$lifecycle.FailureHResult; FailureLine=$lifecycle.FailureLine
        NoCredentialLeakObserved=$lifecycle.PrivacyOk; AllExistingLogBytesPrivacyChecked=(@($instances.Cursors | Where-Object { $_.Existed -and -not $_.CaughtUp }).Count -eq 0)
        OwnProcessCleanupComplete=$lifecycle.CleanupComplete; CleanupScope='Only launched process handles and observed descendants with matching PID/creation time; caller resource guard owns the enclosing job.'
        NetworkReadyForReview=($metadata.Count -eq $Count -and @($metadata | Where-Object Phase -NE 'network_ready_visual_review_pending').Count -eq 0)
        RealVideoVerified=$false; QuestPassthroughVerified=$false; WindowsVisibleFrameVerified=$false; VisualReview='pending_external_review'
        NoSignalOrTargetRoleIsVideoProof=$false; ServerIdentityAndQuestSourceRequireExternalEvidence=$true
        Instances=$metadata
        OwnedProcesses=@($ownedProcesses.Values | Sort-Object Depth,Id | ForEach-Object { [ordered]@{ ProcessId=$_.Id; ParentProcessId=$_.ParentId; CreationTimeUtc=$_.Created.ToString('o'); Depth=$_.Depth; Name=$_.Name; StillAlive=$_.StillAlive } })
    }
    Write-LiveDurableJson (Join-Path $ReportDirectory 'report.json') $report
}
function Update-LiveOwnedProcesses {
    # Never query or save CommandLine. A descendant is adopted only while its
    # observed parent PID still has the recorded creation time (PID reuse guard).
    $rows=@(Get-CimInstance Win32_Process -Property ProcessId,ParentProcessId,CreationDate,Name)
    $byId=@{}
    foreach ($row in $rows) { $byId[[int]$row.ProcessId]=$row }
    foreach ($owned in @($ownedProcesses.Values)) {
        $row=$byId[$owned.Id]
        $owned.StillAlive=$null -ne $row -and (Test-LiveCreationMatch ([DateTime]$row.CreationDate).ToUniversalTime() $owned.Created)
    }
    $added=$true
    while ($added) {
        $added=$false
        foreach ($row in $rows) {
            $childId=[int]$row.ProcessId; $parentId=[int]$row.ParentProcessId
            if ($ownedProcesses.ContainsKey($childId) -or -not $ownedProcesses.ContainsKey($parentId)) { continue }
            $parent=$ownedProcesses[$parentId]
            $created=([DateTime]$row.CreationDate).ToUniversalTime()
            if (-not $parent.StillAlive -or $created -lt $parent.Created) { continue }
            $ownedProcesses[$childId]=[PSCustomObject]@{ Id=$childId; ParentId=$parentId; Created=$created; Depth=($parent.Depth+1); Name=$row.Name; StillAlive=$true; Handle=$null }
            $added=$true
        }
    }
}
function Stop-LiveOwnedProcesses {
    # The resource guard supplies the outer kill-on-close job, including any
    # extremely short-lived child missed between process-tree samples.
    try { Update-LiveOwnedProcesses } catch { $lifecycle.Failure='PROCESS_ENUMERATION_FAILED' }
    foreach ($owned in @($ownedProcesses.Values | Sort-Object Depth -Descending)) {
        $process=$null
        try {
            if ($owned.Handle) { $process=$owned.Handle }
            else { $process=Get-Process -Id $owned.Id -ErrorAction SilentlyContinue }
            if (-not $process -or $process.HasExited) { $owned.StillAlive=$false; continue }
            if (-not (Test-LiveCreationMatch $process.StartTime.ToUniversalTime() $owned.Created)) { $owned.StillAlive=$false; continue }
            # Do not automate UI: terminating the recorded process is cleanup.
            $process.Kill()
            [void]$process.WaitForExit(5000)
            $owned.StillAlive=-not $process.HasExited
        } catch { $owned.StillAlive=$true; $lifecycle.Failure='OWN_PROCESS_CLEANUP_FAILED' }
        finally { if ($process -and -not $owned.Handle) { $process.Dispose() } }
    }
    $lifecycle.CleanupComplete=@($ownedProcesses.Values | Where-Object StillAlive).Count -eq 0
}

if ($env:OS -ne 'Windows_NT') { throw 'LIVE_INSPECTOR_REQUIRES_WINDOWS' }
if ($Server.Length -gt 253 -or $Server -notmatch '^[A-Za-z0-9.:[\]-]+$') { throw 'LIVE_INSPECTOR_INVALID_SERVER' }
if ($Server.Contains(':') -and -not $Server.StartsWith('[')) { $Server='['+$Server+']' }
$endpoint="$($Server):$Port"
$PublicKeyFile=Resolve-MRFPSCredentialFile $PublicKeyFile -Kind PublicKey
$TokenDirectory=[IO.Path]::GetFullPath($TokenDirectory)
if (-not (Test-Path -LiteralPath $TokenDirectory -PathType Container)) { throw 'AUTH_TOKEN_DIRECTORY_MISSING' }
$tokenFiles=@(Get-ChildItem -LiteralPath $TokenDirectory -Filter '*.jwt' -File | Sort-Object Name | ForEach-Object { Resolve-MRFPSCredentialFile $_.FullName })
if ($tokenFiles.Count -ne $Count) { throw 'AUTH_EXACTLY_ONE_INSPECTOR_TOKEN_PER_INSTANCE_REQUIRED' }
$preflights=@($tokenFiles | ForEach-Object { Get-LiveTokenPreflight $_ })
if (@($preflights.Digest | Select-Object -Unique).Count -ne $Count -or @($preflights.TokenId | Select-Object -Unique).Count -ne $Count) { throw 'AUTH_DISTINCT_ONE_CONNECTION_TOKENS_REQUIRED' }
$ReportDirectory=[IO.Path]::GetFullPath($ReportDirectory)
if (-not $StopFile) { $StopFile=Join-Path $ReportDirectory 'STOP' }
$StopFile=[IO.Path]::GetFullPath($StopFile)
if ((Test-Path -LiteralPath (Join-Path $ReportDirectory 'report.json')) -or (Test-Path -LiteralPath $StopFile)) { throw 'LIVE_INSPECTOR_FRESH_REPORT_DIRECTORY_REQUIRED' }
if ($Executable) {
    $Executable=[IO.Path]::GetFullPath($Executable); $launchMode='PackagedWindows'
} else {
    $Executable=Join-Path (Resolve-MRFPSEngine $EngineRoot) 'Engine\Binaries\Win64\UnrealEditor.exe'; $launchMode='EditorGame'
}
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw 'LIVE_INSPECTOR_EXECUTABLE_MISSING' }
$runId=[Guid]::NewGuid().ToString('N')
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
for ($index=0; $index -lt $Count; $index++) {
    $name='inspector-'+$index
    $directory=Join-Path $ReportDirectory $name
    if (Test-Path -LiteralPath $directory) { throw 'LIVE_INSPECTOR_INSTANCE_DIRECTORY_ALREADY_EXISTS' }
    New-Item -ItemType Directory -Path $directory | Out-Null
    $log=Join-Path $directory 'client.log'; $stdout=Join-Path $directory 'stdout.log'; $stderr=Join-Path $directory 'stderr.log'
    $instances.Add([PSCustomObject]@{
        Name=$name; TokenFile=$tokenFiles[$index]; Preflight=$preflights[$index]; Log=$log; Stdout=$stdout; Stderr=$stderr; Metadata=(Join-Path $directory 'metadata.json')
        Cursors=@((New-LiveCursor $log),(New-LiveCursor $stdout),(New-LiveCursor $stderr)); Process=$null; ProcessId=$null; CreationTimeUtc=$null; LaunchedAt=$startedAt
        Snapshot=$null; SnapshotCount=0; CertificateVerified=$false; HandshakeComplete=$false; CertificateAt=$null; HandshakeAt=$null; ShaderWarmupObserved=$false; RendererLogObserved=$false; Failure=$null; AuthSafeCode=$null
    })
}
try {
    Write-LiveReport 'prepared'
    if ($DryRun) {
        $lifecycle.StopReason='dry_run_no_process_started'; $lifecycle.CleanupComplete=$true
    } else {
        # Avoid leaking server-only env into clients. Restore caller values even
        # if process creation fails; no persistent/global environment changes.
        $environmentNames=@('UE_SKIP_UBT_SDK_SETUP','MSBUILDDISABLENODEREUSE','MRFPS_AUTH_PUBLIC_KEY_B64','MRFPS_AUTH_PUBLIC_KEY_PEM','MRFPS_AUTH_AUDIENCE')
        foreach ($instance in $instances) {
            if (Test-Path -LiteralPath $StopFile) { $lifecycle.StopReason='stop_file'; break }
            if ($clock.Elapsed.TotalSeconds -ge $MaximumSeconds) { $lifecycle.StopReason='deadline'; break }
            $values=@('/Game/Maps/MRFPSArena',"-MRFPSServer=$endpoint",'-MRFPSInspector','-MRFPSDesktop','-MRFPSSmoke','-nohmd','-nosound','-NoSplash','-NoLiveCoding','-windowed','-ResX=1280','-ResY=720',('-WinX='+ (40+80*($instances.IndexOf($instance)))),('-WinY='+ (40+60*($instances.IndexOf($instance)))),'-ExecCmds=t.MaxFPS 90,t.IdleWhenNotForeground 0',"-MRFPSAuthTokenFile=$($instance.TokenFile)","-MRFPSAuthPublicKeyFile=$PublicKeyFile","-MRFPSAuthAudience=$Audience",'-stdout','-FullStdOutLogOutput',"-abslog=$($instance.Log)")
            if ($launchMode -eq 'EditorGame') { $values=@((Join-Path $root 'MRFPS.uproject'),'-game')+$values }
            if ($Role -eq 'admin') { $values=@($values|Where-Object{$_ -cne '-MRFPSInspector'})+@('-MRFPSAdmin') }
            $arguments=($values | ForEach-Object { Quote-LiveArgument $_ }) -join ' '
            $prior=@{}
            foreach ($name in $environmentNames) { $prior[$name]=[Environment]::GetEnvironmentVariable($name,'Process') }
            try {
                [Environment]::SetEnvironmentVariable('UE_SKIP_UBT_SDK_SETUP','1','Process')
                [Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE','1','Process')
                foreach ($name in $environmentNames | Where-Object { $_ -notin @('UE_SKIP_UBT_SDK_SETUP','MSBUILDDISABLENODEREUSE') }) { [Environment]::SetEnvironmentVariable($name,$null,'Process') }
                $instance.LaunchedAt=[DateTime]::UtcNow
                $instance.Process=Start-Process -FilePath $Executable -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Normal -PassThru -RedirectStandardOutput $instance.Stdout -RedirectStandardError $instance.Stderr
                [void]$instance.Process.Handle
                $created=$instance.Process.StartTime.ToUniversalTime()
                $instance.ProcessId=$instance.Process.Id; $instance.CreationTimeUtc=$created.ToString('o')
                $ownedProcesses[$instance.Process.Id]=[PSCustomObject]@{ Id=$instance.Process.Id; ParentId=$PID; Created=$created; Depth=0; Name=$instance.Process.ProcessName; StillAlive=$true; Handle=$instance.Process }
            } finally { foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name,$prior[$name],'Process') } }
            Update-LiveOwnedProcesses
            Write-LiveReport 'live'
        }
        while (-not $lifecycle.StopReason) {
            if (Test-Path -LiteralPath $StopFile) { $lifecycle.StopReason='stop_file'; break }
            if ($clock.Elapsed.TotalSeconds -ge $MaximumSeconds) { $lifecycle.StopReason='deadline'; break }
            Update-LiveOwnedProcesses
            foreach ($instance in $instances) {
                foreach ($cursor in $instance.Cursors) { Read-LiveLogDelta $instance $cursor }
                if ($instance.Process -and $instance.Process.HasExited -and -not $instance.Failure) { $instance.Failure='INSTANCE_EXITED' }
            }
            if (@($instances | Where-Object Failure).Count -gt 0) { $lifecycle.StopReason='instance_failure'; $lifecycle.Failure='INSTANCE_FAILURE' }
            Write-LiveReport 'live'
            if (-not $lifecycle.StopReason) { Start-Sleep -Milliseconds 1000 }
        }
    }
} catch {
    # Exception text may contain file/log/JSON content; expose only this code.
    $lifecycle.Failure='LIVE_INSPECTOR_OPERATION_FAILED'; $lifecycle.StopReason='operation_failed'
    $cause=$_.Exception.GetBaseException()
    $lifecycle.FailureType=$cause.GetType().FullName
    $lifecycle.FailureHResult=$cause.HResult
    $lifecycle.FailureLine=$_.InvocationInfo.ScriptLineNumber
} finally {
    if (-not $DryRun) { Stop-LiveOwnedProcesses }
    # Drain finite stopped logs with bounded memory. A backlogged/unreadable log
    # remains explicitly unchecked rather than producing a privacy-success claim.
    $auditClock=[Diagnostics.Stopwatch]::StartNew()
    do {
        foreach ($instance in $instances) {
            foreach ($cursor in $instance.Cursors) { Read-LiveLogDelta $instance $cursor }
        }
        $backlog=@($instances.Cursors | Where-Object { $_.Existed -and -not $_.CaughtUp }).Count -gt 0
    } while ($backlog -and $auditClock.Elapsed.TotalSeconds -lt 10)
    if ($backlog) { $lifecycle.Failure='LOG_PRIVACY_SCAN_INCOMPLETE' }
    if (@($instances | Where-Object Failure).Count -gt 0 -and -not $lifecycle.Failure) { $lifecycle.Failure='INSTANCE_FAILURE' }
    $lifecycle.Finished=$true
    $finalStatus=if ($DryRun) { 'prepared_dry_run_only' } elseif ($lifecycle.Failure -or -not $lifecycle.CleanupComplete) { 'stopped_with_failure' } else { 'stopped_visual_review_pending' }
    Write-LiveReport $finalStatus
    foreach ($instance in $instances) { if ($instance.Process) { $instance.Process.Dispose() } }
}
Write-Host "Live inspector validation $finalStatus. Review the safe report in $ReportDirectory. Passed remains false pending separate visual evidence."
if ($lifecycle.Failure -or -not $lifecycle.CleanupComplete) { throw 'LIVE_INSPECTOR_VALIDATION_FAILED_SEE_SAFE_REPORT' }
