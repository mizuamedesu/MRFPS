param(
    [Parameter(Mandatory)][string]$Id,
    [Parameter(Mandatory)][string]$LinuxScript,
    [Parameter(Mandatory)][string]$Directory
)
$ErrorActionPreference='Stop'
if (-not [Environment]::Is64BitProcess) { throw '64-bit Windows PowerShell is required' }
if ($Id -cnotmatch '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$') { throw 'Invalid identifier' }
if (-not $LinuxScript.StartsWith('/') -or $LinuxScript.Contains('"') -or $LinuxScript.IndexOf([char]92) -ge 0 -or $LinuxScript.IndexOf([char]0) -ge 0 -or $LinuxScript.IndexOf([char]10) -ge 0 -or $LinuxScript.IndexOf([char]13) -ge 0) { throw 'Invalid Linux helper path' }
$root=[IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'MRFPS\WslKeepalive'))
if (-not [StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath($Directory),$root)) { throw 'Invalid state directory' }
$stdin=Join-Path $root ($Id+'.stdin')
$stdout=Join-Path $root ($Id+'.stdout')
$stderr=Join-Path $root ($Id+'.stderr')
foreach($path in @($stdin,$stdout,$stderr)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Invalid launch stream'
    }
}
$receipt=Join-Path $root ($Id+'.windows.json')
$pending=Join-Path $root ($Id+'.windows.pending')
if ((Test-Path -LiteralPath $receipt) -or (Test-Path -LiteralPath $pending)) { throw 'Launch receipt already exists' }
$wsl=Join-Path $env:WINDIR 'System32\wsl.exe'
if (-not (Test-Path -LiteralPath $wsl -PathType Leaf)) { throw 'WSL executable unavailable' }
# Start-Process joins ArgumentList using spaces. Quote the one validated path
# that can contain spaces; no shell interprets the resulting arguments.
$args=@('-d','Ubuntu','-u','root','--exec','/usr/bin/python3',('"'+$LinuxScript+'"'),'serve',$Id)
$process=Start-Process -FilePath $wsl -ArgumentList $args -WindowStyle Hidden -PassThru `
    -RedirectStandardInput $stdin -RedirectStandardOutput $stdout -RedirectStandardError $stderr
try { $created=$process.StartTime.ToUniversalTime().ToFileTimeUtc() }
catch { throw 'Launched process identity is unavailable' }
# Store the identity durably before acknowledging. A lost or partial receipt
# is unknown; the controller must not start a second keeper or send a signal.
$json=[ordered]@{pid=$process.Id;created=$created} | ConvertTo-Json -Compress
$bytes=[Text.Encoding]::UTF8.GetBytes($json)
$stream=[IO.File]::Open($pending,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
[IO.File]::Move($pending,$receipt)
Write-Output 'launch receipt recorded'
