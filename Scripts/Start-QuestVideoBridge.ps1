# Development-only ADB fallback. It never launches an Android app or UE client.
[CmdletBinding(DefaultParameterSetName='Bounded')]
param(
    [Parameter(Mandatory)][string]$Serial,
    [string]$SdkRoot, [string]$Package = 'org.mrfps.arena',
    [Parameter(ParameterSetName='Bounded')][ValidateRange(0.1,3600)][double]$DurationSeconds = 15,
    [Parameter(Mandatory,ParameterSetName='Watch')][switch]$Watch,
    [ValidateRange(0.1,5)][double]$Fps = 5,
    [ValidateRange(1,1280)][int]$Width = 640,
    [ValidateRange(1,720)][int]$Height = 360,
    [ValidateRange(1,95)][int]$Quality = 75,
    [ValidateSet('both','left','right')][string]$Eye = 'both',
    [ValidateSet('png','raw')][string]$CaptureFormat = 'png',
    [ValidateRange(0.1,3)][double]$CaptureTimeout = 1.5,
    [ValidateRange(0.1,2)][double]$MaxFrameAge = 1.5,
    [string]$HostFrameFile,
    [string]$ReportPath,
    [switch]$InternalWorker
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$sdk = Resolve-MRFPSAndroidSdk $SdkRoot
$python = Join-Path $root '.local\video-tools\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) { throw 'Run Scripts/Initialize-VideoTools.ps1 first.' }
$culture = [Globalization.CultureInfo]::InvariantCulture
if ($HostFrameFile -and -not [IO.Path]::IsPathRooted($HostFrameFile)) {
    throw 'HostFrameFile must be an absolute host path.'
}
if (-not $InternalWorker) {
    $arguments = @('-InternalWorker','-Serial',$Serial,'-SdkRoot',$sdk,'-Package',$Package,
        '-Fps',$Fps.ToString($culture),'-Width',"$Width",'-Height',"$Height",'-Quality',"$Quality",
        '-Eye',$Eye,'-CaptureFormat',$CaptureFormat,'-CaptureTimeout',$CaptureTimeout.ToString($culture),
        '-MaxFrameAge',$MaxFrameAge.ToString($culture))
    if ($Watch) { $arguments += '-Watch' } else { $arguments += @('-DurationSeconds',$DurationSeconds.ToString($culture)) }
    if ($HostFrameFile) { $arguments += @('-HostFrameFile',$HostFrameFile) }
    $guard = @{ ScriptFile=$PSCommandPath; ScriptArguments=$arguments }
    if ($ReportPath) { $guard.ReportPath=$ReportPath }
    & (Join-Path $PSScriptRoot 'Invoke-MRFPSResourceLimited.ps1') @guard
    exit $LASTEXITCODE
}
$arguments = @((Join-Path $root 'tools\video\quest_video_bridge.py'),'--adb',(Join-Path $sdk 'platform-tools\adb.exe'),
    '--serial',$Serial,'--package',$Package,'--fps',$Fps.ToString($culture),'--width',"$Width",'--height',"$Height",
    '--quality',"$Quality",'--eye',$Eye,'--capture-format',$CaptureFormat,
    '--timeout',$CaptureTimeout.ToString($culture),'--max-frame-age',$MaxFrameAge.ToString($culture))
if ($Watch) { $arguments += '--watch' } else { $arguments += @('--duration',$DurationSeconds.ToString($culture)) }
if ($HostFrameFile) { $arguments += @('--host-frame-file',$HostFrameFile) }
Invoke-MRFPSCommand $python $arguments
