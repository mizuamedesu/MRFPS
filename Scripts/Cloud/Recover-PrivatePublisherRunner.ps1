[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReportPath,
    [string]$WslDistribution = 'Ubuntu'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$allowed = [IO.Path]::GetFullPath((Join-Path $root 'Saved/Validation')).TrimEnd('\')
$report = (Resolve-Path -LiteralPath $ReportPath).ProviderPath
$full = [IO.Path]::GetFullPath($report)
if (-not $full.StartsWith($allowed + '\', [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($full) -ne 'report.json') {
    throw 'RECOVERY_VALIDATION_REPORT_REQUIRED'
}
$current = $full
while ($current.Length -ge $allowed.Length) {
    $item = Get-Item -LiteralPath $current -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'RECOVERY_REPORT_REPARSE_FORBIDDEN' }
    if ($current.Equals($allowed, [StringComparison]::OrdinalIgnoreCase)) { break }
    $current = [IO.Path]::GetDirectoryName($current)
    if (-not $current) { throw 'RECOVERY_REPORT_PARENT_INVALID' }
}
$info = Get-Content -LiteralPath $full -Raw -Encoding UTF8 | ConvertFrom-Json
if ($info.repository -ne 'mizuamedesu/MRFPS-Server-Registry' -or
    $info.taskId -notmatch '^[0-9a-f]{16}$' -or $info.runnerLabel -ne ('mrfps-private-' + $info.taskId) -or
    $info.operation -notin @('Inspect','Probe','Publish')) { throw 'RECOVERY_REPORT_IDENTITY_MISMATCH' }
$script = Join-Path $root 'tools/cloud/ephemeral_runner.py'
$wsl = (Get-Command wsl.exe -ErrorAction Stop).Source
function Get-WslPath([string]$Path) {
    $value = (& $wsl -d $WslDistribution -u root --exec wslpath -a $Path.Replace('\','/') | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $value -notmatch '^/mnt/[a-z]/[A-Za-z0-9._/-]+$') { throw 'RECOVERY_WSL_PATH_INVALID' }
    return $value
}
$scriptPath = Get-WslPath $script
$reportPathWsl = Get-WslPath $full
& $wsl -d $WslDistribution -u root --exec python3 $scriptPath recover-artifact --task $info.taskId --report $reportPathWsl
exit $LASTEXITCODE
