[CmdletBinding()]
param([string]$ReportDirectory,[switch]$InternalWorker)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $ReportDirectory){$ReportDirectory=Join-Path $root ('Saved/Validation/ConsentLifecycle-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))}
$ReportDirectory=[IO.Path]::GetFullPath($ReportDirectory)
if(-not $InternalWorker){
    & (Join-Path $PSScriptRoot 'Invoke-MRFPSResourceLimited.ps1') -ScriptFile $PSCommandPath -ScriptArguments @('-InternalWorker','-ReportDirectory',$ReportDirectory) -MinimumFreeGB 10 -MaximumCommitPercent 75 -ReportPath (Join-Path ($ReportDirectory+'-guard') report.json)
    exit $LASTEXITCODE
}
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
if(Test-Path -LiteralPath $ReportDirectory){throw 'CONSENT_TEST_REPORT_ALREADY_EXISTS'}
New-Item -ItemType Directory -Path $ReportDirectory|Out-Null
$vcvars='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
if(-not (Test-Path -LiteralPath $vcvars)){throw 'CONSENT_TEST_MSVC_ENVIRONMENT_MISSING'}
$source=Join-Path $root 'tools/validation/consent_lifecycle_test.cpp'
$exe=Join-Path $ReportDirectory consent-lifecycle.exe
$obj=Join-Path $ReportDirectory consent-lifecycle.obj
$command='call "'+$vcvars+'" >nul && cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$source+'"'
Invoke-MRFPSCommand $env:ComSpec @('/d','/s','/c',$command) -LogPath (Join-Path $ReportDirectory compile.log)
Invoke-MRFPSCommand $exe @() -LogPath (Join-Path $ReportDirectory execution.log)
$text=Get-Content (Join-Path $ReportDirectory execution.log) -Raw
$match=[regex]::Match($text,'CONSENT_LIFECYCLE_PASS checks=(\d+)')
if(-not $match.Success){throw 'CONSENT_POLICY_PASS_MARKER_MISSING'}
$policyHashes=[ordered]@{}
foreach($relative in @('Source/MRFPS/Public/MRFPSConsentPolicy.h','Source/MRFPS/Public/MRFPSVideoControlPolicy.h','Plugins/MRFPSAuth/Source/MRFPSAuth/Public/MRFPSAuthClientUsePolicy.h')){
    $policyHashes[$relative]=(Get-FileHash -Algorithm SHA256 (Join-Path $root $relative)).Hash.ToLowerInvariant()
}
[ordered]@{TimestampUtc=[DateTime]::UtcNow.ToString('o');Passed=$true;Scope='Actual portable consent/reconnect policies, compiled with MSVC; UE integration and physical headset not exercised';Checks=[int]$match.Groups[1].Value;UEStarted=$false;DeviceUsed=$false;ServerTouched=$false;SigningKeyRead=$false;HeavyBuildRun=$false;SourceSHA256=(Get-FileHash -Algorithm SHA256 $source).Hash.ToLowerInvariant();PolicySHA256=$policyHashes}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $ReportDirectory report.json) -Encoding UTF8
