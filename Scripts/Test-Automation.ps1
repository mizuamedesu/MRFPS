[CmdletBinding()]
param(
    [string]$EngineRoot,
    [ValidateSet('MRFPS','MoBitsSDK')][string[]]$Suites = @('MRFPS','MoBitsSDK'),
    [string]$ReportDirectory,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$engine = Resolve-MRFPSEngine $EngineRoot
$executable = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$project = Join-Path $root 'MRFPS.uproject'
if (-not $ReportDirectory) { $ReportDirectory = Join-Path $root 'Saved\Validation\Automation' }
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
$results = [Collections.Generic.List[object]]::new()
$suiteFilters = @{ MRFPS = 'MRFPS'; MoBitsSDK = 'MoBits.SDK' }

function Quote-MRFPSArgument([string]$Value) {
    '"' + (($Value -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"'
}
function Get-RequiredAutomationTests([string]$Filter) {
    $files = @()
    $disabled = @(Get-MRFPSDisabledPluginNames -ProjectRoot $root)
    foreach ($directory in @('Source','Plugins')) {
        $path = Join-Path $root $directory
        if (Test-Path -LiteralPath $path) {
            $files += Get-ChildItem -LiteralPath $path -Recurse -File -Filter '*.cpp' | Where-Object {
                $filePath = $_.FullName.Replace('\','/')
                $included = $filePath -notmatch '/(Binaries|Intermediate)/'
                foreach ($plugin in $disabled) { if ($filePath -like "*/Plugins/$plugin/*") { $included = $false } }
                $included
            }
        }
    }
    @($files | ForEach-Object {
        $source = [IO.File]::ReadAllText($_.FullName)
        [regex]::Matches($source, 'IMPLEMENT_[A-Z_]*AUTOMATION_TEST\s*\([^,]+,\s*"([^"]+)"') | ForEach-Object { $_.Groups[1].Value }
    } | Where-Object { $_ -like "$Filter.*" } | Sort-Object -Unique)
}

if (-not $DryRun) {
    if (-not (Test-Path -LiteralPath (Join-Path $root 'Content\Maps\MRFPSArena.umap'))) { throw 'Generated arena map is missing. Complete the Editor build and -run=MRFPSSetup before running automation.' }
    foreach ($module in @('MRFPS','MRFPSEditor','MRFPSGloves','MoBitsSDK')) {
        $moduleCandidates = @(
            (Join-Path $root "Binaries\Win64\UnrealEditor-$module.dll"),
            (Join-Path $root "Plugins\$module\Binaries\Win64\UnrealEditor-$module.dll")
        )
        if (-not @($moduleCandidates | Where-Object { Test-Path -LiteralPath $_ }).Count) { throw "Built module $module is missing. Complete the Editor build before running automation." }
    }
}

foreach ($suite in $Suites | Select-Object -Unique) {
    $filter = $suiteFilters[$suite]
    $required = @(Get-RequiredAutomationTests $filter)
    if (-not $required.Count) { throw "No registered automation tests were found for $filter; an empty run cannot pass." }
    $suiteDirectory = Join-Path $ReportDirectory $suite
    $indexPath = Join-Path $suiteDirectory 'index.json'
    $engineLog = Join-Path $suiteDirectory 'engine.log'
    $stdoutPath = Join-Path $suiteDirectory 'stdout.log'
    $stderrPath = Join-Path $suiteDirectory 'stderr.log'
    $arguments = @($project,'/Game/Maps/MRFPSArena','-unattended','-NullRHI','-nohmd','-nosound','-NoSplash','-NoLiveCoding','-nop4','-stdout','-FullStdOutLogOutput',"-ExecCmds=Automation RunTests $filter",'-TestExit=Automation Test Queue Empty',"-ReportExportPath=$suiteDirectory","-abslog=$engineLog")
    $argumentLine = ($arguments | ForEach-Object { Quote-MRFPSArgument $_ }) -join ' '
    Write-Host "$suite : $executable $argumentLine"
    Write-Host "Required tests ($($required.Count)): $($required -join ', ')"
    if ($DryRun) { continue }
    New-Item -ItemType Directory -Force -Path $suiteDirectory | Out-Null
    $started = [DateTime]::UtcNow
    $process = $null
    $exitCode = $null
    $errors = [Collections.Generic.List[string]]::new()
    $tests = @()
    $notExercised = @()
    try {
        # A distinct editor process is required for each Automation RunTests command.
        $process = Start-Process -FilePath $executable -ArgumentList $argumentLine -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        $processHandle = $process.Handle
        Write-Host "$suite running in PID $($process.Id)."
        while (-not $process.HasExited) { [void]$process.WaitForExit(1000) }
        $process.Refresh()
        $exitCode = $process.ExitCode
        if ($exitCode -ne 0) { $errors.Add("Editor process exited with code $exitCode.") }
        if (-not (Test-Path -LiteralPath $indexPath)) { $errors.Add('UE did not export index.json; no successful test run is inferred.') }
        elseif ((Get-Item -LiteralPath $indexPath).LastWriteTimeUtc -lt $started) { $errors.Add('Exported index.json predates this process and cannot prove this run.') }
        else {
            $export = Get-Content -Raw -LiteralPath $indexPath -Encoding UTF8 | ConvertFrom-Json
            $tests = @($export.tests | Where-Object { $_.fullTestPath -like "$filter.*" })
            foreach ($name in $required) {
                $matching = @($tests | Where-Object fullTestPath -EQ $name)
                if ($matching.Count -ne 1) { $errors.Add("Expected one result for $name, found $($matching.Count).") }
                elseif ($matching[0].state -ne 'Success' -or $matching[0].errors -gt 0) { $errors.Add("$name ended with state $($matching[0].state), errors $($matching[0].errors).") }
            }
            foreach ($test in $tests) {
                if ($test.state -ne 'Success' -or $test.errors -gt 0) { $errors.Add("Unexpected failure/unfinished result: $($test.fullTestPath).") }
                if (($test.entries | ConvertTo-Json -Depth 12 -Compress) -match 'requires[^"\r\n]*(?:FakeBackend|isolated Rust)') { $notExercised += $test.fullTestPath }
            }
            if (-not $tests.Count) { $errors.Add('The export contains zero matching tests.') }
        }
    } catch { $errors.Add($_.Exception.Message) }
    finally {
        # Teardown is restricted to this runner's process handle.
        if ($process -and -not $process.HasExited) { $process.Kill(); [void]$process.WaitForExit(5000) }
    }
    $suiteResult = [ordered]@{ Suite = $suite; Filter = $filter; ProcessId = $(if ($process) { $process.Id } else { $null }); StartedUtc = $started.ToString('o'); FinishedUtc = [DateTime]::UtcNow.ToString('o'); ExitCode = $exitCode; Passed = ($errors.Count -eq 0); RequiredTests = $required; ResultCount = $tests.Count; Results = @($tests | ForEach-Object { [ordered]@{ Name = $_.fullTestPath; State = $_.state; Errors = $_.errors; Warnings = $_.warnings } }); InteropNotExercised = $notExercised; Errors = @($errors.ToArray()); Export = $indexPath; EngineLog = $engineLog; Stdout = $stdoutPath; Stderr = $stderrPath }
    $results.Add([PSCustomObject]$suiteResult)
    [IO.File]::WriteAllText((Join-Path $suiteDirectory 'run-report.json'), ($suiteResult | ConvertTo-Json -Depth 7), (New-Object Text.UTF8Encoding($false)))
}
if ($DryRun) { return }
$failedSuites = @($results | Where-Object Passed -EQ $false)
$report = [ordered]@{ TimestampUtc = [DateTime]::UtcNow.ToString('o'); Passed = ($failedSuites.Count -eq 0); Scope = 'UE automation suite results only; physical MR/glove/MoBits output and optional external interop remain separate'; Suites = @($results.ToArray()) }
[IO.File]::WriteAllText((Join-Path $ReportDirectory 'report.json'), ($report | ConvertTo-Json -Depth 9), (New-Object Text.UTF8Encoding($false)))
if ($failedSuites.Count) { throw "Automation failed for: $(($failedSuites.Suite) -join ', '). Evidence: $ReportDirectory" }
Write-Host "Automation suites passed their required test result checks. Evidence: $ReportDirectory"
