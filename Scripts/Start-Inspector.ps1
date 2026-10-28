[CmdletBinding()]
param([string]$Server = '127.0.0.1', [ValidateRange(1,65535)][int]$Port = 7777,
    [ValidateRange(1,64)][int]$Count = 1, [string]$Executable, [string]$EngineRoot, [switch]$UseEditor,
    [Parameter(Mandatory)][string]$PublicKeyFile, [string[]]$TokenFiles, [string]$TokenDirectory,
    [string]$Audience = 'mrfps', [switch]$DryRun)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
if ($Server -notmatch '^[a-zA-Z0-9.:-]+$') { throw 'Use an IP address or hostname.' }
$PublicKeyFile = Resolve-MRFPSCredentialFile $PublicKeyFile -Kind PublicKey
if ($Audience -notmatch '^[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}$') { throw 'Invalid auth audience.' }
if ([bool]$TokenFiles -eq [bool]$TokenDirectory) { throw 'Supply either -TokenFiles or -TokenDirectory, with one fresh inspector token per instance.' }
if ($TokenDirectory) { $TokenFiles = @(Get-ChildItem -LiteralPath $TokenDirectory -Filter '*.jwt' -File | Sort-Object Name | ForEach-Object FullName) }
if ($TokenFiles.Count -ne $Count) { throw 'Exactly one token file is required per inspector instance.' }
$TokenFiles = @($TokenFiles | ForEach-Object { Resolve-MRFPSCredentialFile $_ })
$tokenHashes = @($TokenFiles | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash })
if (@($tokenHashes | Select-Object -Unique).Count -ne $Count) { throw 'Inspector instances must use distinct one-connection tokens.' }
if ($UseEditor) {
    $Executable = Join-Path (Resolve-MRFPSEngine $EngineRoot) 'Engine\Binaries\Win64\UnrealEditor.exe'
    $projectArgument = '"' + (Join-Path $root 'MRFPS.uproject') + '" -game '
} else {
    if (-not $Executable) { $Executable = Join-Path $root 'Artifacts\Packages\Windows\MRFPS.exe' }
    $projectArgument = ''
}
if (-not (Test-Path -LiteralPath $Executable)) { throw "Executable not found: $Executable. Build -Target Windows or specify -UseEditor." }
for ($i = 0; $i -lt $Count; $i++) {
    $arguments = $projectArgument + "/Game/Maps/MRFPSArena -MRFPSServer=$($Server):$Port -MRFPSInspector -MRFPSDesktop -MRFPSAuthTokenFile=`"$($TokenFiles[$i])`" -MRFPSAuthPublicKeyFile=`"$PublicKeyFile`" -MRFPSAuthAudience=$Audience -nohmd -windowed -ResX=1280 -ResY=720 -WinX=$((($i % 2) * 80) + 40) -WinY=$(([Math]::Floor($i / 2) * 60) + 40) -log"
    # These are visible interactive windows requested for operating inspectors.
    if ($DryRun) { Write-Host "$Executable $arguments" }
    else { Start-Process -FilePath $Executable -ArgumentList $arguments -WorkingDirectory $root }
}
