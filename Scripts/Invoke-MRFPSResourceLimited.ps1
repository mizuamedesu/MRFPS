# Run a build script in a private Windows job. Closing this job cannot affect
# existing applications: its first process waits until assignment succeeds.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ScriptFile,
    [string[]]$ScriptArguments = @(),
    [Alias('MinFreeGB')][ValidateRange(0,1048576)][double]$MinimumFreeGB = 10,
    [Alias('MaxCommitPercent')][ValidateRange(0,100)][double]$MaximumCommitPercent = 75,
    [ValidateRange(0.1,60)][double]$PollIntervalSeconds = 2,
    [string]$ReportPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT') { throw 'This guard requires Windows job objects and GlobalMemoryStatusEx.' }
$scriptPath = (Resolve-Path -LiteralPath $ScriptFile).ProviderPath
if ([IO.Path]::GetExtension($scriptPath) -ne '.ps1') { throw 'ScriptFile must be a .ps1 file.' }
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $ReportPath) {
    $runName = 'ResourceGuard-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
    $ReportPath = Join-Path $projectRoot ('Saved\Validation\' + $runName + '\report.json')
}
$ReportPath = [IO.Path]::GetFullPath($ReportPath)
$reportDirectory = Split-Path -Parent $ReportPath
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$stem = [IO.Path]::GetFileNameWithoutExtension($ReportPath)
$stdoutPath = Join-Path $reportDirectory ($stem + '.stdout.log')
$stderrPath = Join-Path $reportDirectory ($stem + '.stderr.log')
$bootstrapStdoutPath = Join-Path $reportDirectory ($stem + '.bootstrap.stdout.log')
$bootstrapStderrPath = Join-Path $reportDirectory ($stem + '.bootstrap.stderr.log')
$samplesPath = Join-Path $reportDirectory ($stem + '.samples.jsonl')
foreach ($outputPath in @($ReportPath,$stdoutPath,$stderrPath,$bootstrapStdoutPath,$bootstrapStderrPath,$samplesPath)) {
    if (Test-Path -LiteralPath $outputPath) { throw "Output already exists: $outputPath. Use a fresh ReportPath." }
}

$report = [ordered]@{
    schemaVersion = 1
    startedAtUtc = [DateTime]::UtcNow.ToString('o')
    finishedAtUtc = $null
    status = 'initializing'
    scriptFile = $scriptPath
    scriptArgumentCount = $ScriptArguments.Count
    workingDirectory = (Get-Location).ProviderPath
    minimumFreeGB = $MinimumFreeGB
    maximumCommitPercent = $MaximumCommitPercent
    pollIntervalSeconds = $PollIntervalSeconds
    rootProcessId = $null
    rootProcessStartedAtUtc = $null
    childExitCode = $null
    wrapperExitCode = $null
    stopReason = $null
    error = $null
    containment = 'WindowsJobObject; assignment before child-script execution; kill-on-job-close'
    jobAssigned = $false
    terminatedOwnedJob = $false
    remainingOwnedProcessesBeforeJobClose = 0
    closedOwnedJobWithRemainingProcesses = $false
    sampleCount = 0
    minimumObservedFreeGB = $null
    maximumObservedCommitPercent = $null
    lastSample = $null
    samplesPath = $samplesPath
    reportWriteDeferrals = 0
    stdoutPath = $stdoutPath
    stderrPath = $stderrPath
    bootstrapStdoutPath = $bootstrapStdoutPath
    bootstrapStderrPath = $bootstrapStderrPath
}
$utf8 = New-Object Text.UTF8Encoding($false)
function Save-GuardReport {
    $temporaryPath = $ReportPath + '.tmp'
    $bytes = $utf8.GetBytes(($report | ConvertTo-Json -Depth 8))
    $stream = [IO.File]::Open($temporaryPath,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
    for ($attempt=0; $attempt -lt 10; $attempt++) {
        try {
            if (Test-Path -LiteralPath $ReportPath) { [IO.File]::Replace($temporaryPath,$ReportPath,[NullString]::Value) }
            else { [IO.File]::Move($temporaryPath,$ReportPath) }
            return
        } catch {
            $cause=$_.Exception.GetBaseException()
            $nativeCode=$cause.HResult -band 0xffff
            if ($cause -isnot [IO.IOException] -or $nativeCode -notin @(32,33)) { throw }
            # Report readers do not suspend the independent memory checks.
            # Keep the complete previous report and publish on the next poll.
            if ($attempt -eq 9) { $report.reportWriteDeferrals++; return }
            Start-Sleep -Milliseconds 25
        }
    }
}

$job = $null
$gate = $null
$child = $null
$wrapperExit = 126
try {
    if (-not ('MRFPSResourceGuard.Native' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace MRFPSResourceGuard {
    [StructLayout(LayoutKind.Sequential)] public struct MemoryStatus {
        public uint Length, Load;
        public ulong TotalPhysical, AvailablePhysical, CommitLimit, AvailableCommit;
        public ulong TotalVirtual, AvailableVirtual, AvailableExtendedVirtual;
    }
    [StructLayout(LayoutKind.Sequential)] struct BasicLimits {
        public long ProcessUserTime, JobUserTime;
        public uint Flags;
        public UIntPtr MinimumWorkingSet, MaximumWorkingSet;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)] struct IoCounters {
        public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes;
    }
    [StructLayout(LayoutKind.Sequential)] struct ExtendedLimits {
        public BasicLimits Basic;
        public IoCounters Io;
        public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
    }
    [StructLayout(LayoutKind.Sequential)] struct Accounting {
        public long UserTime, KernelTime, PeriodUserTime, PeriodKernelTime;
        public uint PageFaults, TotalProcesses, ActiveProcesses, TerminatedProcesses;
    }
    public static class Native {
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool GlobalMemoryStatusEx(ref MemoryStatus value);
        public static MemoryStatus ReadMemory() {
            var value = new MemoryStatus();
            value.Length = (uint)Marshal.SizeOf(typeof(MemoryStatus));
            if (!GlobalMemoryStatusEx(ref value)) throw new Win32Exception(Marshal.GetLastWin32Error());
            return value;
        }
    }
    public sealed class OwnedJob : IDisposable {
        IntPtr handle;
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr attributes, string name);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job, int kind, ref ExtendedLimits value, uint size);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool QueryInformationJobObject(IntPtr job, int kind, out Accounting value, uint size, IntPtr returned);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool TerminateJobObject(IntPtr job, uint exitCode);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        public OwnedJob() {
            handle = CreateJobObject(IntPtr.Zero, null);
            if (handle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            var limits = new ExtendedLimits();
            limits.Basic.Flags = 0x00002000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if (!SetInformationJobObject(handle, 9, ref limits, (uint)Marshal.SizeOf(typeof(ExtendedLimits)))) {
                int error = Marshal.GetLastWin32Error(); CloseHandle(handle); handle = IntPtr.Zero;
                throw new Win32Exception(error);
            }
        }
        public void Assign(IntPtr process) {
            if (!AssignProcessToJobObject(handle, process)) throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        public uint ActiveProcesses() {
            Accounting value;
            if (!QueryInformationJobObject(handle, 1, out value, (uint)Marshal.SizeOf(typeof(Accounting)), IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return value.ActiveProcesses;
        }
        public void Stop() {
            if (!TerminateJobObject(handle, 125)) throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        public void Dispose() { if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; } }
    }
}
'@
    }
    function Read-GuardSample {
        $memory = [MRFPSResourceGuard.Native]::ReadMemory()
        if ($memory.CommitLimit -eq 0) { throw 'GlobalMemoryStatusEx returned a zero commit limit.' }
        $freeGB = [double]$memory.AvailablePhysical / 1GB
        $commitPercent = 100.0 * ($memory.CommitLimit - $memory.AvailableCommit) / $memory.CommitLimit
        $sample = [ordered]@{
            atUtc = [DateTime]::UtcNow.ToString('o')
            availablePhysicalBytes = $memory.AvailablePhysical
            totalPhysicalBytes = $memory.TotalPhysical
            availablePhysicalGB = [Math]::Round($freeGB,4)
            committedBytes = $memory.CommitLimit - $memory.AvailableCommit
            commitLimitBytes = $memory.CommitLimit
            commitPercent = [Math]::Round($commitPercent,4)
            ownedActiveProcesses = $(if ($job) { $job.ActiveProcesses() } else { 0 })
            pressureReason = $(if ($freeGB -lt $MinimumFreeGB) { 'available_physical_below_minimum' }
                elseif ($commitPercent -gt $MaximumCommitPercent) { 'commit_above_maximum' } else { $null })
        }
        $report.sampleCount++
        if ($null -eq $report.minimumObservedFreeGB -or $freeGB -lt $report.minimumObservedFreeGB) {
            $report.minimumObservedFreeGB = $freeGB
        }
        if ($null -eq $report.maximumObservedCommitPercent -or $commitPercent -gt $report.maximumObservedCommitPercent) {
            $report.maximumObservedCommitPercent = $commitPercent
        }
        $report.lastSample = $sample
        [IO.File]::AppendAllText($samplesPath, (($sample | ConvertTo-Json -Compress) + [Environment]::NewLine), $utf8)
        Save-GuardReport
        return [PSCustomObject]$sample
    }

    $sample = Read-GuardSample
    if ($sample.pressureReason) {
        $report.status = 'preflight_rejected'
        $report.stopReason = $sample.pressureReason
        $wrapperExit = 125
    } else {
        $job = New-Object MRFPSResourceGuard.OwnedJob
        $gateName = 'Local\MRFPSResourceGuard-' + [Guid]::NewGuid().ToString('N')
        $gate = New-Object Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $gateName)
        $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        # JSON + EncodedCommand transports argument values as data. A second
        # PowerShell uses -File for normal named-parameter binding (array splats
        # of strings do not bind PowerShell named parameters).
        $payload = [ordered]@{ script = $scriptPath; arguments = @($ScriptArguments); gate = $gateName; powershell = $powershell; stdout = $stdoutPath; stderr = $stderrPath }
        $payload64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($payload | ConvertTo-Json -Compress)))
        $bootstrap = @'
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try {
    $data = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('__PAYLOAD__')) | ConvertFrom-Json
    $gate = [Threading.EventWaitHandle]::OpenExisting($data.gate)
    try { if (-not $gate.WaitOne(60000)) { throw 'Resource guard did not authorize child execution.' } }
    finally { $gate.Dispose() }
    # Child processes inherit this process-local mode. Optional profiler DLL
    # failures must return to their caller instead of blocking unattended runs
    # behind a Windows loader dialog. Required DLL failures still fail startup.
    Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public static class MRFPSChildLoaderMode { [DllImport("kernel32.dll")] public static extern uint GetErrorMode(); [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint value); }'
    [void][MRFPSChildLoaderMode]::SetErrorMode([MRFPSChildLoaderMode]::GetErrorMode() -bor 1)
    function Quote-WindowsArgument([string]$value) {
        # CommandLineToArgvW/CRT quoting, including embedded quotes and trailing backslashes.
        return '"' + ([regex]::Replace([regex]::Replace($value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1')) + '"'
    }
    $argumentValues = @('-NoLogo','-NoProfile','-NonInteractive','-File',[string]$data.script) + @($data.arguments)
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = [string]$data.powershell
    $startInfo.Arguments = (($argumentValues | ForEach-Object { Quote-WindowsArgument ([string]$_) }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $stdout = [IO.File]::Open([string]$data.stdout,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    $stderr = [IO.File]::Open([string]$data.stderr,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    $process = [Diagnostics.Process]::Start($startInfo)
    $stdoutCopy = $process.StandardOutput.BaseStream.CopyToAsync($stdout)
    $stderrCopy = $process.StandardError.BaseStream.CopyToAsync($stderr)
    $process.WaitForExit()
    # A reused build worker can inherit a write end of these pipes. Its EOF
    # must not delay the actual script's completion indefinitely. Copying runs
    # throughout execution; allow one final second to drain trailing output.
    $copies = [Threading.Tasks.Task[]]@($stdoutCopy,$stderrCopy)
    if (-not [Threading.Tasks.Task]::WaitAll($copies,1000)) {
        [Console]::Error.WriteLine('RESOURCE_GUARD_OUTPUT_PIPE_OPEN_AFTER_SCRIPT_EXIT; remaining owned processes will be cleaned up by the job.')
    }
    $result = $process.ExitCode
    $process.Dispose()
    $stdout.Dispose()
    $stderr.Dispose()
    exit $result
} catch {
    [Console]::Error.WriteLine($_.ToString())
    exit 126
}
'@
        $bootstrap = $bootstrap.Replace('__PAYLOAD__', $payload64)
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($bootstrap))
        $child = Start-Process -FilePath $powershell -ArgumentList ('-NoLogo -NoProfile -NonInteractive -EncodedCommand ' + $encoded) -WindowStyle Hidden -PassThru -WorkingDirectory $report.workingDirectory -RedirectStandardOutput $bootstrapStdoutPath -RedirectStandardError $bootstrapStderrPath
        $report.rootProcessId = $child.Id
        $report.rootProcessStartedAtUtc = $child.StartTime.ToUniversalTime().ToString('o')
        $job.Assign($child.Handle)
        $report.jobAssigned = $true
        $report.status = 'running'
        $sample = Read-GuardSample
        if (-not $sample.pressureReason) { $gate.Set() | Out-Null }
        while ($true) {
            if ($sample.pressureReason) {
                $report.status = 'memory_limit_exceeded'
                $report.stopReason = $sample.pressureReason
                $job.Stop()
                $report.terminatedOwnedJob = $true
                $wrapperExit = 125
                break
            }
            if ($child.HasExited) {
                $child.WaitForExit()
                $report.childExitCode = $child.ExitCode
                $wrapperExit = $child.ExitCode
                $report.status = $(if ($wrapperExit -eq 0) { 'completed' } else { 'child_failed' })
                break
            }
            Start-Sleep -Milliseconds ([int]($PollIntervalSeconds * 1000))
            $sample = Read-GuardSample
        }
    }
} catch {
    $report.status = 'guard_failed'
    $report.error = $_.Exception.Message
    $report.stopReason = 'guard_error'
    $wrapperExit = 126
    if ($job -and $report.jobAssigned) {
        try { $job.Stop(); $report.terminatedOwnedJob = $true } catch { }
    } elseif ($child) {
        # Assignment failed while this exact process object was still at the
        # gate. It has not run the supplied script or spawned descendants.
        try { if (-not $child.HasExited) { $child.Kill() } } catch { }
    }
} finally {
    if ($job) {
        try {
            $report.remainingOwnedProcessesBeforeJobClose = $job.ActiveProcesses()
            $report.closedOwnedJobWithRemainingProcesses = ($report.remainingOwnedProcessesBeforeJobClose -gt 0)
        } catch { }
        $job.Dispose()
    }
    if ($child) {
        try {
            if ($child.WaitForExit(5000)) { $report.childExitCode = $child.ExitCode }
        } catch { }
        $child.Dispose()
    }
    if ($gate) { $gate.Dispose() }
    $report.wrapperExitCode = $wrapperExit
    $report.finishedAtUtc = [DateTime]::UtcNow.ToString('o')
    Save-GuardReport
}
Write-Host ("Resource guard: {0}; exit={1}; min free={2:N2} GB; max commit={3:N2}%; report={4}" -f $report.status,$wrapperExit,$report.minimumObservedFreeGB,$report.maximumObservedCommitPercent,$ReportPath)
exit $wrapperExit
