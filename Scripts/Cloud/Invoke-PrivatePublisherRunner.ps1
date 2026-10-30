[CmdletBinding()]
param(
    [ValidateSet('Inspect','Publish')][string]$Operation='Inspect',
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{40}$')][string]$SourceCommit,
    [string]$BundleDirectory,
    [string]$ExpectedServerSha256,
    [string]$ReportDirectory,
    [switch]$InternalWorker,
    [ValidatePattern('^[0-9a-f]{16}$')][string]$TaskId,
    [string]$GuardReport
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$repository='mizuamedesu/MRFPS-Server-Registry'
function Get-WslPath([string]$Path){
    $value=(& wsl -d Ubuntu -u root --exec wslpath -a $Path.Replace('\','/')|Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or $value -notmatch '^/[A-Za-z0-9._/-]+$'){throw 'RUNNER_WSL_PATH_INVALID'}
    return $value
}
function Save-RunnerJson([string]$Path,$Value){
    $temporary=$Path+'.incoming'
    [IO.File]::WriteAllText($temporary,($Value|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
    if(Test-Path -LiteralPath $Path){[IO.File]::Replace($temporary,$Path,[NullString]::Value)}else{[IO.File]::Move($temporary,$Path)}
}
if($InternalWorker){
    if(-not $TaskId -or -not $GuardReport){throw 'OWNED_RUNNER_WORKER_ARGUMENTS_REQUIRED'}
    $cache=Join-Path $root '.local/cloud-runner-tools'
    New-Item -ItemType Directory -Force -Path $cache|Out-Null
    $assets=@(
        @{name='actions-runner-linux-x64-2.338.2.tar.gz';url='https://github.com/actions/runner/releases/download/v2.338.2/actions-runner-linux-x64-2.338.2.tar.gz';sha='baebf204307bac6193480852732f0aba2ae97b6c5597c244f4d5558390147c40'},
        @{name='gh_2.103.3_linux_amd64.tar.gz';url='https://github.com/cli/cli/releases/download/v2.103.3/gh_2.103.3_linux_amd64.tar.gz';sha='4c806978ffce1cec70853302a56d70ac6da4f19bae7adca4e1a8ab00633dd91d'}
    )
    foreach($asset in $assets){
        $destination=Join-Path $cache $asset.name
        if(-not (Test-Path -LiteralPath $destination)){
            & curl.exe --fail --location --silent --show-error --connect-timeout 15 --max-time 240 --output ($destination+'.incoming') $asset.url
            if($LASTEXITCODE -ne 0){throw 'OFFICIAL_RUNNER_DOWNLOAD_FAILED'}
            if((Get-FileHash -LiteralPath ($destination+'.incoming') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $asset.sha){throw 'OFFICIAL_RUNNER_DOWNLOAD_DIGEST_MISMATCH'}
            Move-Item -LiteralPath ($destination+'.incoming') -Destination $destination
        }
        if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $asset.sha){throw 'OFFICIAL_RUNNER_CACHE_DIGEST_MISMATCH'}
    }
    # The classic local credential never enters WSL. Only a repository-scoped,
    # one-hour registration token is transported on this private stdin pipe.
    $registration=(& gh api -X POST "repos/$repository/actions/runners/registration-token"|Out-String)|ConvertFrom-Json
    if($LASTEXITCODE -ne 0 -or -not $registration.token){throw 'EPHEMERAL_RUNNER_REGISTRATION_TOKEN_FAILED'}
    $script=Get-WslPath (Join-Path $root 'tools/cloud/ephemeral_runner.py')
    $guard=Get-WslPath $GuardReport
    $runnerArchive=Get-WslPath (Join-Path $cache $assets[0].name)
    $ghArchive=Get-WslPath (Join-Path $cache $assets[1].name)
    $start=New-Object Diagnostics.ProcessStartInfo
    $start.FileName=(Get-Command wsl.exe).Source
    $inspection=if($Operation -eq 'Inspect'){' --inspect-only'}else{''}
    $start.Arguments="-d Ubuntu -u root --exec systemd-run --quiet --wait --pipe --collect --unit=mrfps-private-manager-$TaskId --property=MemoryMax=1024M --property=MemorySwapMax=0 --property=CPUQuota=200% --property=RuntimeMaxSec=1800 --property=KillMode=control-group python3 $script run --task $TaskId --guard $guard --runner-archive $runnerArchive --gh-archive $ghArchive$inspection"
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardInput=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $process=New-Object Diagnostics.Process
    $process.StartInfo=$start
    if(-not $process.Start()){throw 'OWNED_RUNNER_PROCESS_START_FAILED'}
    $process.StandardInput.WriteLine((@{token=$registration.token}|ConvertTo-Json -Compress))
    $process.StandardInput.Close()
    $registration=$null
    $stdoutTask=$process.StandardOutput.ReadToEndAsync()
    $stderrTask=$process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    # The Linux manager emits fixed errors/metadata only, never runner logs.
    $stdoutTask.Result|Set-Content -LiteralPath (Join-Path $ReportDirectory linux-manager.stdout.log) -Encoding UTF8
    $stderrTask.Result|Set-Content -LiteralPath (Join-Path $ReportDirectory linux-manager.stderr.log) -Encoding UTF8
    exit $process.ExitCode
}
if(-not $ReportDirectory){$ReportDirectory=Join-Path $root ('Saved/Validation/PrivatePublisher-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))}
$ReportDirectory=[IO.Path]::GetFullPath($ReportDirectory)
if(Test-Path -LiteralPath $ReportDirectory){throw 'PRIVATE_RUNNER_REPORT_ALREADY_EXISTS'}
New-Item -ItemType Directory -Path $ReportDirectory|Out-Null
$TaskId=[Guid]::NewGuid().ToString('N').Substring(0,16)
$label='mrfps-private-'+$TaskId
$GuardReport=Join-Path $ReportDirectory guard/report.json
$record=[ordered]@{repository=$repository;taskId=$TaskId;runnerLabel=$label;operation=$Operation;startedUtc=[DateTime]::UtcNow.ToString('o');workflowRunId=$null;guardExit=$null;cleanup=$null;registrationRemoved=$false;otherContainersChanged=$false;status='preparing'}
$publisher=(& gh api "repos/$repository"|Out-String)|ConvertFrom-Json
if($LASTEXITCODE -ne 0 -or $publisher.id -ne 1438751425 -or $publisher.private -ne $true -or $publisher.owner.login -ne 'mizuamedesu' -or $publisher.description -ne 'MRFPS private server artifact publisher (managed by MRFPS cloud-deploy)'){throw 'DEDICATED_PRIVATE_PUBLISHER_IDENTITY_MISMATCH'}
$initialRunners=(& gh api "repos/$repository/actions/runners?per_page=100"|Out-String)|ConvertFrom-Json
if($LASTEXITCODE -ne 0 -or @($initialRunners.runners|Where-Object {$_.name -eq $label}).Count -ne 0){throw 'OWNED_RUNNER_NAME_COLLISION'}
$record.runnerNameInitiallyAbsent=$true
try{
    if($Operation -eq 'Inspect'){
        & gh workflow run publish-linux-server.yml --repo $repository --ref main -f "operation=$Operation" -f "runner_label=$label" -f "source_commit=$SourceCommit"
        if($LASTEXITCODE -ne 0){throw 'PRIVATE_RUNNER_INSPECTION_DISPATCH_FAILED'}
    }else{
        & (Join-Path $PSScriptRoot Publish-GhcrServer.ps1) -Repository $repository -BundleDirectory $BundleDirectory -ExpectedServerSha256 $ExpectedServerSha256 -SourceCommit $SourceCommit -RunnerLabel $label -Mode Publish
        if($LASTEXITCODE -ne 0){throw 'PRIVATE_PUBLISHER_DISPATCH_FAILED'}
    }
    for($attempt=0;$attempt -lt 8;$attempt++){
        $runs=(& gh run list --repo $repository --workflow publish-linux-server.yml --limit 20 --json databaseId,displayTitle,status|Out-String)|ConvertFrom-Json
        if($LASTEXITCODE -ne 0){throw 'PRIVATE_RUNNER_RUN_QUERY_FAILED'}
        $matching=@($runs|Where-Object {$_.displayTitle -eq "$Operation $label"})
        if($matching.Count -eq 1){$record.workflowRunId=$matching[0].databaseId;break}
        if($matching.Count -gt 1){throw 'PRIVATE_RUNNER_RUN_AMBIGUOUS'}
        Start-Sleep -Milliseconds 750
    }
    if(-not $record.workflowRunId){throw 'PRIVATE_RUNNER_DISPATCHED_RUN_NOT_VISIBLE'}
    $record.status='running';Save-RunnerJson (Join-Path $ReportDirectory report.json) $record
    & (Join-Path $root 'Scripts/Invoke-MRFPSResourceLimited.ps1') -ScriptFile $PSCommandPath -ScriptArguments @('-InternalWorker','-Operation',$Operation,'-SourceCommit',$SourceCommit,'-TaskId',$TaskId,'-GuardReport',$GuardReport,'-ReportDirectory',$ReportDirectory) -MinimumFreeGB 10 -MaximumCommitPercent 75 -ReportPath $GuardReport
    $record.guardExit=$LASTEXITCODE
    $record.status=if($LASTEXITCODE -eq 0){'runner_completed'}else{'guard_or_runner_failed'}
}finally{
    # Runs outside the Windows job: explicit Linux cleanup complements the
    # independent Linux watchdog if Windows job termination killed wsl.exe.
    $cleanupScript=Get-WslPath (Join-Path $root 'tools/cloud/ephemeral_runner.py')
    $cleanup=(& wsl -d Ubuntu -u root --exec python3 $cleanupScript cleanup --task $TaskId|Out-String)
    if($LASTEXITCODE -eq 0){$record.cleanup=$cleanup|ConvertFrom-Json}
    $runners=(& gh api "repos/$repository/actions/runners?per_page=100"|Out-String)|ConvertFrom-Json
    if($LASTEXITCODE -eq 0){
        # The API may omit `ephemeral`; the random name was claimed absent
        # before this task, and config.sh never uses --replace/adopts a runner.
        $owned=@($runners.runners|Where-Object {$_.name -eq $label -and $record.runnerNameInitiallyAbsent})
        foreach($runner in $owned){& gh api -X DELETE "repos/$repository/actions/runners/$($runner.id)"|Out-Null}
        $after=(& gh api "repos/$repository/actions/runners?per_page=100"|Out-String)|ConvertFrom-Json
        $record.registrationRemoved=($LASTEXITCODE -eq 0 -and @($after.runners|Where-Object {$_.name -eq $label}).Count -eq 0)
    }
    if($record.workflowRunId){
        $run=(& gh run view $record.workflowRunId --repo $repository --json status,conclusion|Out-String)|ConvertFrom-Json
        if($run.status -ne 'completed'){
            & gh run cancel $record.workflowRunId --repo $repository|Out-Null
        }
        $record.workflow=$run
    }
    $record.finishedUtc=[DateTime]::UtcNow.ToString('o')
    $record.passed=($record.guardExit -eq 0 -and $record.registrationRemoved -and $record.cleanup.passed -eq $true -and $record.workflow.status -eq 'completed' -and $record.workflow.conclusion -eq 'success')
    if(-not $record.passed){$record.status='validation_failed'}
    Save-RunnerJson (Join-Path $ReportDirectory report.json) $record
    $record|ConvertTo-Json -Depth 8
}
if(-not $record.passed){exit 1}
