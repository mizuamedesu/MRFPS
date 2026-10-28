[CmdletBinding()]
param([string]$EngineRoot, [string]$SdkRoot, [string]$SdkManager, [string]$JavaRoot)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$engine = Resolve-MRFPSEngine $EngineRoot
if (-not $SdkRoot) { $SdkRoot = Join-Path $root '.local\android' }
if (-not $SdkManager) {
    $SdkManager = @(
        (Join-Path $SdkRoot 'cmdline-tools\latest\bin\sdkmanager.bat'),
        'C:\Program Files (x86)\Android\android-sdk\cmdline-tools\12.0\bin\sdkmanager.bat',
        (Join-Path $env:LOCALAPPDATA 'Android\Sdk\cmdline-tools\latest\bin\sdkmanager.bat')
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $SdkManager) { throw 'Install Android SDK command-line tools and supply their sdkmanager.bat path using -SdkManager.' }
$requirements = Get-Content -Raw -LiteralPath (Join-Path $engine 'Engine\Config\Android\Android_SDK.json') | ConvertFrom-Json
$previousJava = $env:JAVA_HOME
try {
    $env:JAVA_HOME = Resolve-MRFPSJava $JavaRoot
    $packages = @('platform-tools', ('platforms;' + $requirements.platforms), ('build-tools;' + $requirements.'build-tools'), ('ndk;' + $requirements.ndk), ('cmake;' + $requirements.cmake))
    Invoke-MRFPSCommand $SdkManager (@("--sdk_root=$SdkRoot") + $packages)
    Write-Host "Android SDK ready at $SdkRoot. Environment changes apply only during build commands."
} finally { $env:JAVA_HOME = $previousJava }
