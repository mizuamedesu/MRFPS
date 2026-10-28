# Automatically contained by the existing Windows resource guard. No server is created.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PrivateKeyFile,
    [Parameter(Mandatory)][string]$PublicKeyFile,
    [Parameter(Mandatory)][ValidatePattern('^[a-f0-9]{64}$')][string]$Fingerprint,
    [string]$Server='172.23.166.33', [ValidateRange(1,65535)][int]$Port=17779,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_.-]{0,62}$')][string]$Container='linux-mrfps-1',
    [string]$Executable, [string]$EngineRoot,
    [ValidateRange(300,600)][int]$StableSeconds=300,
    [ValidateRange(30,600)][int]$StartupTimeoutSeconds=180,
    [string]$ReportDirectory, [string]$PassphraseFile, [switch]$DevUnencrypted,
    [switch]$DryRun, [switch]$InternalWorker
)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root=Get-MRFPSRoot
if(-not $ReportDirectory){$ReportDirectory=Join-Path $root ('Saved/Validation/LinuxMultiplayer-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))}
$ReportDirectory=[IO.Path]::GetFullPath($ReportDirectory)
if(-not $InternalWorker){
    $arguments=@('-InternalWorker','-PrivateKeyFile',([IO.Path]::GetFullPath($PrivateKeyFile)),
        '-PublicKeyFile',([IO.Path]::GetFullPath($PublicKeyFile)),'-Fingerprint',$Fingerprint,
        '-Server',$Server,'-Port',"$Port",'-StableSeconds',"$StableSeconds",'-StartupTimeoutSeconds',"$StartupTimeoutSeconds",
        '-ReportDirectory',$ReportDirectory,'-Container',$Container)
    if($Executable){$arguments+=@('-Executable',([IO.Path]::GetFullPath($Executable)))}
    if($EngineRoot){$arguments+=@('-EngineRoot',$EngineRoot)}
    if($PassphraseFile){$arguments+=@('-PassphraseFile',([IO.Path]::GetFullPath($PassphraseFile)))}
    if($DevUnencrypted){$arguments+='-DevUnencrypted'}
    if($DryRun){$arguments+='-DryRun'}
    & (Join-Path $PSScriptRoot 'Invoke-MRFPSResourceLimited.ps1') -ScriptFile $PSCommandPath -ScriptArguments $arguments -MinimumFreeGB 10 -MaximumCommitPercent 75 -ReportPath (Join-Path ($ReportDirectory+'-guard') 'report.json')
    exit $LASTEXITCODE
}
$packaged=[bool]$Executable
if(-not $Executable){$Executable=Join-Path (Resolve-MRFPSEngine $EngineRoot) 'Engine/Binaries/Win64/UnrealEditor.exe'}
$arguments=@((Join-Path $root 'tools/auth/linux_multiplayer_probe.py'),'--executable',$Executable,
    '--project',(Join-Path $root 'MRFPS.uproject'),'--private-key',([IO.Path]::GetFullPath($PrivateKeyFile)),
    '--public-key',([IO.Path]::GetFullPath($PublicKeyFile)),'--fingerprint',$Fingerprint,
    '--report-directory',$ReportDirectory,'--server',$Server,'--port',"$Port",'--container',$Container,'--stable-seconds',"$StableSeconds",'--startup-timeout',"$StartupTimeoutSeconds")
if($packaged){$arguments+='--packaged'}
if($PassphraseFile){$arguments+=@('--passphrase-file',([IO.Path]::GetFullPath($PassphraseFile)))}
if($DevUnencrypted){$arguments+='--dev-unencrypted'}
if($DryRun){$arguments+='--dry-run'}
Invoke-MRFPSCommand (Join-Path $root '.local/auth-tools/Scripts/python.exe') $arguments
