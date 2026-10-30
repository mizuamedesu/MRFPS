[CmdletBinding()]
param([string]$WslDistribution = 'Ubuntu', [switch]$Build, [switch]$Stop, [switch]$Status,
    [string]$PublicKeyFile, [string]$Audience = 'mrfps', [ValidateRange(1,65535)][int]$HostPort = 7777,
    [ValidatePattern('^(?:[a-z0-9][a-z0-9_-]{0,62})?$')][string]$ProjectName='')
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$compose = Join-Path $root 'Deploy\Linux\compose.yaml'
$keepaliveFile = Join-Path $root 'Saved\Runtime\LinuxContainer-WslKeepalive.json'
$docker = Get-Command docker -ErrorAction SilentlyContinue
$arguments = @('compose')
if ($ProjectName) { $arguments+=@('--project-name',$ProjectName) }
$arguments+=@('-f',$compose)
$publicEncoded = ''
if (-not $Status -and -not $Stop) {
    if ($PublicKeyFile) {
        $publicPath = Resolve-MRFPSCredentialFile $PublicKeyFile -Kind PublicKey
        $publicEncoded = [Convert]::ToBase64String([IO.File]::ReadAllBytes($publicPath))
    } else {
        $encoded = [Environment]::GetEnvironmentVariable('MRFPS_AUTH_PUBLIC_KEY_B64','Process')
        $literal = [Environment]::GetEnvironmentVariable('MRFPS_AUTH_PUBLIC_KEY_PEM','Process')
        if ([bool]$encoded -eq [bool]$literal) { throw 'Set exactly one public-key environment value or supply -PublicKeyFile. Issuer private keys are never accepted.' }
        $publicEncoded = if ($encoded) { $encoded } else { [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($literal)) }
    }
    if ($publicEncoded -notmatch '^[A-Za-z0-9+/=]+$' -or $Audience -notmatch '^[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}$') { throw 'Invalid public-key encoding or audience.' }
}
if ($Status) { $arguments += 'ps' }
elseif ($Stop) { $arguments += 'down' }
else {
    if (-not (Test-Path -LiteralPath (Join-Path $root 'Artifacts\Packages\LinuxServer\MRFPS\Binaries\Linux'))) {
        throw 'Packaged Linux server is missing. Run Scripts/Build-MRFPS.ps1 -Target LinuxServer first.'
    }
    $arguments += @('up','-d','--wait','--wait-timeout','180')
    if ($Build) { $arguments += '--build' }
}
if ($docker) {
    $prior = @{}
    try {
        if ($publicEncoded) {
            foreach ($name in @('MRFPS_AUTH_PUBLIC_KEY_B64','MRFPS_AUTH_PUBLIC_KEY_PEM','MRFPS_AUTH_AUDIENCE','MRFPS_HOST_PORT')) { $prior[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
            $env:MRFPS_AUTH_PUBLIC_KEY_B64 = $publicEncoded
            $env:MRFPS_AUTH_PUBLIC_KEY_PEM = $null
            $env:MRFPS_AUTH_AUDIENCE = $Audience
            $env:MRFPS_HOST_PORT = "$HostPort"
        }
        Invoke-MRFPSCommand $docker.Source $arguments
    } finally { foreach ($name in $prior.Keys) { [Environment]::SetEnvironmentVariable($name,$prior[$name],'Process') } }
}
else {
    $wsl = (Get-Command wsl -ErrorAction Stop).Source
    if ($WslDistribution -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') { throw 'Use a simple WSL distribution name.' }
    $managedKeepalive = $null
    if (Test-Path -LiteralPath $keepaliveFile) {
        $record = Get-Content -Raw -LiteralPath $keepaliveFile | ConvertFrom-Json
        $candidate = Get-CimInstance Win32_Process -Filter "ProcessId=$([int]$record.ProcessId)" -ErrorAction SilentlyContinue
        if ($candidate -and $candidate.Name -eq 'wsl.exe' -and $record.Distribution -eq $WslDistribution -and $candidate.CommandLine -match "-d $([regex]::Escape($WslDistribution)) --exec /bin/sleep infinity" -and [Math]::Abs(($candidate.CreationDate.ToUniversalTime() - [DateTime]::Parse($record.CreatedUtc).ToUniversalTime()).TotalSeconds) -lt 1) {
            $managedKeepalive = $candidate
        }
    }
    if (-not $Status -and -not $Stop -and -not $managedKeepalive) {
        # systemd services alone do not keep a WSL distribution alive.
        $helper = Start-Process -FilePath $wsl -ArgumentList @('-d',$WslDistribution,'--exec','/bin/sleep','infinity') -WindowStyle Hidden -PassThru
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $keepaliveFile) | Out-Null
        [ordered]@{ ProcessId=$helper.Id; CreatedUtc=$helper.StartTime.ToUniversalTime().ToString('o'); Distribution=$WslDistribution; Purpose='Managed game-container WSL keepalive' } | ConvertTo-Json | Out-File -LiteralPath $keepaliveFile -Encoding utf8
    }
    for ($composeIndex=0; $composeIndex -lt $arguments.Count-1; $composeIndex++) {
        if ($arguments[$composeIndex] -ne '-f') { continue }
        $wslCompose = (& $wsl -d $WslDistribution -- wslpath -a $arguments[$composeIndex+1].Replace('\','/') | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or -not $wslCompose) { throw 'Cannot map a compose path into WSL.' }
        $arguments[$composeIndex+1] = $wslCompose
        $composeIndex++
    }
    $prefix = @('-d',$WslDistribution,'-u','root','--')
    if ($publicEncoded) { $prefix += @('env',"MRFPS_AUTH_PUBLIC_KEY_B64=$publicEncoded",'MRFPS_AUTH_PUBLIC_KEY_PEM=',"MRFPS_AUTH_AUDIENCE=$Audience","MRFPS_HOST_PORT=$HostPort") }
    Invoke-MRFPSCommand $wsl ($prefix + @('docker') + $arguments)
    if ($Stop -and $managedKeepalive -and -not $ProjectName) {
        Stop-Process -Id $managedKeepalive.ProcessId -ErrorAction Stop
        Remove-Item -LiteralPath $keepaliveFile -Force
    }
}
