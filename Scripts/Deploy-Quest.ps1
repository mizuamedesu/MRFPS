[CmdletBinding()]
param(
    [string]$Apk, [string]$SdkRoot, [string]$Serial,
    [string]$Package = 'org.mrfps.arena',
    [string]$Server = '', [ValidateRange(1,65535)][int]$Port = 7777,
    [ValidateRange(1,120)][int]$CaptureSeconds = 15,
    [switch]$SkipInstall, [switch]$Inspector, [switch]$DryRun,
    [switch]$RequestCapture, [switch]$VideoFromAdb, [switch]$NoSound,
    [string]$GloveSender, [string]$GloveDevice,
    [ValidateRange(1024,65535)][int]$GlovePort = 25788,
    [Parameter(Mandatory)][string]$TokenFile,
    [Parameter(Mandatory)][string]$PublicKeyFile,
    [string]$Audience = 'mrfps'
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
if ($Inspector -and -not $Server) { throw '-Inspector requires -Server so the observer role is assigned by the dedicated server.' }
if (-not $Server) { throw 'An authenticated dedicated -Server is required.' }
if ($GloveSender) {
    $gloveAddress=$null
    if (-not [Net.IPAddress]::TryParse($GloveSender,[ref]$gloveAddress) -or
        $gloveAddress.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
        $gloveAddress.Equals([Net.IPAddress]::Any) -or $gloveAddress.Equals([Net.IPAddress]::Broadcast)) {
        throw '-GloveSender must identify the PC bridge by a single valid IPv4 address.'
    }
    $GloveSender=$gloveAddress.ToString()
}
if ($GloveDevice -and $GloveDevice -notmatch '^[A-Za-z0-9_.:-]{1,128}$') { throw 'Invalid -GloveDevice identifier.' }
if ($GloveDevice -and -not $GloveSender) { throw '-GloveDevice requires -GloveSender.' }
if ($Inspector -and $GloveSender) { throw 'Glove input belongs to a player, not an inspector.' }
if ($Serial -and $Serial -notmatch '^[A-Za-z0-9_.:-]{1,128}$') { throw 'Invalid -Serial.' }
$TokenFile = Resolve-MRFPSCredentialFile $TokenFile
$PublicKeyFile = Resolve-MRFPSCredentialFile $PublicKeyFile -Kind PublicKey
if ($Package -notmatch '^[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+$' -or $Audience -notmatch '^[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}$') { throw 'Invalid package or audience.' }
$sdk = Resolve-MRFPSAndroidSdk $SdkRoot
$adb = Join-Path $sdk 'platform-tools\adb.exe'
$device = if ($DryRun -and $Serial) {
    # An explicit target can be reviewed while USB authorization is unavailable.
    # Every device mutation below remains guarded by -DryRun.
    [PSCustomObject]@{Serial=$Serial;State='dry_run_no_connection_used'}
} else { Get-MRFPSQuest -Adb $adb -Serial $Serial }
$prefix = @('-s', $device.Serial)

function Write-QuestPrivateFile([string]$Source, [string]$RelativePath) {
    if ($RelativePath -notmatch '^files/MRFPS/(access\.jwt|issuer-public\.pem)$') { throw 'Invalid app-private credential path.' }
    Write-Host "Provision app-private $RelativePath through adb stdin (contents omitted)."
    if ($DryRun) { return }
    # Keep credentials out of process arguments, shell history and tool output.
    # run-as is available for this development APK and writes as the app UID.
    $remoteCommand = "run-as $Package sh -c 'umask 077; cat > $RelativePath.tmp && chmod 600 $RelativePath.tmp && mv $RelativePath.tmp $RelativePath'"
    $arguments = $prefix + @('shell', '-T', $remoteCommand)
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $adb
    $start.Arguments = (($arguments | ForEach-Object {
        '"' + ([regex]::Replace([regex]::Replace([string]$_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1')) + '"'
    }) -join ' ')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    $started = $false
    try {
        if (-not $process.Start()) { throw 'Credential provisioning process did not start.' }
        $started = $true
        # Drain both pipes without ever echoing output that could contain a credential.
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $bytes = [IO.File]::ReadAllBytes($Source)
        try { $process.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length) }
        finally { [Array]::Clear($bytes, 0, $bytes.Length); $process.StandardInput.Close() }
        if (-not $process.WaitForExit(20000)) { throw 'App-private credential provisioning timed out.' }
        if ($process.ExitCode -ne 0) { throw "App-private credential provisioning failed (exit $($process.ExitCode)); contents omitted." }
        if (-not $stdout.Wait(1000) -or -not $stderr.Wait(1000)) { throw 'Credential provisioning output did not close.' }
    } finally {
        if ($started -and -not $process.HasExited) { $process.Kill(); [void]$process.WaitForExit(2000) }
        $process.Dispose()
    }
}

function Get-QuestRunningPid {
    $prior = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $adb @prefix shell pidof $Package 2>&1 | ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $prior }
    $value = ($output -join ' ').Trim()
    if ($exitCode -eq 1 -and -not $value) { return '' }
    if ($exitCode -eq 0 -and $value -cmatch '^\d+(?:\s+\d+)*$') { return $value }
    throw 'Cannot reliably inspect the Quest app process; no dialog was closed.'
}

function Assert-QuestPermissionClear {
    $activityDump = (& $adb @prefix shell dumpsys activity activities | Out-String -Width 8192)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect Quest activities; no permission prompt was changed.' }
    Assert-MRFPSQuestSharePermissionClear -ActivityDump $activityDump
}

# Install or force-stop can indirectly dismiss an app's permission request.
# Check before either operation and again before continuing after a stop.
if (-not $DryRun) { Assert-QuestPermissionClear }
if (-not $SkipInstall) {
    if (-not $Apk) {
        $apks = @(Get-ChildItem -LiteralPath (Join-Path $root 'Artifacts\Packages') -Filter '*.apk' -File -Recurse | Where-Object Name -NotMatch '(?i)universal.*unsigned|unaligned')
        if ($apks.Count -ne 1) { throw "Found $($apks.Count) APK files. Specify -Apk explicitly." }
        $Apk = $apks[0].FullName
    }
    if (-not (Test-Path -LiteralPath $Apk -PathType Leaf)) { throw "APK not found: $Apk" }
    Invoke-MRFPSCommand $adb ($prefix + @('install','-r',$Apk)) -DryRun:$DryRun
}
if (-not $DryRun) { Assert-QuestPermissionClear }
$stamp = Get-Date -Format yyyyMMdd-HHmmss
$validation = Join-Path $root "Saved\Validation\Quest-$stamp"
New-Item -ItemType Directory -Force -Path $validation | Out-Null
if ($Server -notmatch '^[a-zA-Z0-9.:-]+$') { throw 'Server must be an IP address or hostname without URL/shell characters.' }
Invoke-MRFPSCommand $adb ($prefix + @('shell','am','force-stop',$Package)) -DryRun:$DryRun
if (-not $DryRun) {
    $remainingPid = ''
    for ($attempt=0; $attempt -lt 8; $attempt++) {
        $remainingPid = Get-QuestRunningPid
        if (-not $remainingPid) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($remainingPid) { throw 'Quest app still has a process after force-stop; no dialog was closed.' }
    Assert-QuestPermissionClear
}
$appDataRoot = "/data/user/0/$Package"
if (-not $DryRun) {
    $appDataRoot = (& $adb @prefix shell run-as $Package pwd | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $appDataRoot -notmatch "^/data/(?:user/\d+|data)/$([regex]::Escape($Package))$") {
        throw 'Cannot locate the development APK app-private directory using run-as.'
    }
}
$remoteDirectory = "$appDataRoot/files/MRFPS"
$remoteToken = "$remoteDirectory/access.jwt"
$remotePublic = "$remoteDirectory/issuer-public.pem"
Invoke-MRFPSCommand $adb ($prefix + @('shell',"run-as $Package sh -c 'umask 077; mkdir -p files/MRFPS && chmod 700 files/MRFPS'")) -DryRun:$DryRun
Write-QuestPrivateFile $TokenFile 'files/MRFPS/access.jwt'
Write-QuestPrivateFile $PublicKeyFile 'files/MRFPS/issuer-public.pem'
# Use the normal validated connect path, including a safe error code if loading
# or verifying credentials fails before the native DTLS handshake.
$commandLine = "/Game/Maps/MRFPSArena -vr -log -MRFPSServer=$($Server):$Port"
if ($Inspector) { $commandLine += ' -MRFPSInspector' }
if ($RequestCapture) { $commandLine += ' -MRFPSRequestCapture' }
if ($VideoFromAdb) { $commandLine += ' -MRFPSVideoFromAdb' }
if ($NoSound) { $commandLine += ' -nosound' }
if ($GloveSender) { $commandLine += " -MRFPSGloveSender=$GloveSender -MRFPSGlovePort=$GlovePort" }
if ($GloveDevice) { $commandLine += " -MRFPSGloveDevice=$GloveDevice" }
$commandLine += " -MRFPSAuthTokenFile=$remoteToken -MRFPSAuthPublicKeyFile=$remotePublic -MRFPSAuthAudience=$Audience"
# UE5.8 LaunchAndroid.cpp reads the cmdline intent extra before its embedded command line.
# adb joins its shell arguments for Android's shell. Preserve the complete intent
# value there; Windows native argument quoting alone does not protect its spaces.
$wakeKey = 'KEYCODE_WAKEUP'
if (-not $DryRun) {
    $powerState = (& $adb @prefix shell dumpsys power | Out-String)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect Quest power state before launch.' }
    if ($powerState -match 'mWakefulness=Asleep') { $wakeKey = 'KEYCODE_POWER' }
    Assert-QuestPermissionClear
}
Invoke-MRFPSCommand $adb ($prefix + @('shell','input','keyevent',$wakeKey)) -DryRun:$DryRun
# Do not wait indefinitely for a resumed Activity while an unworn headset or
# system dialog defers launch. The bounded observation below checks the actual
# process and preserves activity/log evidence even when launch remains pending.
if (-not $DryRun) { Assert-QuestPermissionClear }
Invoke-MRFPSCommand $adb ($prefix + @('shell','am','start','-a','android.intent.action.MAIN','-c','android.intent.category.LAUNCHER','-n',"$Package/com.epicgames.unreal.GameActivity",'--es','cmdline',("'" + $commandLine + "'"))) -DryRun:$DryRun
if ($DryRun) { return }
Start-Sleep -Seconds ([Math]::Min($CaptureSeconds, 60))
if ($CaptureSeconds -gt 60) { Start-Sleep -Seconds ($CaptureSeconds - 60) }
$appPid = (& $adb @prefix shell pidof $Package | Out-String).Trim()
$appProcessIds = @($appPid -split '\s+' | Where-Object { $_ -match '^\d+$' })
$appLog = Join-Path $validation 'logcat.txt'
if ($appProcessIds.Count) {
    foreach ($processId in $appProcessIds) {
        & $adb @prefix logcat -d "--pid=$processId" -v threadtime | Out-File -LiteralPath $appLog -Encoding utf8 -Append
    }
} else {
    'App exited before PID-scoped log collection.' | Out-File -LiteralPath $appLog -Encoding utf8
}
& $adb @prefix shell dumpsys activity activities | Out-File -LiteralPath (Join-Path $validation 'activities.txt') -Encoding utf8
& $adb @prefix shell dumpsys SurfaceFlinger --list | Out-File -LiteralPath (Join-Path $validation 'surfaces.txt') -Encoding utf8
$report = [ordered]@{ TimestampUtc = [DateTime]::UtcNow.ToString('o'); Serial = $device.Serial; Package = $Package; Apk = $Apk; CommandLine = $commandLine; ProcessId = $appPid; RuntimeProcessAlive = [bool]$appPid; EvidenceDirectory = $validation; ManualChecksRemaining = @('Headset renders passthrough with arena alignment', 'Two players join dedicated server with independent body/hand tracking', 'Glove rising-left-hand wall and open-left-palm shield', 'Shot/hit MoBits physical feedback', 'Independent inspectors switch player views/free view') }
$report | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $validation 'report.json') -Encoding utf8
if (-not $appPid) { throw "App process did not remain running. Inspect $validation\logcat.txt." }
Write-Host "App process $appPid is alive. Logs: $validation. This alone does not verify MR or physical interactions."
