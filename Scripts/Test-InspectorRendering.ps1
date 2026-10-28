[CmdletBinding()]
param(
    [string]$EngineRoot,
    [ValidateRange(1,65535)][int]$Port = 17890,
    [ValidateRange(15,1800)][int]$StartupTimeoutSeconds = 1800,
    [string]$ReportDirectory,
    [switch]$KeepProcesses,
    [switch]$VisibleInspector,
    [ValidateRange(0,300)][int]$ReviewHoldSeconds = 0,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$engine = Resolve-MRFPSEngine $EngineRoot
$executable = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor.exe'
$project = Join-Path $root 'MRFPS.uproject'
if (-not $ReportDirectory) { $ReportDirectory = Join-Path $root ('Saved\Validation\InspectorRendering-' + (Get-Date -Format yyyyMMdd-HHmmss)) }
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
$runId = [Guid]::NewGuid().ToString('N')
$credentialDirectory = Join-Path $root ('.local\auth-fixtures\rendering-' + $runId)
# ExecMacro accepts an absolute path unchanged when it contains the word Binaries.
$execDirectory = Join-Path $root ('Binaries\Validation\InspectorRendering-' + $runId)
$watchPath = Join-Path $execDirectory 'watch.exec'
$watchSlash = $watchPath.Replace('\','/')
# UE5.8 UEngine::HandleDeferCommand / TickDeferredCommands queue the next frame
# after normal world/network ticks. ULocalPlayer::ExecMacro rereads this file each
# time. Replacing the file after a replication marker supplies an external gate.
foreach ($directory in @($ReportDirectory,$execDirectory)) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
$instances = [Collections.Generic.List[object]]::new()
$keys = $null
$credentials = $null
$images = [Collections.Generic.List[object]]::new()
$utf8 = New-Object Text.UTF8Encoding($false)
$stagePaths = @($watchPath) + @(2..6 | ForEach-Object { Join-Path $execDirectory ("stage-$_.exec") })
$captureState = [PSCustomObject]@{ Active=$false; EvidenceOwned=$false; ReplicationReadyUtc=$null; MemoryCheckAfter=[DateTime]::MinValue }
$checks = [ordered]@{}

function Quote-RenderArgument([string]$Value) { '"' + (($Value -replace '(\\*)"','$1$1\"') -replace '(\\+)$','$1$1') + '"' }
function After-RenderFrames([int]$Frames, [string]$Command) { ('DEFER ' * $Frames) + $Command }
function Write-RenderDurableText([string]$Path, [string]$Text) {
    $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $bytes = $utf8.GetBytes($Text)
    $stream = [IO.FileStream]::new($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None,4096,[IO.FileOptions]::WriteThrough)
    try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
    if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($temporary,$Path,[NullString]::Value) }
    else { [IO.File]::Move($temporary,$Path) }
    $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,([IO.FileShare]::Read -bor [IO.FileShare]::Delete))
    try { $stream.Flush($true) } finally { $stream.Dispose() }
}
function Write-RenderReport([string]$Status, [string]$Failure = '') {
    $report = [ordered]@{
        TimestampUtc=[DateTime]::UtcNow.ToString('o'); Status=$Status; Passed=($Status -eq 'capture_pipeline_passed')
        Scope='Authenticated UE desktop capture pipeline only. Camera framing, the one-frame M input effect, multiview and 3D player information require visual review. Physical MR/haptics are not covered.'
        VisualReview='pending'; MultiViewToggleVerified=$false; Port=$Port; ReplicationReadyUtc=$captureState.ReplicationReadyUtc
        MinimumFreePhysicalMemoryGB=10; MaximumSnapshotAgeSeconds=3; ExecFiles=$stagePaths; Checks=$checks; Failure=$Failure
        Images=$images.ToArray(); Processes=@($instances | ForEach-Object { [ordered]@{ Name=$_.Name; Id=$(if ($_.Process) { $_.Process.Id } else { $null }); Log=$_.Log } })
    }
    Write-RenderDurableText (Join-Path $ReportDirectory 'report.json') ($report | ConvertTo-Json -Depth 9)
}
function Read-RenderLog([string]$Name) {
    $instance = $instances | Where-Object Name -EQ $Name | Select-Object -First 1
    if (-not $instance -or -not (Test-Path -LiteralPath $instance.Log)) { return '' }
    $stream = $null; $reader = $null
    try {
        $stream = [IO.File]::Open($instance.Log,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader = New-Object IO.StreamReader($stream)
        return $reader.ReadToEnd()
    } finally { if ($reader) { $reader.Dispose() } elseif ($stream) { $stream.Dispose() } }
}
function Get-RenderInspectorState {
    $log = Read-RenderLog 'inspector-0'
    $pattern = '(?m)^\[(?<time>\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\]\[\s*(?<frame>\d+)\]LogMRFPS: Display: MRFPS_SNAPSHOT players=(?<players>\d+) role=(?<role>\w+)\b'
    $snapshots = [regex]::Matches($log,$pattern)
    if ($snapshots.Count -eq 0) { return $null }
    $last = $snapshots[$snapshots.Count - 1]
    $style = [Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
    $utc = [DateTime]::ParseExact($last.Groups['time'].Value,'yyyy.MM.dd-HH.mm.ss:fff',[Globalization.CultureInfo]::InvariantCulture,$style)
    [PSCustomObject]@{ Timestamp=$utc; SnapshotUtc=$utc.ToString('o'); Offset=$last.Index; Frame=[int]$last.Groups['frame'].Value; Players=[int]$last.Groups['players'].Value; Role=$last.Groups['role'].Value; AgeSeconds=([DateTime]::UtcNow - $utc).TotalSeconds }
}
function Assert-RenderHealthy([string]$Context) {
    foreach ($instance in $instances) {
        if ($instance.Process -and $instance.Process.HasExited) { throw "$($instance.Name) exited during $Context. Inspect $($instance.Log)." }
        $log = Read-RenderLog $instance.Name
        if ($log.IndexOf([char]0) -ge 0) { throw "$($instance.Name) log contains NUL data; its evidence is invalid." }
        if ($log -match 'MRFPS_NETWORK_FAILURE|MRFPS_AUTH_STARTUP_FAILED|Fatal error:|Assertion failed:|Unhandled Exception:|Ensure condition failed:') { throw "$($instance.Name) reported a connection or runtime failure during $Context; capture rejected." }
    }
    if ([DateTime]::UtcNow -ge $captureState.MemoryCheckAfter) {
        $availableKB = [double](Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory
        if ($availableKB / 1048576 -lt 10) { throw 'Available physical memory fell below the required 10 GB; stopping this rendering test.' }
        $captureState.MemoryCheckAfter = [DateTime]::UtcNow.AddSeconds(1)
    }
    if ($captureState.Active) {
        $state = Get-RenderInspectorState
        if (-not $state -or $state.Role -ne 'inspector' -or $state.Players -ne 2) { throw "Latest replicated state is not inspector with two players during $Context; capture rejected." }
        if ($state.AgeSeconds -lt -1 -or $state.AgeSeconds -gt 3) { throw "Inspector replication snapshot is stale during $Context; capture rejected." }
    }
}
function Wait-RenderEvidence([string]$Description, [scriptblock]$Condition) {
    if ($DryRun) { return }
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Assert-RenderHealthy $Description
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for $Description. Evidence: $ReportDirectory"
}
function Set-RenderExec([string]$Path, [string[]]$Lines) {
    Write-RenderDurableText $Path (($Lines -join "`n") + "`n")
}
function Stop-RenderCaptureQueue {
    foreach ($path in $stagePaths) {
        if (Test-Path -LiteralPath $path) { Set-RenderExec $path @('Input.-key Tab','Input.-key M') }
    }
}
function Start-RenderInstance([string]$Name, [string[]]$ExtraArguments, [switch]$Render) {
    $log = Join-Path $ReportDirectory ($Name + '.log')
    $arguments = @($project) + $ExtraArguments
    if ($Name -ne 'server') {
        $token = if ($DryRun) { Join-Path $credentialDirectory ($Name + '.jwt') } else { $credentials.tokens.$Name }
        if (-not $token) { throw "No one-connection credential was issued for $Name." }
        $arguments += @("-MRFPSAuthTokenFile=$token","-MRFPSAuthPublicKeyFile=$($keys.public_key_path)",'-MRFPSAuthAudience=mrfps')
    }
    if ($Render) {
        # ParseExecCommands converts these inner single quotes to console double quotes.
        $initial = 't.MaxFPS 30,' + (After-RenderFrames 120 ("exec '" + $watchSlash + "'"))
        $arguments += @('-windowed','-ResX=1280','-ResY=720','-ForceRes',"-ExecCmds=$initial")
        if (-not $VisibleInspector) { $arguments += '-RenderOffscreen' }
    } else { $arguments += '-nullrhi' }
    $arguments += @('-nohmd','-nosound','-unattended','-NoSplash','-NoLiveCoding','-stdout','-FullStdOutLogOutput',"-abslog=$log")
    $line = ($arguments | ForEach-Object { Quote-RenderArgument $_ }) -join ' '
    Write-Host "$Name : $executable (log $log)"
    $process = $null
    if (-not $DryRun) {
        $captureState.MemoryCheckAfter = [DateTime]::MinValue
        Assert-RenderHealthy "starting $Name"
        $prior = @{}
        try {
            foreach ($variable in @('UE_SKIP_UBT_SDK_SETUP','MRFPS_AUTH_PUBLIC_KEY_B64','MRFPS_AUTH_PUBLIC_KEY_PEM','MRFPS_AUTH_AUDIENCE')) { $prior[$variable] = [Environment]::GetEnvironmentVariable($variable,'Process') }
            $env:UE_SKIP_UBT_SDK_SETUP = '1'
            $env:MRFPS_AUTH_PUBLIC_KEY_B64 = $null; $env:MRFPS_AUTH_PUBLIC_KEY_PEM = $null; $env:MRFPS_AUTH_AUDIENCE = $null
            if ($Name -eq 'server') { $env:MRFPS_AUTH_PUBLIC_KEY_B64 = $keys.public_key_b64; $env:MRFPS_AUTH_AUDIENCE = 'mrfps' }
            $style = if ($Render -and $VisibleInspector) { 'Normal' } else { 'Hidden' }
            $process = Start-Process -FilePath $executable -ArgumentList $line -WorkingDirectory $root -WindowStyle $style -PassThru
        } finally { foreach ($variable in $prior.Keys) { [Environment]::SetEnvironmentVariable($variable,$prior[$variable],'Process') } }
    }
    $instances.Add([PSCustomObject]@{ Name=$Name; Process=$process; Log=$log; Arguments=$arguments })
}
function Get-RenderPng([string]$Name) {
    $path = Join-Path $ReportDirectory ($Name + '.png')
    $stream = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
    try {
        $header = New-Object byte[] 24; $end = New-Object byte[] 12
        if ($stream.Length -lt 10000 -or $stream.Read($header,0,24) -ne 24 -or [BitConverter]::ToString($header,0,8) -ne '89-50-4E-47-0D-0A-1A-0A' -or [BitConverter]::ToString($header,12,4) -ne '49-48-44-52') { throw "Invalid PNG signature or header: $path" }
        [void]$stream.Seek(-12,[IO.SeekOrigin]::End)
        if ($stream.Read($end,0,12) -ne 12 -or [BitConverter]::ToString($end) -ne '00-00-00-00-49-45-4E-44-AE-42-60-82') { throw "PNG is incomplete or corrupt: $path" }
        $width = [uint32]$header[16]*16777216 + [uint32]$header[17]*65536 + [uint32]$header[18]*256 + [uint32]$header[19]
        $height = [uint32]$header[20]*16777216 + [uint32]$header[21]*65536 + [uint32]$header[22]*256 + [uint32]$header[23]
        if ($width -ne 1280 -or $height -ne 720) { throw "Unexpected rendered PNG dimensions: $width x $height" }
        $length = $stream.Length
        $stream.Flush($true)
    } finally { $stream.Dispose() }
    [PSCustomObject]@{ Name=$Name; Path=$path; Width=$width; Height=$height; Bytes=$length; PngSignatureAndEndValidated=$true; Sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
}

try {
    if (@(Get-ChildItem -LiteralPath $ReportDirectory -File).Count -ne 0) { throw 'Use an empty report directory; old logs or PNGs cannot be accepted as a new capture.' }
    $captureState.EvidenceOwned = $true
    Write-RenderReport 'in_progress'
    if (-not $DryRun) {
        foreach ($required in @('Content\Maps\MRFPSArena.umap','Binaries\Win64\UnrealEditor-MRFPS.dll')) {
            if (-not (Test-Path -LiteralPath (Join-Path $root $required))) { throw 'Build the Editor and generate the arena before running rendering verification.' }
        }
        $probe = New-Object Net.Sockets.UdpClient
        try { $probe.Client.Bind((New-Object Net.IPEndPoint([Net.IPAddress]::Any,$Port))) } finally { $probe.Dispose() }
    }
    # Every subsequent stage starts as an idle watcher. Only the current stage is
    # ever armed; a failed/disconnected capture cannot queue the remaining images.
    foreach ($path in $stagePaths) {
        Set-RenderExec $path @((After-RenderFrames 30 ('exec "' + $path.Replace('\','/') + '"')))
    }
    $keys = New-MRFPSTestAuthKeys -Directory $credentialDirectory -DryRun:$DryRun
    Start-RenderInstance 'server' @('/Game/Maps/MRFPSArena','-server',"-port=$Port")
    Wait-RenderEvidence 'the server world and public DTLS fingerprint' {
        $startup = Read-RenderLog 'server'
        ($startup -match 'MRFPS_SERVER_READY') -and ($startup -match 'MRFPS_AUTH_TRANSPORT_READY fingerprint=[0-9a-f]{64}')
    }
    if (-not $DryRun) {
        $fingerprint = [regex]::Match((Read-RenderLog 'server'),'MRFPS_AUTH_TRANSPORT_READY fingerprint=([0-9a-f]{64})').Groups[1].Value
        $credentials = Complete-MRFPSTestAuthTokens -Directory $credentialDirectory -Fingerprint $fingerprint -Inspectors 1
    }
    $url = "127.0.0.1:$Port"
    Start-RenderInstance 'player-0' @('/Game/Maps/MRFPSArena',"-MRFPSServer=$url",'-game','-MRFPSDesktop','-MRFPSAutoReady')
    Wait-RenderEvidence 'player 0 calibration' { (Read-RenderLog 'server') -match 'MRFPS_CALIBRATED server slot=0 ready=1' }
    Start-RenderInstance 'player-1' @('/Game/Maps/MRFPSArena',"-MRFPSServer=$url",'-game','-MRFPSDesktop','-MRFPSAutoReady')
    Wait-RenderEvidence 'player 1 calibration and match start' {
        $log = Read-RenderLog 'server'
        ($log -match 'MRFPS_CALIBRATED server slot=1 ready=1') -and ($log -match 'MRFPS_MATCH playing')
    }
    # Smoke supplies replication snapshots and cycles POV0/POV1/free once, then stops.
    Start-RenderInstance 'inspector-0' @('/Game/Maps/MRFPSArena',"-MRFPSServer=$url",'-game','-MRFPSDesktop','-MRFPSInspector','-MRFPSSmoke') -Render
    Wait-RenderEvidence 'two replicated players and completion of the inspector startup view cycle' {
        $log = Read-RenderLog 'inspector-0'
        $state = Get-RenderInspectorState
        $state -and $state.Role -eq 'inspector' -and $state.Players -eq 2 -and $state.AgeSeconds -ge -1 -and $state.AgeSeconds -le 3 -and ($log -match 'MRFPS_VIEW inspector=free')
    }
    if (-not $DryRun) {
        $captureState.ReplicationReadyUtc = [DateTime]::UtcNow.ToString('o')
        $captureState.Active = $true
        Assert-RenderHealthy 'arming the first screenshot'
    }
    $baseline = (Read-RenderLog 'inspector-0').Length
    $phases = @(
        [PSCustomObject]@{ Name='free-multiview'; Actions=@('Input.+key Tab 1','DEFER Input.-key Tab'); Marker='MRFPS_VIEW inspector=free'; MultiViewIntent='default enabled' },
        [PSCustomObject]@{ Name='free-single'; Actions=@('Input.+key M 1','DEFER Input.-key M'); Marker=''; MultiViewIntent='one M press requests disabled; visual confirmation required' },
        [PSCustomObject]@{ Name='pov-0'; Actions=@('MRFPSView 0'); Marker='MRFPS_VIEW inspector=player slot=0'; MultiViewIntent='disabled requested; visual confirmation required' },
        [PSCustomObject]@{ Name='pov-1'; Actions=@('MRFPSView 1'); Marker='MRFPS_VIEW inspector=player slot=1'; MultiViewIntent='disabled requested; visual confirmation required' },
        [PSCustomObject]@{ Name='pov-1-multiview'; Actions=@('Input.+key M 1','DEFER Input.-key M'); Marker=''; MultiViewIntent='one M press requests enabled; visual confirmation required' },
        [PSCustomObject]@{ Name='free-return-multiview'; Actions=@('Input.+key Tab 1','DEFER Input.-key Tab'); Marker='MRFPS_VIEW inspector=free'; MultiViewIntent='enabled requested; visual confirmation required' }
    )
    for ($stage = 0; $stage -lt $phases.Count; $stage++) {
        $phase = $phases[$stage]
        if (-not $DryRun) { Assert-RenderHealthy ("before " + $phase.Name) }
        $before = if ($DryRun) { $null } else { Get-RenderInspectorState }
        $phaseBaseline = (Read-RenderLog 'inspector-0').Length
        $requestedAt = [DateTime]::UtcNow
        $output = (Join-Path $ReportDirectory ($phase.Name + '.png')).Replace('\','/')
        $commands = @($phase.Actions) + @((After-RenderFrames 12 ('SHOT filename="' + $output + '" -nosuffix')))
        if ($stage + 1 -lt $stagePaths.Count) { $commands += After-RenderFrames 30 ('exec "' + $stagePaths[$stage + 1].Replace('\','/') + '"') }
        # Canvas HUD and 3D text are part of the viewport. Plain SHOT avoids the
        # extra Slate PrivateDrawWindows pass used by SHOWUI.
        Set-RenderExec $stagePaths[$stage] $commands
        if ($DryRun) { continue }
        $imageState = [PSCustomObject]@{ Size=-1L; Stable=0 }
        Wait-RenderEvidence ("saved PNG " + $phase.Name) {
            if (-not (Test-Path -LiteralPath $output)) { return $false }
            $length = (Get-Item -LiteralPath $output).Length
            if ($length -lt 10000) { return $false }
            if ($length -eq $imageState.Size) { $imageState.Stable++ } else { $imageState.Size=$length; $imageState.Stable=0 }
            $imageState.Stable -ge 3
        }
        Assert-RenderHealthy ("validating PNG " + $phase.Name)
        $image = Get-RenderPng $phase.Name
        $validatedAt = [DateTime]::UtcNow
        # A pre-shot snapshot is insufficient: require a NEW replicated snapshot
        # after the file has been saved and validated, still on the same connection.
        Wait-RenderEvidence ("fresh inspector replication after " + $phase.Name) {
            $state = Get-RenderInspectorState
            $state -and $state.Offset -gt $before.Offset -and $state.Timestamp -ge $validatedAt
        }
        Assert-RenderHealthy ("after " + $phase.Name)
        $after = Get-RenderInspectorState
        $log = Read-RenderLog 'inspector-0'
        $delta = $log.Substring([Math]::Min($phaseBaseline,$log.Length))
        if ($delta -match 'Command not recognized|Failed to locate a key|not using the Enhanced Input') { throw "Console action failed for $($phase.Name)." }
        $markerCount = if ($phase.Marker) { [regex]::Matches($delta,[regex]::Escape($phase.Marker)).Count } else { 0 }
        if ($phase.Marker -and $markerCount -ne 1) { throw "Expected exactly one view-selection event for $($phase.Name), observed $markerCount; capture rejected." }
        $image | Add-Member -NotePropertyMembers @{ RequestedAtUtc=$requestedAt.ToString('o'); FileValidatedAtUtc=$validatedAt.ToString('o'); SnapshotBefore=$before; SnapshotAfter=$after; ConsoleCommands=$commands; ViewEventCount=$markerCount; MultiViewIntent=$phase.MultiViewIntent; VisualReview='pending' }
        $images.Add($image)
        $checks[($phase.Name + '.FreshInspectorBeforeAndAfter')] = $true
        Write-RenderReport 'in_progress'
    }
    if ($DryRun) { Write-RenderReport 'dry_run'; Write-Host "Dry run prepared staged capture files at $execDirectory"; return }
    Assert-RenderHealthy 'final capture checks'
    $allLogs = ($instances | ForEach-Object { Read-RenderLog $_.Name }) -join "`n"
    $inspectorLog = Read-RenderLog 'inspector-0'
    $captureLog = $inspectorLog.Substring([Math]::Min($baseline,$inspectorLog.Length))
    $serverLog = Read-RenderLog 'server'
    $finalChecks = [ordered]@{
        AuthenticatedTwoPlayerSlots = (($serverLog -match 'MRFPS_JOIN role=player slot=0') -and ($serverLog -match 'MRFPS_JOIN role=player slot=1'))
        AuthenticatedInspector = ($serverLog -match 'MRFPS_JOIN role=inspector')
        EveryImageHasFreshInspectorStateBeforeAndAfter = ($images.Count -eq 6 -and @($images | Where-Object { $_.SnapshotBefore.Role -ne 'inspector' -or $_.SnapshotBefore.Players -ne 2 -or $_.SnapshotAfter.Role -ne 'inspector' -or $_.SnapshotAfter.Players -ne 2 }).Count -eq 0)
        NativeRendererUsed = ($inspectorLog -match 'LogD3D12RHI|LogD3D11RHI|LogVulkanRHI')
        ManualPov0CommandExecuted = ($captureLog -match 'MRFPS_VIEW inspector=player slot=0')
        ManualPov1CommandExecuted = ($captureLog -match 'MRFPS_VIEW inspector=player slot=1')
        TabInputReturnedFreeViewOncePerPress = ([regex]::Matches($captureLog,'MRFPS_VIEW inspector=free').Count -eq 2)
        NoConsoleCommandFailure = ($captureLog -notmatch 'Command not recognized|Failed to locate a key|not using the Enhanced Input')
        SixValid1280x720Pngs = ($images.Count -eq 6 -and @($images | Where-Object { $_.Width -ne 1280 -or $_.Height -ne 720 -or -not $_.PngSignatureAndEndValidated }).Count -eq 0)
        DistinctRenderedViews = (@($images.Sha256 | Select-Object -Unique).Count -ge 4)
        NoNetworkFailureOrRuntimeError = ($allLogs -notmatch 'MRFPS_NETWORK_FAILURE|Fatal error:|Assertion failed:|Unhandled Exception:|Ensure condition failed:')
        NoCredentialLeak = (Test-MRFPSCredentialPrivacy -Text $allLogs -TokenFiles @($credentials.tokens.PSObject.Properties | ForEach-Object Value))
    }
    foreach ($key in $finalChecks.Keys) { $checks[$key] = $finalChecks[$key] }
    $failures = @($checks.Keys | Where-Object { -not $checks[$_] })
    if ($failures.Count) { throw "Inspector capture checks failed: $($failures -join ', '). Evidence: $ReportDirectory" }
    Write-RenderReport 'capture_pipeline_passed'
    Write-Host "Inspector rendering capture passed; visual review pending. Evidence: $ReportDirectory"
    if ($VisibleInspector -and $ReviewHoldSeconds -gt 0) {
        Write-Host "Windows inspector is available for native window capture for $ReviewHoldSeconds seconds."
        $reviewDeadline = [DateTime]::UtcNow.AddSeconds($ReviewHoldSeconds)
        while ([DateTime]::UtcNow -lt $reviewDeadline) {
            Assert-RenderHealthy 'native Windows window review'
            Start-Sleep -Milliseconds 250
        }
    }
} catch {
    $failure = $_
    if ($captureState.EvidenceOwned) {
        try { Stop-RenderCaptureQueue; Write-RenderDurableText (Join-Path $ReportDirectory 'harness-error.txt') ($failure | Out-String); Write-RenderReport 'failed' $failure.Exception.Message }
        catch { Write-Warning 'Could not durably persist all rendering failure evidence.' }
    }
    throw $failure
} finally {
    try { Stop-RenderCaptureQueue } catch { Write-Warning 'Could not disarm all capture stage files.' }
    if (-not $KeepProcesses) {
        foreach ($instance in $instances) {
            if ($instance.Process -and -not $instance.Process.HasExited) {
                # A worker can spend minutes in one shader batch before noticing its
                # parent exited. Only stop workers belonging to this test's Editor.
                $workers = @(Get-CimInstance Win32_Process -Filter ("Name='ShaderCompileWorker.exe' AND ParentProcessId=" + $instance.Process.Id) -ErrorAction SilentlyContinue)
                $instance.Process.Kill(); [void]$instance.Process.WaitForExit(5000)
                foreach ($worker in $workers) {
                    if ($worker.ExecutablePath -eq (Join-Path $engine 'Engine\Binaries\Win64\ShaderCompileWorker.exe')) { Stop-Process -Id $worker.ProcessId -ErrorAction SilentlyContinue }
                }
            }
        }
    }
}
