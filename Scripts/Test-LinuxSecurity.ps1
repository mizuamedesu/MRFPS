# Invoke with Scripts/Invoke-MRFPSResourceLimited.ps1 (10 GiB / 75 percent).
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PrivateKeyFile,
    [Parameter(Mandatory)][string]$PublicKeyFile,
    [Parameter(Mandatory)][ValidatePattern('^[a-f0-9]{64}$')][string]$Fingerprint,
    [Parameter(Mandatory)][string]$ReportDirectory,
    [string]$Server='172.23.166.33', [int]$Port=17779,
    [string]$Executable, [string]$EngineRoot,
    [ValidateSet('lifecycle','rejections','all')][string]$Phase='lifecycle',
    [switch]$LegacyProtocol2,
    [string]$PassphraseFile,
    [switch]$DevUnencrypted,
    [switch]$DryRun
)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root=Get-MRFPSRoot
$python=Join-Path $root '.local/auth-tools/Scripts/python.exe'
$packaged=[bool]$Executable
if(-not $Executable){$Executable=Join-Path (Resolve-MRFPSEngine $EngineRoot) 'Engine/Binaries/Win64/UnrealEditor.exe'}
$arguments=@((Join-Path $root 'tools/auth/linux_security_probe.py'),'--executable',$executable,
    '--project',(Join-Path $root 'MRFPS.uproject'),'--private-key',([IO.Path]::GetFullPath($PrivateKeyFile)),
    '--public-key',([IO.Path]::GetFullPath($PublicKeyFile)),'--fingerprint',$Fingerprint,
    '--report-directory',([IO.Path]::GetFullPath($ReportDirectory)),'--server',$Server,'--port',"$Port",'--phase',$Phase)
if($LegacyProtocol2){$arguments+='--legacy-protocol2'}
if($packaged){$arguments+='--packaged'}
if($PassphraseFile){$arguments+=@('--passphrase-file',([IO.Path]::GetFullPath($PassphraseFile)))}
if($DevUnencrypted){$arguments+='--dev-unencrypted'}
if($DryRun){$arguments+='--dry-run'}
Invoke-MRFPSCommand $python $arguments
