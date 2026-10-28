[CmdletBinding()]
param([string]$EngineRoot, [string]$SdkRoot, [string]$JavaRoot, [string]$Serial, [switch]$RequireQuest, [string]$ReportPath, [string]$WslDistribution = 'Ubuntu')
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$result = [ordered]@{ TimestampUtc = [DateTime]::UtcNow.ToString('o'); ProjectRoot = $root; Engine = $null; Android = $null; Quest = @(); Docker = $null; Errors = @() }
try {
    $engine = Resolve-MRFPSEngine $EngineRoot
    $version = Get-Content -Raw -LiteralPath (Join-Path $engine 'Engine\Build\Build.version') | ConvertFrom-Json
    $result.Engine = [ordered]@{ Root = $engine; Version = "$($version.MajorVersion).$($version.MinorVersion).$($version.PatchVersion)"; Editor = (Test-Path -LiteralPath (Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor.exe')); LinuxToolchain = $env:LINUX_MULTIARCH_ROOT }
} catch { $result.Errors += $_.Exception.Message }
try {
    $sdk = Resolve-MRFPSAndroidSdk $SdkRoot
    $java = Resolve-MRFPSJava $JavaRoot
    $requirements = Get-Content -Raw -LiteralPath (Join-Path $engine 'Engine\Config\Android\Android_SDK.json') | ConvertFrom-Json
    $result.Android = [ordered]@{ Sdk = $sdk; Java = $java; RequiredNdk = $requirements.ndk; NdkInstalled = (Test-Path -LiteralPath (Join-Path $sdk ('ndk\' + $requirements.ndk + '\source.properties'))); RequiredBuildTools = $requirements.'build-tools'; BuildToolsInstalled = (Test-Path -LiteralPath (Join-Path $sdk ('build-tools\' + $requirements.'build-tools' + '\aapt2.exe'))) }
    $adb = Join-Path $sdk 'platform-tools\adb.exe'
    $result.Quest = @(Get-MRFPSQuest -Adb $adb -Serial $Serial -AllowUnavailable | ForEach-Object {
        $device = $_
        $info = [ordered]@{ Serial = $device.Serial; State = $device.State; Details = $device.Details; Model = $null; Android = $null; Abi = $null }
        if ($device.State -eq 'device') {
            $info.Model = (& $adb -s $device.Serial shell getprop ro.product.model | Out-String).Trim()
            $info.Android = (& $adb -s $device.Serial shell getprop ro.build.version.release | Out-String).Trim()
            $info.Abi = (& $adb -s $device.Serial shell getprop ro.product.cpu.abi | Out-String).Trim()
        }
        [PSCustomObject]$info
    })
} catch { $result.Errors += $_.Exception.Message }
$docker = Get-Command docker -ErrorAction SilentlyContinue
if ($docker) {
    $dockerInfo = & $docker.Source info --format '{{.ServerVersion}}' 2>&1
    $result.Docker = [ordered]@{ Command = $docker.Source; Reachable = ($LASTEXITCODE -eq 0); VersionOrError = ($dockerInfo | Out-String).Trim() }
} else {
    $wsl = Get-Command wsl -ErrorAction SilentlyContinue
    if ($wsl) {
        $dockerInfo = & $wsl.Source -d $WslDistribution -u root -- docker info --format '{{.ServerVersion}}' 2>&1
        $result.Docker = [ordered]@{ Command = "wsl -d $WslDistribution -u root -- docker"; Reachable = ($LASTEXITCODE -eq 0); VersionOrError = ($dockerInfo | Out-String).Trim() }
    } else {
        $result.Docker = [ordered]@{ Command = $null; Reachable = $false; VersionOrError = 'Docker CLI is unavailable.' }
    }
}
if ($RequireQuest -and @($result.Quest | Where-Object State -EQ 'device').Count -ne 1) { $result.Errors += 'An authorized Quest is required; accept the USB debugging RSA prompt inside the headset.' }
if (-not $ReportPath) { $ReportPath = Join-Path $root 'Saved\Validation\environment.json' }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ReportPath) | Out-Null
$json = $result | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText($ReportPath, $json, (New-Object Text.UTF8Encoding($false)))
$json
if ($result.Errors.Count -gt 0) { exit 1 }
