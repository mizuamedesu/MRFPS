[CmdletBinding()]
param(
    [string]$EngineRoot,
    [Parameter(Mandatory)][string]$ReportDirectory,
    [Parameter(Mandatory)][string]$GuardReport
)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root=Get-MRFPSRoot
$engine=Resolve-MRFPSEngine $EngineRoot
$editor=Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$project=Join-Path $root 'MRFPS.uproject'
$GuardReport=[IO.Path]::GetFullPath($GuardReport)
$ReportDirectory=[IO.Path]::GetFullPath($ReportDirectory)
if (-not (Test-Path -LiteralPath $GuardReport -PathType Leaf)) { throw 'SLATE_FIXTURE_GUARD_REQUIRED' }
$guardFile=Get-Item -LiteralPath $GuardReport
if (([DateTime]::UtcNow-$guardFile.LastWriteTimeUtc).TotalSeconds -gt 15) { throw 'SLATE_FIXTURE_GUARD_STALE' }
$guard=Get-Content -Raw -LiteralPath $GuardReport | ConvertFrom-Json
if ($null -eq $guard.minimumFreeGB -or $null -eq $guard.maximumCommitPercent -or
    -not $guard.lastSample -or $null -eq $guard.lastSample.availablePhysicalGB -or
    $null -eq $guard.lastSample.commitPercent) { throw 'SLATE_FIXTURE_GUARD_INVALID' }
foreach ($value in @($guard.minimumFreeGB,$guard.maximumCommitPercent,
                     $guard.lastSample.availablePhysicalGB,$guard.lastSample.commitPercent)) {
    $number=[double]$value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw 'SLATE_FIXTURE_GUARD_INVALID' }
}
if ($guard.status -ne 'running' -or $guard.jobAssigned -ne $true -or
    $guard.scriptFile -ne [IO.Path]::GetFullPath($PSCommandPath) -or
    $guard.minimumFreeGB -lt 10 -or $guard.maximumCommitPercent -lt 0 -or $guard.maximumCommitPercent -gt 75 -or
    $guard.lastSample.pressureReason -or $guard.lastSample.availablePhysicalGB -lt 10 -or
    $guard.lastSample.commitPercent -lt 0 -or $guard.lastSample.commitPercent -gt 75) {
    throw 'SLATE_FIXTURE_GUARD_INVALID'
}
if (-not ('MRFPSLoginSlateFixtureJobCheck' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MRFPSLoginSlateFixtureJobCheck {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError=true)] [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool IsProcessInJob(IntPtr process, IntPtr job, [MarshalAs(UnmanagedType.Bool)] out bool contained);
    public static bool IsContained() { bool value; return IsProcessInJob(GetCurrentProcess(),IntPtr.Zero,out value) && value; }
}
'@
}
if (-not [MRFPSLoginSlateFixtureJobCheck]::IsContained()) { throw 'SLATE_FIXTURE_JOB_REQUIRED' }
if (Test-Path -LiteralPath $ReportDirectory) { throw 'SLATE_FIXTURE_REPORT_EXISTS' }
New-Item -ItemType Directory -Path $ReportDirectory | Out-Null
$arguments=@($project,'-unattended','-nohmd','-nosound','-NoSplash','-NoLiveCoding','-nop4',
    '-stdout','-FullStdOutLogOutput','-ExecCmds=Automation RunTests QuestVisualFixture.LoginPointerCancel',
    '-TestExit=Automation Test Queue Empty',"-ReportExportPath=$ReportDirectory", "-abslog=$(Join-Path $ReportDirectory 'engine.log')")
Invoke-MRFPSCommand $editor $arguments -LogPath (Join-Path $ReportDirectory 'stdout.log')
$index=Join-Path $ReportDirectory 'index.json'
if (-not (Test-Path -LiteralPath $index -PathType Leaf)) { throw 'SLATE_FIXTURE_REPORT_MISSING' }
$report=Get-Content -Raw -LiteralPath $index | ConvertFrom-Json
$tests=@($report.tests | Where-Object { $_.fullTestPath -eq 'QuestVisualFixture.LoginPointerCancel' })
if ($tests.Count -ne 1 -or $tests[0].state -ne 'Success' -or $tests[0].errors -ne 0) {
    throw 'SLATE_FIXTURE_FAILED_OR_MISSING'
}
Write-Output '{"status":"passed","scope":"UE Editor Slate and world-widget pointer cancellation"}'
