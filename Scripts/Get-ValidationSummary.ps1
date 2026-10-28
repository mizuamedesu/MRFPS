[CmdletBinding()]
param(
    [string]$ProjectRoot,
    [string]$ValidationDirectory,
    [string[]]$BuildLog = @(),
    [string]$OutputPath,
    [switch]$NoWrite,
    [switch]$SkipProcessInspection
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
if (-not $ProjectRoot) { $ProjectRoot = Get-MRFPSRoot }
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
if (-not $ValidationDirectory) { $ValidationDirectory = Join-Path $ProjectRoot 'Saved\Validation' }
if (-not $OutputPath) { $OutputPath = Join-Path $ValidationDirectory 'summary.json' }
$projectPath = (Join-Path $ProjectRoot 'MRFPS.uproject').Replace('\','/')
$gates = [Collections.Generic.List[object]]::new()
$warnings = [Collections.Generic.List[string]]::new()

function Add-ValidationGate([string]$Id, [string]$Status, [string]$Scope, [string]$Detail, [object[]]$Evidence = @()) {
    $gates.Add([PSCustomObject]@{ Id = $Id; Status = $Status; Scope = $Scope; Detail = $Detail; Evidence = @($Evidence | Where-Object { $_ }) })
}
function Read-ValidationJson([IO.FileInfo]$File) {
    if (-not $File) { return $null }
    try { [PSCustomObject]@{ Path = $File.FullName; ModifiedUtc = $File.LastWriteTimeUtc.ToString('o'); Data = (Get-Content -LiteralPath $File.FullName -Raw -Encoding UTF8 | ConvertFrom-Json) } }
    catch { $warnings.Add("Invalid JSON ignored: $($File.FullName)"); return $null }
}
function Get-LatestJson([string]$Pattern) {
    if (-not (Test-Path -LiteralPath $ValidationDirectory)) { return $null }
    $validationPrefix = [IO.Path]::GetFullPath($ValidationDirectory).TrimEnd('\','/')
    $file = Get-ChildItem -LiteralPath $ValidationDirectory -Filter '*.json' -Recurse -File | Where-Object {
        $relative = $_.FullName.Substring($validationPrefix.Length).TrimStart('\','/').Replace('\','/')
        $relative -notmatch '(^|/)(?:SummaryFixtures|Fixtures|TestFixtures)[^/]*/' -and $_.FullName.Replace('\','/') -match $Pattern
    } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    Read-ValidationJson $file
}
function Read-Log([string]$Path) {
    if ($Path -and (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $stream = $null
        $reader = $null
        try {
            $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
            $reader = New-Object IO.StreamReader($stream)
            return $reader.ReadToEnd()
        } catch { $warnings.Add("Log could not be read: $Path") }
        finally { if ($reader) { $reader.Dispose() } elseif ($stream) { $stream.Dispose() } }
    }
    return ''
}

$sourceFiles = @()
foreach ($directory in @('Source','Plugins','Config')) {
    $path = Join-Path $ProjectRoot $directory
    if (Test-Path -LiteralPath $path) {
        $sourceFiles += @(Get-ChildItem -LiteralPath $path -Recurse -File | Where-Object { $_.Extension -in '.cpp','.h','.cs','.uplugin','.ini' -and $_.FullName.Replace('\','/') -notmatch '/(Binaries|Intermediate)/' })
    }
}
$latestSource = $sourceFiles | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
$disabledPlugins = @(Get-MRFPSDisabledPluginNames -ProjectRoot $ProjectRoot)
function Get-FreshStatus([string]$ProposedStatus, [string]$EvidencePath) {
    if ($ProposedStatus -eq 'passed' -and $latestSource -and $EvidencePath -and (Test-Path -LiteralPath $EvidencePath)) {
        if ($latestSource.LastWriteTimeUtc -gt (Get-Item -LiteralPath $EvidencePath).LastWriteTimeUtc) { return 'stale' }
    }
    return $ProposedStatus
}

# Read local process state only. A log that stops growing is not treated as a dead build.
$activeBuilds = @()
$queuedBuilds = @()
$processInspection = 'skipped'
if (-not $SkipProcessInspection) {
    try {
        $processes = @(Get-CimInstance Win32_Process)
        $activeBuilds = @($processes | Where-Object {
            $_.CommandLine -and $_.Name -match '^(dotnet|UnrealBuildTool)(\.exe)?$' -and
            $_.CommandLine -match 'UnrealBuildTool' -and $_.CommandLine.Replace('\','/').Contains($projectPath)
        } | ForEach-Object {
            $buildProcess = $_
            $descendantIds = [Collections.Generic.HashSet[int]]::new()
            [void]$descendantIds.Add([int]$buildProcess.ProcessId)
            do {
                $added = $false
                foreach ($process in $processes) {
                    if ($descendantIds.Contains([int]$process.ParentProcessId) -and $descendantIds.Add([int]$process.ProcessId)) { $added = $true }
                }
            } while ($added)
            [PSCustomObject]@{ ProcessId = $buildProcess.ProcessId; Target = $(if ($buildProcess.CommandLine -match 'MRFPSEditor') { 'Editor' } elseif ($buildProcess.CommandLine -match 'MRFPSServer') { 'LinuxServer' } elseif ($buildProcess.CommandLine -match 'Android') { 'Quest' } else { 'Game' }); StartedUtc = $buildProcess.CreationDate.ToUniversalTime().ToString('o'); CompilerChildren = @($processes | Where-Object { $descendantIds.Contains([int]$_.ProcessId) -and $_.Name -match '^(cl|clang|clang\+\+)(\.exe)?$' }).Count }
        })
        $queuedBuilds = @($processes | Where-Object {
            $_.Name -eq 'cmd.exe' -and $_.CommandLine -and $_.CommandLine -match 'Build\.bat"?\s+MRFPS' -and $_.CommandLine.Replace('\','/').Contains($projectPath)
        } | ForEach-Object {
            $batch = $_
            $target = if ($batch.CommandLine -match 'MRFPSEditor') { 'Editor' } elseif ($batch.CommandLine -match 'MRFPSServer') { 'LinuxServer' } elseif ($batch.CommandLine -match 'Android') { 'Quest' } else { 'Game' }
            if (-not ($activeBuilds | Where-Object Target -EQ $target)) {
                [PSCustomObject]@{ ProcessId = $batch.ProcessId; Target = $target; StartedUtc = $batch.CreationDate.ToUniversalTime().ToString('o'); State = 'Live Build.bat caller; native build has not been confirmed and may be waiting for the engine build lock.' }
            }
        })
        $processInspection = 'queried local Win32_Process'
    } catch { $warnings.Add('Build process state could not be inspected; no terminal-state inference was made.'); $processInspection = 'unavailable' }
}

$environment = $null
$environmentFile = Join-Path $ValidationDirectory 'environment.json'
if (Test-Path -LiteralPath $environmentFile) { $environment = Read-ValidationJson (Get-Item -LiteralPath $environmentFile) }
$engineRoot = $null
if ($environment -and $environment.Data.Engine) { $engineRoot = $environment.Data.Engine.Root }
if (-not $engineRoot) { try { $engineRoot = Resolve-MRFPSEngine } catch { } }
$engineVersion = $null
if ($engineRoot -and (Test-Path -LiteralPath (Join-Path $engineRoot 'Engine\Build\Build.version'))) {
    $version = Get-Content -LiteralPath (Join-Path $engineRoot 'Engine\Build\Build.version') -Raw | ConvertFrom-Json
    $engineVersion = "$($version.MajorVersion).$($version.MinorVersion).$($version.PatchVersion)"
}
Add-ValidationGate 'Unreal58Installed' $(if ($engineVersion -like '5.8.*') { 'passed' } else { 'pending' }) 'Engine installation only' "Detected engine version: $engineVersion. Installation does not establish a successful project build." @($engineRoot)

$android = [ordered]@{ Sdk = $null; Ndk = $null; BuildTools = $null; Platform = $null; CMake = $null; Ready = $false }
if ($engineRoot -and (Test-Path -LiteralPath (Join-Path $engineRoot 'Engine\Config\Android\Android_SDK.json'))) {
    $requirements = Get-Content -Raw -LiteralPath (Join-Path $engineRoot 'Engine\Config\Android\Android_SDK.json') | ConvertFrom-Json
    $sdk = Join-Path $ProjectRoot '.local\android'
    if ($environment -and $environment.Data.Android.Sdk) { $sdk = $environment.Data.Android.Sdk }
    $android.Sdk = $sdk
    $android.Ndk = $requirements.ndk
    $android.BuildTools = $requirements.'build-tools'
    $android.Platform = $requirements.platforms
    $android.CMake = $requirements.cmake
    $paths = @((Join-Path $sdk 'platform-tools\adb.exe'), (Join-Path $sdk ('ndk\' + $requirements.ndk + '\source.properties')), (Join-Path $sdk ('build-tools\' + $requirements.'build-tools' + '\aapt2.exe')), (Join-Path $sdk ('platforms\' + $requirements.platforms + '\android.jar')), (Join-Path $sdk ('cmake\' + $requirements.cmake + '\bin\cmake.exe')))
    $android.Ready = @($paths | Where-Object { -not (Test-Path -LiteralPath $_) }).Count -eq 0
}
Add-ValidationGate 'AndroidToolchain' $(if ($android.Ready) { 'passed' } else { 'pending' }) 'Installed files only' 'Checks the SDK requirements in the selected engine source. This is not a completed Android build.' @($android.Sdk)

$logFiles = @()
foreach ($directory in @((Join-Path $ProjectRoot 'Saved\BuildLogs'),(Join-Path $ProjectRoot 'Saved\Logs'))) {
    if (Test-Path -LiteralPath $directory) { $logFiles += @(Get-ChildItem -LiteralPath $directory -Filter '*.log' -File) }
}
if ($engineRoot) { $BuildLog += Join-Path $engineRoot 'Engine\Programs\UnrealBuildTool\Log.txt' }
foreach ($path in $BuildLog) { if (Test-Path -LiteralPath $path -PathType Leaf) { $logFiles += Get-Item -LiteralPath $path } }
$buildEvidence = @($logFiles | Sort-Object FullName -Unique | Sort-Object LastWriteTimeUtc -Descending | ForEach-Object {
    $content = Read-Log $_.FullName
    if ($content.Replace('\','/').Contains($projectPath)) {
        [PSCustomObject]@{ Path = $_.FullName; ModifiedUtc = $_.LastWriteTimeUtc.ToString('o'); Text = $content }
    }
})
foreach ($scope in @('Editor','LinuxServer','Quest')) {
    $matchingLog = $buildEvidence | Where-Object {
        if ($scope -eq 'Editor') { $_.Text -match 'MRFPSEditor' }
        elseif ($scope -eq 'LinuxServer') { $_.Text -match 'MRFPSServer' -and $_.Text -match 'Linux' }
        else { $_.Text -match '(?:platform=Android|MRFPS Android|TargetPlatform = Android)' }
    } | Select-Object -First 1
    $status = 'pending'
    $detail = 'No project-scoped terminal build evidence was found.'
    if ($activeBuilds | Where-Object Target -EQ $scope) {
        $status = 'in_progress'; $detail = 'A matching UnrealBuildTool process is live now; no completed build is claimed.'
        if ($matchingLog) {
            $progress = [regex]::Matches($matchingLog.Text, '\[(\d+)/(\d+)\]') | Select-Object -Last 1
            if ($progress) {
                foreach ($build in $activeBuilds | Where-Object Target -EQ $scope) {
                    $build | Add-Member -NotePropertyName LastLoggedAction -NotePropertyValue ([int]$progress.Groups[1].Value) -Force
                    $build | Add-Member -NotePropertyName TotalLoggedActions -NotePropertyValue ([int]$progress.Groups[2].Value) -Force
                }
            }
        }
    }
    elseif ($queuedBuilds | Where-Object Target -EQ $scope) { $status = 'waiting'; $detail = 'A project-scoped Build.bat process is live with no confirmed target UBT process yet. It may be waiting for the engine build lock; no completed build is claimed.' }
    elseif ($matchingLog) {
        if ($matchingLog.Text -match 'Result: Succeeded|AutomationTool exiting with ExitCode=0') { $status = Get-FreshStatus 'passed' $matchingLog.Path; $detail = 'The latest scoped log contains a successful terminal result. Timestamp freshness is a heuristic, not source/binary identity verification.' }
        elseif ($matchingLog.Text -match 'Result: Failed|AutomationTool exiting with ExitCode=[1-9]|BUILD FAILED') { $status = 'failed'; $detail = 'The latest scoped log contains a failed terminal result.' }
        else { $status = 'unverified'; $detail = 'The latest scoped log has no terminal result and no matching live process was confirmed. No stopped or failed state is inferred.' }
    }
    Add-ValidationGate ($scope + 'Build') $status 'Project build' $detail @($matchingLog.Path)
}
$mapPath = Join-Path $ProjectRoot 'Content\Maps\MRFPSArena.umap'
Add-ValidationGate 'GeneratedArenaMap' $(if (Test-Path -LiteralPath $mapPath -PathType Leaf) { 'passed' } else { 'pending' }) 'Map artifact only' 'Map presence does not prove physical calibration or arena dimensions on the headset.' @($mapPath)

$automation = Get-LatestJson '/Automation[^/]*/(?:.*/)?MRFPS/index\.json$'
if (-not $automation) { $automation = Get-LatestJson '/Automation[^/]*/index\.json$' }
$expectedAutomation = @($sourceFiles | Where-Object Extension -EQ '.cpp' | ForEach-Object {
    $sourcePath = $_.FullName.Replace('\','/')
    foreach ($disabled in $disabledPlugins) { if ($sourcePath -like "*/Plugins/$disabled/*") { return } }
    $source = Read-Log $_.FullName
    [regex]::Matches($source, 'IMPLEMENT_[A-Z_]*AUTOMATION_TEST\s*\([^,]+,\s*"(MRFPS\.[^"]+)"') | ForEach-Object { $_.Groups[1].Value }
} | Sort-Object -Unique)
$automationStatus = 'pending'
$automationDetail = 'No exported UE Automation report was found.'
if ($automation) {
    $tests = @($automation.Data.tests | Where-Object { $_.fullTestPath -like 'MRFPS.*' })
    if ($tests.Count -gt 0) {
        $failed = @($tests | Where-Object state -EQ 'Fail')
        $unfinished = @($tests | Where-Object { $_.state -notin 'Success','Fail' })
        $missing = @($expectedAutomation | Where-Object { $_ -notin $tests.fullTestPath })
        $automationStatus = if ($failed.Count) { 'failed' } elseif ($unfinished.Count -or $missing.Count -or -not $expectedAutomation.Count) { 'unverified' } else { Get-FreshStatus 'passed' $automation.Path }
        $automationDetail = "MRFPS tests: $($tests.Count), failed: $($failed.Count), unfinished: $($unfinished.Count), expected tests absent: $($missing -join ', '). This covers the source-registered MRFPS automation cases only."
    } else { $automationStatus = 'unverified'; $automationDetail = 'The automation report contains no MRFPS test cases.' }
}
Add-ValidationGate 'CoreAutomation' $automationStatus 'UE automated tests' $automationDetail @($automation.Path)

$multiplayer = Get-LatestJson '/Multiplayer-[^/]+/report\.json$'
$multiplayerStatus = 'pending'
$multiplayerDetail = 'No desktop multiplayer smoke report was found.'
if ($multiplayer) {
    if ($multiplayer.Data.Passed -eq $false) { $multiplayerStatus = 'failed'; $multiplayerDetail = 'The latest desktop multiplayer run reported a failure; inspect its checks and logs.' }
    elseif ($multiplayer.Data.Passed -eq $true) {
        $checks = $multiplayer.Data.Checks
        $requiredChecks = @('TwoPlayerSlots','InspectorsJoined','ExtraPlayerRejected','ExactlyTwoPlayerJoins','MatchStarted','PlayersCalibrated','WallSpawned','AuthoritativeDamage','ShieldBlockedShot','NoFatalError','PublicKeyAuthenticatedServer','NoCredentialLeak')
        $checksValid = ($checks -and $multiplayer.Data.InspectorCount -ge 2)
        foreach ($name in $requiredChecks) { if (-not $checks -or $checks.$name -ne $true) { $checksValid = $false } }
        foreach ($i in 0..([Math]::Max(0,[int]$multiplayer.Data.InspectorCount - 1))) {
            foreach ($suffix in @('ReceivedTwoPlayers','ReceivedWall','ReceivedDamage','ViewedBothPlayers','ReturnedToFreeView')) {
                $name = "Inspector$i$suffix"
                if (-not $checks -or $checks.$name -ne $true) { $checksValid = $false }
            }
        }
        $logsExist = @($multiplayer.Data.Processes).Count -ge (4 + [int]$multiplayer.Data.InspectorCount)
        $processLogs = @{}
        $reportParent = (Split-Path -Parent $multiplayer.Path).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
        foreach ($process in @($multiplayer.Data.Processes)) {
            if (-not $process.Log -or -not (Test-Path -LiteralPath $process.Log -PathType Leaf)) { $logsExist = $false; continue }
            $resolved = [IO.Path]::GetFullPath($process.Log)
            if (-not $resolved.StartsWith($reportParent, [StringComparison]::OrdinalIgnoreCase)) { $logsExist = $false; continue }
            $processLogs[$process.Name] = Read-Log $resolved
        }
        $serverLog = $processLogs['server']
        foreach ($marker in @('MRFPS_AUTH_TRANSPORT_READY fingerprint=[0-9a-f]{64}','MRFPS_JOIN role=player slot=0','MRFPS_JOIN role=player slot=1','MRFPS_CALIBRATED server slot=0 ready=1','MRFPS_CALIBRATED server slot=1 ready=1','MRFPS_MATCH playing','MRFPS_WALL','MRFPS_DAMAGE','MRFPS_BLOCK')) {
            if (-not $serverLog -or $serverLog -notmatch $marker) { $logsExist = $false }
        }
        if ($processLogs['extra-player'] -notmatch 'ARENA_FULL') { $logsExist = $false }
        for ($i = 0; $i -lt [int]$multiplayer.Data.InspectorCount; $i++) {
            $inspectorLog = $processLogs["inspector-$i"]
            foreach ($marker in @('MRFPS_SNAPSHOT players=2 role=inspector','MRFPS_SNAPSHOT[^\r\n]*walls=[1-9][0-9]*','MRFPS_SNAPSHOT[^\r\n]*p1hp=(?:[0-9]|[1-9][0-9])\.[0-9]','MRFPS_VIEW inspector=player slot=0','MRFPS_VIEW inspector=player slot=1','MRFPS_VIEW inspector=free')) {
                if (-not $inspectorLog -or $inspectorLog -notmatch $marker) { $logsExist = $false }
            }
        }
        $multiplayerStatus = if ($checksValid -and $logsExist) { Get-FreshStatus 'passed' $multiplayer.Path } else { 'unverified' }
        $multiplayerDetail = 'A pass requires all current slot/combat/observer checks, at least two inspectors, and retained per-process logs. This does not prove physical MR or haptics.'
    }
}
Add-ValidationGate 'DesktopMultiplayer' $multiplayerStatus 'Desktop network and game rules only' $multiplayerDetail @($multiplayer.Path)

$admission = Get-LatestJson '/AuthAdmission-[^/]+/report\.json$'
$admissionStatus = 'pending'
$admissionDetail = 'No actual UE admission test report was found. Offline signature/profile tests are separate evidence.'
if ($admission) {
    $admissionStatus = if ($admission.Data.Passed -eq $false) { 'failed' } else { 'unverified' }
    if ($admission.Data.Passed -eq $true -and $admission.Data.NoCredentialLeak -eq $true -and $admission.Data.NoFatalError -eq $true -and $admission.Data.ValidClientsRemainAlive -eq $true) {
        $allCases = @($admission.Data.Cases)
        $complete = $allCases.Count -eq 14
        foreach ($role in @('player','inspector')) {
            foreach ($kind in @('missing','fake','expired','audience','role','replay','transport')) {
                $case = @($allCases | Where-Object Name -EQ "$kind-$role")
                if ($case.Count -ne 1 -or $case[0].Passed -ne $true -or $case[0].NoAdditionalServerJoin -ne $true) { $complete = $false }
                if ($kind -eq 'replay' -and ($case.Count -ne 1 -or $case[0].ServerRejectionCodeObserved -ne $true)) { $complete = $false }
                if ($kind -eq 'transport' -and ($case.Count -ne 1 -or $case[0].NativeTlsPinFailureObserved -ne $true -or $case[0].ServerJwtTransportRejectionObserved -ne $false)) { $complete = $false }
            }
        }
        foreach ($process in @($admission.Data.Processes)) { if (-not $process.Log -or -not (Test-Path -LiteralPath $process.Log -PathType Leaf)) { $complete = $false } }
        if (@($admission.Data.Processes).Count -lt 17) { $complete = $false }
        if ($complete) { $admissionStatus = Get-FreshStatus 'passed' $admission.Path }
    }
    $admissionDetail = 'Scope: real UE clients accept valid roles and reject seven negative cases per role without an extra server join. Replay requires a fresh server rejection marker; a wrong signed DTLS pin requires native TLS fingerprint rejection before server JWT processing. Local pre-network rejection is not represented as server receipt of a forged token.'
}
Add-ValidationGate 'SignedTokenAdmission' $admissionStatus 'Actual UE authentication/admission tests' $admissionDetail @($admission.Path)

$container = Get-LatestJson '/LinuxContainer[^/]*/report\.json$'
$containerStatus = if ($container) { 'needs_review' } else { 'pending' }
if ($container -and $container.Data.Running -eq $false) { $containerStatus = 'failed' }
Add-ValidationGate 'LinuxGameContainer' $containerStatus 'Actual packaged game container' 'Docker installation, echo fixtures, and server headers do not prove the game container ran. A game-container runtime report, image identity and its server log require review; no current container state is inferred offline.' @($container.Path)
$quest = Get-LatestJson '/Quest-[^/]+/report\.json$'
$questStatus = 'pending'
$questDetail = 'No Quest app process smoke report was found.'
if ($quest -and $quest.Data.RuntimeProcessAlive -eq $true) {
    $questStatus = 'needs_review'
    $questDetail = 'A prior report recorded an alive Android process. Check retained logs and APK identity; this does not prove MR rendering or physical interactions.'
}
Add-ValidationGate 'QuestAppRuntime' $questStatus 'Quest app process only' $questDetail @($quest.Path)
$questDevices = @()
if ($environment) { $questDevices = @($environment.Data.Quest) }
$lastUnauthorized = @($questDevices | Where-Object State -EQ 'unauthorized').Count -gt 0
Add-ValidationGate 'QuestAuthorization' 'pending' 'Last captured ADB state' $(if ($lastUnauthorized) { 'Last environment capture recorded unauthorized. Accept this computer RSA prompt in the headset, then refresh Test-Environment; this summary does not poll ADB.' } else { 'Review the last environment capture. This summary does not assert current ADB authorization from a stale report.' }) @($environmentFile)
$physical = Get-LatestJson '/physical-acceptance\.json$'
foreach ($physicalGate in @('QuestMRPassthrough','Shared10mArenaAlignment','TwoTrackedPlayersOnLinuxServer','ContactGlove2WallGesture','ContactGlove2OpenPalmShield','MoBitsFiringAndHitPhysicalOutput','InspectorRenderedPlayerAndFreeViews')) {
    Add-ValidationGate $physicalGate $(if ($physical) { 'needs_review' } else { 'pending' }) 'Physical/end-to-end acceptance' 'Requires actual runtime/device evidence at this scope. Mock tests, compilation, headers, network acknowledgements and an alive process cannot establish this gate.' @($physical.Path)
}
$summary = [ordered]@{
    GeneratedUtc = [DateTime]::UtcNow.ToString('o')
    ProjectRoot = $ProjectRoot
    OverallStatus = 'incomplete'
    FullObjectiveVerified = $false
    CompletionDecision = 'This consolidator makes no full-objective completion claim. Inspect the evidence for every gate and perform the goal completion audit.'
    ProcessInspection = $processInspection
    ActiveBuilds = $activeBuilds
    QueuedBuilds = $queuedBuilds
    LatestSourceChange = $(if ($latestSource) { [ordered]@{ Path = $latestSource.FullName; TimestampUtc = $latestSource.LastWriteTimeUtc.ToString('o') } } else { $null })
    EnvironmentCaptureUtc = $(if ($environment) { $environment.Data.TimestampUtc } else { $null })
    LastCapturedQuestDevices = $questDevices
    AndroidFileReadiness = $android
    ExpectedAutomationTests = $expectedAutomation
    Gates = @($gates.ToArray())
    Warnings = @($warnings.ToArray())
}
$json = $summary | ConvertTo-Json -Depth 9
if (-not $NoWrite) {
    $OutputPath = [IO.Path]::GetFullPath($OutputPath)
    New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
    [IO.File]::WriteAllText($OutputPath, $json, (New-Object Text.UTF8Encoding($false)))
}
$json
