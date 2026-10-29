[CmdletBinding()]
param(
    [string]$PackageRoot,
    [ValidatePattern('^[0-9a-f]{64}$')][string]$ExpectedServerSha256,
    [string]$PublicKeyFile,
    [string]$ReportDirectory,
    [ValidateRange(1024,65535)][int]$HostPort=17780,
    [switch]$StartValidation,
    [switch]$ProbeOnly,
    [string]$ProbeBaseImage='mrfps-server:stage-p12',
    [ValidatePattern('^[a-z0-9][a-z0-9._/:@-]+$')][string]$ReuseBaseImage,
    [switch]$InternalWorker
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if(-not $ReportDirectory){$ReportDirectory=Join-Path $root ('Saved/Validation/CloudImage-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))}
$ReportDirectory=[IO.Path]::GetFullPath($ReportDirectory)
if(-not $InternalWorker){
    $forward=@('-InternalWorker','-ReportDirectory',$ReportDirectory,'-HostPort',"$HostPort")
    if($ProbeOnly){$forward+=@('-ProbeOnly','-ProbeBaseImage',$ProbeBaseImage)}elseif($ReuseBaseImage){$forward+=@('-ReuseBaseImage',$ReuseBaseImage,'-ExpectedServerSha256',$ExpectedServerSha256)}else{$forward+=@('-PackageRoot',$PackageRoot,'-ExpectedServerSha256',$ExpectedServerSha256)}
    if($PublicKeyFile){$forward+=@('-PublicKeyFile',$PublicKeyFile)}
    if($StartValidation){$forward+='-StartValidation'}
    & (Join-Path $root 'Scripts/Invoke-MRFPSResourceLimited.ps1') -ScriptFile $PSCommandPath -ScriptArguments $forward -MinimumFreeGB 10 -MaximumCommitPercent 75 -ReportPath (Join-Path ($ReportDirectory+'-guard') report.json)
    exit $LASTEXITCODE
}
if(Test-Path -LiteralPath $ReportDirectory){throw 'CLOUD_IMAGE_REPORT_ALREADY_EXISTS'}
New-Item -ItemType Directory -Path $ReportDirectory | Out-Null
Import-Module (Join-Path $root 'Scripts/MRFPS.Tools.psm1') -Force
$wsl=(Get-Command wsl.exe -ErrorAction Stop).Source
$python=Join-Path $root '.local/auth-tools/Scripts/python.exe'
if(-not (Test-Path -LiteralPath $python)){throw 'LOCAL_PYTHON_REQUIRED'}
function Invoke-Docker([string[]]$Arguments,[string]$LogName){
    Invoke-MRFPSCommand $wsl (@('-d','Ubuntu','-u','root','--exec','docker')+$Arguments) -LogPath (Join-Path $ReportDirectory $LogName)
}
function Get-WslPath([string]$Path){
    $mapped=(& $wsl -d Ubuntu --exec wslpath -a $Path.Replace('\','/') | Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or -not $mapped){throw 'CLOUD_WSL_PATH_FAILED'}
    return $mapped
}
$base=$ProbeBaseImage
$tag='mrfps-cloud:probe-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
$manifest=$null
if($ReuseBaseImage){
    if(-not $ExpectedServerSha256){throw 'APPROVED_SERVER_HASH_REQUIRED'}
    $baseInfo=(& $wsl -d Ubuntu -u root --exec docker image inspect $ReuseBaseImage|Out-String)|ConvertFrom-Json
    if($LASTEXITCODE -ne 0 -or $baseInfo[0].Config.Labels.'app.mrfps.server.sha256' -ne $ExpectedServerSha256){throw 'REUSED_BASE_SERVER_HASH_MISMATCH'}
    $base=$ReuseBaseImage
    $tag='mrfps-cloud:health-'+$ExpectedServerSha256.Substring(0,12)
}elseif(-not $ProbeOnly){
    if(-not $PackageRoot -or -not $ExpectedServerSha256){throw 'FRESH_PACKAGE_AND_APPROVED_SERVER_HASH_REQUIRED'}
    $commit=(& git -C $root rev-parse HEAD | Out-String).Trim()
    $bundle=Join-Path $ReportDirectory bundle
    Invoke-MRFPSCommand $python @((Join-Path $root 'tools/cloud/server_bundle.py'),'bundle','--package',$PackageRoot,'--server-sha256',$ExpectedServerSha256,'--source-commit',$commit,'--output',$bundle) -LogPath (Join-Path $ReportDirectory bundle.log)
    $manifest=Get-Content (Join-Path $bundle server-artifact.json) -Raw|ConvertFrom-Json
    $context=Join-Path $ReportDirectory context
    New-Item -ItemType Directory -Path $context | Out-Null
    Invoke-MRFPSCommand $python @((Join-Path $root 'tools/cloud/server_bundle.py'),'verify','--archive',(Join-Path $bundle linux-server.tar.gz),'--bundle-sha256',$manifest.bundle_sha256,'--server-sha256',$ExpectedServerSha256,'--destination',$context) -LogPath (Join-Path $ReportDirectory verify.log)
    $deploy=Join-Path $context Deploy/Linux
    New-Item -ItemType Directory -Path $deploy -Force|Out-Null
    foreach($name in @('Dockerfile','Dockerfile.dockerignore','entrypoint.sh','auth_config.py','healthcheck.py')){Copy-Item -LiteralPath (Join-Path $root "Deploy/Linux/$name") -Destination (Join-Path $deploy $name)}
    Copy-Item -LiteralPath (Join-Path $root '.dockerignore') -Destination (Join-Path $context '.dockerignore')
    $base='mrfps-cloud-base:'+ $ExpectedServerSha256.Substring(0,12)
    $mapped=Get-WslPath $context
    Invoke-Docker @('build','--label',"app.mrfps.server.sha256=$ExpectedServerSha256",'-f',"$mapped/Deploy/Linux/Dockerfile",'-t',$base,$mapped) base-build.log
    $tag='mrfps-cloud:'+ $manifest.image_tag
}
$cloud=Get-WslPath (Join-Path $root 'Deploy/Cloud')
Invoke-Docker @('build','--network=none','-f',"$cloud/Flash.Dockerfile",'--build-arg',"MRFPS_BASE_IMAGE=$base",'-t',$tag,$cloud) flash-build.log
$negative=@()
foreach($uid in @(10001,0)){
    $oldPreference=$ErrorActionPreference;$ErrorActionPreference='Continue'
    $lines=& $wsl -d Ubuntu -u root --exec docker run --rm --user $uid --memory 128m --memory-swap 128m --cpus 1 --security-opt no-new-privileges:true $tag 2>&1
    $code=$LASTEXITCODE;$ErrorActionPreference=$oldPreference
    $text=($lines|ForEach-Object {$_.ToString()}) -join "`n"
    $text|Set-Content (Join-Path $ReportDirectory "negative-uid$uid.log") -Encoding UTF8
    $ok=$code -eq 78 -and $text -match 'MRFPS_FLASH_RUNTIME uid=10001 capabilities=0+' -and $text -match '(?m)^AUTH_CONFIG:'
    $negative+=@{requestedUid=$uid;exitCode=$code;passed=$ok;gameStarted=$false}
    if(-not $ok){throw 'CLOUD_FLASH_UID_OR_FAIL_CLOSED_CHECK_FAILED'}
}
$report=[ordered]@{timestampUtc=[DateTime]::UtcNow.ToString('o');image=$tag;baseImage=$base;probeOnly=[bool]$ProbeOnly;uidNegativeChecks=$negative;serverSha256=$ExpectedServerSha256;containerStarted=$false;publicPort7777Satisfied=$false;registryPushed=$false}
if($StartValidation){
    if($ProbeOnly -or -not $PublicKeyFile){throw 'APPROVED_PACKAGE_AND_PUBLIC_KEY_REQUIRED_FOR_GAME_START'}
    $keyBytes=[IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $PublicKeyFile))
    $keyText=[Text.Encoding]::ASCII.GetString($keyBytes)
    if($keyText -notmatch '^-----BEGIN PUBLIC KEY-----' -or $keyBytes.Where({$_ -gt 127 -or $_ -eq 0}).Count){throw 'PUBLIC_KEY_FILE_INVALID'}
    $b64=[Convert]::ToBase64String($keyBytes)
    $name='mrfps-cloud-'+$(if($ReuseBaseImage){'hc-'}else{''})+$ExpectedServerSha256.Substring(0,12)
    # Match Flash's initial root UID; the image launcher then drops every cap.
    # A distinct name/port preserves the existing p12 container.
    Invoke-Docker @('run','-d','--name',$name,'--memory','1g','--memory-swap','1g','--cpus','2','--user','0:0','--cap-drop','NET_RAW','--read-only','--security-opt','no-new-privileges:true','--restart','no','--tmpfs','/tmp:rw,noexec,nosuid,size=64m','--tmpfs','/home/mrfps:rw,nosuid,size=16m,uid=10001,gid=10001,mode=0700','--tmpfs','/opt/mrfps/MRFPS/Saved:rw,nosuid,size=64m,uid=10001,gid=10001,mode=0700','-p',"${HostPort}:7777/udp",'-e',"MRFPS_AUTH_PUBLIC_KEY_B64=$b64",'-e','MRFPS_AUTH_AUDIENCE=mrfps',$tag) game-start.log
    $report.containerStarted=$true;$report.containerName=$name;$report.hostPort=$HostPort
    $address=(& $wsl -d Ubuntu --exec hostname -I | Out-String).Trim().Split(' ')[0]
    $report.endpoint="${address}:$HostPort"
    for($i=0;$i -lt 30;$i++){
        $inspect=(& $wsl -d Ubuntu -u root --exec docker inspect $name | Out-String)|ConvertFrom-Json
        $logs=(& $wsl -d Ubuntu -u root --exec docker logs --tail 300 $name 2>&1 | ForEach-Object {$_.ToString()}) -join "`n"
        $fp=[regex]::Match($logs,'MRFPS_AUTH_TRANSPORT_READY fingerprint=([a-f0-9]{64})')
        if($inspect[0].State.Health.Status -eq 'healthy' -and $fp.Success){
            $report.containerId=$inspect[0].Id;$report.startedAt=$inspect[0].State.StartedAt;$report.health='healthy';$report.dtlsFingerprint=$fp.Groups[1].Value
            $report.memoryBytes=$inspect[0].HostConfig.Memory;$report.memorySwapBytes=$inspect[0].HostConfig.MemorySwap;$report.nanoCpus=$inspect[0].HostConfig.NanoCpus
            $logs|Set-Content (Join-Path $ReportDirectory server.log) -Encoding UTF8
            break
        }
        if(-not $inspect[0].State.Running){throw 'CLOUD_VALIDATION_SERVER_EXITED'}
        Start-Sleep -Seconds 2
    }
    if(-not $report.Contains('dtlsFingerprint')){throw 'CLOUD_VALIDATION_HEALTH_OR_FINGERPRINT_TIMEOUT'}
}
$report|ConvertTo-Json -Depth 6|Set-Content (Join-Path $ReportDirectory report.json) -Encoding UTF8
$report|ConvertTo-Json -Depth 6
