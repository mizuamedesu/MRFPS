Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-MRFPSRoot { [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')) }

function Resolve-MRFPSEngine {
    param([string]$EngineRoot)
    $candidates = @($EngineRoot, $env:MRFPS_UE_ROOT, 'C:\UnrealEngine\UE5.8')
    $key = Get-ItemProperty 'HKCU:\Software\Epic Games\Unreal Engine\Builds' -ErrorAction SilentlyContinue
    if ($key) { $candidates += @($key.PSObject.Properties | Where-Object Name -NotMatch '^PS' | ForEach-Object Value) }
    foreach ($candidate in $candidates) {
        if (-not $candidate) { continue }
        $versionFile = Join-Path $candidate 'Engine\Build\Build.version'
        if (-not (Test-Path -LiteralPath $versionFile)) { continue }
        $version = Get-Content -Raw -LiteralPath $versionFile | ConvertFrom-Json
        if ($version.MajorVersion -ne 5 -or $version.MinorVersion -ne 8) { continue }
        return [IO.Path]::GetFullPath($candidate)
    }
    throw 'Unreal Engine 5.8 was not found. Supply -EngineRoot or set MRFPS_UE_ROOT.'
}

function Resolve-MRFPSAndroidSdk {
    param([string]$SdkRoot)
    $candidates = @($SdkRoot, (Join-Path (Get-MRFPSRoot) '.local\android'), $env:ANDROID_HOME, $env:ANDROID_SDK_ROOT,
        (Join-Path $env:LOCALAPPDATA 'Android\Sdk'), 'C:\Program Files (x86)\Android\android-sdk')
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'platform-tools\adb.exe'))) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }
    throw 'Android SDK/platform-tools not found. Run Scripts/Initialize-Android.ps1 or supply -SdkRoot.'
}

function Resolve-MRFPSJava {
    param([string]$JavaRoot)
    foreach ($candidate in @($JavaRoot, $env:JAVA_HOME, 'C:\Program Files (x86)\Android\openjdk\jdk-17.0.14', 'C:\Program Files\Android\Android Studio\jbr')) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'bin\java.exe'))) { return $candidate }
    }
    throw 'JDK 17 or later was not found. Supply -JavaRoot or set JAVA_HOME.'
}

function Invoke-MRFPSCommand {
    param([Parameter(Mandatory)][string]$Program, [string[]]$Arguments = @(), [string]$LogPath, [switch]$DryRun)
    Write-Host ($Program + ' ' + (($Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '))
    if ($DryRun) { return }
    $previousPreference = $ErrorActionPreference
    $nativeExitCode = $null
    try {
        if ($LogPath) { New-Item -ItemType Directory -Path (Split-Path -Parent $LogPath) -Force | Out-Null }
        $ErrorActionPreference = 'Continue'
        $global:LASTEXITCODE = $null
        if ($LogPath) {
            & $Program @Arguments 2>&1 | ForEach-Object { if ($_ -is [Management.Automation.ErrorRecord]) { $_.ToString() } else { $_ } } | Tee-Object -FilePath $LogPath
        } else {
            & $Program @Arguments 2>&1 | ForEach-Object { if ($_ -is [Management.Automation.ErrorRecord]) { $_.ToString() } else { $_ } }
        }
        $nativeExitCode = $global:LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($null -eq $nativeExitCode) { throw "$Program did not return a native process exit code." }
    if ($nativeExitCode -ne 0) { throw "$Program failed with exit code $nativeExitCode." }
}

function Get-MRFPSQuest {
    param([Parameter(Mandatory)][string]$Adb, [string]$Serial, [switch]$AllowUnavailable)
    $lines = @(& $Adb devices -l 2>&1 | ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0) { throw "adb devices failed: $($lines -join [Environment]::NewLine)" }
    $devices = @($lines | ForEach-Object {
        if ($_ -match '^(\S+)\s+(device|unauthorized|offline|recovery|sideload)(?:\s+(.*))?$') {
            [PSCustomObject]@{ Serial = $Matches[1]; State = $Matches[2]; Details = $Matches[3] }
        }
    })
    if ($Serial) { $devices = @($devices | Where-Object Serial -EQ $Serial) }
    if ($AllowUnavailable) { return $devices }
    if ($devices.Count -ne 1) { throw "Expected exactly one adb device; found $($devices.Count). Use -Serial to choose the Quest." }
    if ($devices[0].State -eq 'unauthorized') { throw "Quest $($devices[0].Serial) needs USB debugging authorization. Put on the headset and accept this computer's RSA prompt, then rerun." }
    if ($devices[0].State -ne 'device') { throw "Quest $($devices[0].Serial) is $($devices[0].State). Reconnect it and rerun." }
    return $devices[0]
}

function Resolve-MRFPSCredentialFile {
    param([Parameter(Mandatory)][string]$Path, [ValidateSet('Token','PublicKey')][string]$Kind = 'Token')
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) { throw "$Kind credential file is missing." }
    $file = Get-Item -LiteralPath $fullPath
    if ($file.Length -lt 1 -or $file.Length -gt 2048) { throw "$Kind credential file has an invalid size." }
    $bytes = [IO.File]::ReadAllBytes($fullPath)
    if (@($bytes | Where-Object { $_ -eq 0 -or $_ -gt 127 }).Count) { throw "$Kind credential file must be ASCII text without a BOM. No credential content was logged." }
    $content = [IO.File]::ReadAllText($fullPath).Trim()
    if ($Kind -eq 'Token') {
        if ($content -notmatch '^[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+$') { throw 'Token file is not a compact JWT. No credential content was logged.' }
    } elseif ($content -notmatch '^-----BEGIN PUBLIC KEY-----[\s\S]+-----END PUBLIC KEY-----$' -or $content -match 'PRIVATE') {
        throw 'Supply an SPKI public-key file; private-key files are never accepted by launchers.'
    }
    return $fullPath
}

function New-MRFPSTestAuthKeys {
    param([Parameter(Mandatory)][string]$Directory, [switch]$DryRun)
    $root = Get-MRFPSRoot
    $python = Join-Path $root '.local\auth-tools\Scripts\python.exe'
    if ($DryRun) { return [PSCustomObject]@{ public_key_path = (Join-Path $Directory 'issuer-public.pem'); public_key_b64 = ''; audience = 'mrfps' } }
    if (-not (Test-Path -LiteralPath $python)) { throw 'Run Scripts/Initialize-AuthTools.ps1 before auth/network tests.' }
    Invoke-MRFPSCommand $python @((Join-Path $root 'tools\auth\create_test_credentials.py'),'--directory',$Directory,'--keys-only') | Out-Host
    return Get-Content -Raw -LiteralPath (Join-Path $Directory 'keys.json') | ConvertFrom-Json
}

function Complete-MRFPSTestAuthTokens {
    param([Parameter(Mandatory)][string]$Directory, [Parameter(Mandatory)][string]$Fingerprint, [int]$Inspectors = 3)
    $root = Get-MRFPSRoot
    $python = Join-Path $root '.local\auth-tools\Scripts\python.exe'
    Invoke-MRFPSCommand $python @((Join-Path $root 'tools\auth\create_test_credentials.py'),'--directory',$Directory,'--dtls-fingerprint',$Fingerprint,'--inspectors',"$Inspectors") | Out-Host
    return Get-Content -Raw -LiteralPath (Join-Path $Directory 'manifest.json') | ConvertFrom-Json
}

function Test-MRFPSCredentialPrivacy {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text, [string[]]$TokenFiles = @())
    if ($Text -match 'eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{80,}|-----BEGIN (?:ENCRYPTED )?PRIVATE KEY-----') { return $false }
    foreach ($path in $TokenFiles) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $token = [IO.File]::ReadAllText($path).Trim()
            if ($token -and $Text.Contains($token)) { return $false }
        }
    }
    return $true
}

function Assert-MRFPSQuestSharePermissionClear {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$ActivityDump)
    # Treat known or ambiguous privacy prompts alike. This check only reads
    # the dump; an OS permission task must be handled by the headset user.
    if ($ActivityDump -match '(?i)(MediaProjection(?:Permission|Consent)|ScreenCapture(?:Permission|Consent)|GrantPermissionsActivity)') {
        throw 'Quest may have a screen-sharing or privacy permission prompt; handle it on the headset before retrying.'
    }
    $roots = [regex]::Matches($ActivityDump,'(?m)^  \* Task\{[^\r\n]*#(?<id>\d+)[^\r\n]*\r?$')
    if (-not $roots.Count) { throw 'Cannot reliably inspect Quest activity roots; no permission prompt was changed.' }
    $ids = [Collections.Generic.HashSet[long]]::new()
    foreach ($root in $roots) {
        $id=0L
        if (-not [long]::TryParse($root.Groups['id'].Value,[ref]$id) -or $id -gt 2147483647 -or -not $ids.Add($id)) {
            throw 'Quest activity roots are ambiguous; no permission prompt was changed.'
        }
    }
}

function Get-MRFPSDisabledPluginNames {
    param([string]$ProjectRoot = (Get-MRFPSRoot))
    $project = Get-Content -Raw -LiteralPath (Join-Path $ProjectRoot 'MRFPS.uproject') | ConvertFrom-Json
    $states = @{}
    if ($project.PSObject.Properties['Plugins']) {
        foreach ($plugin in $project.Plugins) { if ($plugin.PSObject.Properties['Enabled']) { $states[$plugin.Name] = [bool]$plugin.Enabled } }
    }
    $disabled = @($states.Keys | Where-Object { -not $states[$_] })
    $pluginsDirectory = Join-Path $ProjectRoot 'Plugins'
    if (Test-Path -LiteralPath $pluginsDirectory) {
        foreach ($directory in Get-ChildItem -LiteralPath $pluginsDirectory -Directory) {
            $descriptor = Get-ChildItem -LiteralPath $directory.FullName -Filter '*.uplugin' -File | Select-Object -First 1
            if ($descriptor -and -not $states.ContainsKey($descriptor.BaseName)) {
                $metadata = Get-Content -Raw -LiteralPath $descriptor.FullName | ConvertFrom-Json
                if ($metadata.PSObject.Properties['EnabledByDefault'] -and $metadata.EnabledByDefault -eq $false) { $disabled += $descriptor.BaseName }
            }
        }
    }
    return $disabled
}

Export-ModuleMember -Function Get-MRFPSRoot, Resolve-MRFPSEngine, Resolve-MRFPSAndroidSdk, Resolve-MRFPSJava, Invoke-MRFPSCommand, Get-MRFPSQuest, Resolve-MRFPSCredentialFile, New-MRFPSTestAuthKeys, Complete-MRFPSTestAuthTokens, Test-MRFPSCredentialPrivacy, Assert-MRFPSQuestSharePermissionClear, Get-MRFPSDisabledPluginNames
