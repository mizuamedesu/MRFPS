# Quest login pointer: real Slate fixture

`QuestVisualFixture.LoginPointerCancel` uses an isolated test world, a real
`UWidgetComponent`, Slate virtual pointer captures, and `UMRFPSLoginInteraction`.
It checks a normal click, cancellation while pressed, a delayed release,
recovery, and isolation from another interaction's capture. It creates no
network connection, does not operate the Quest, and does not touch the real
login widget or game settings. It is not a Quest input or rendering test.

This test must use a real RHI: UE does not create the world-widget renderer
under `-NullRHI`. It is deliberately outside the ordinary `MRFPS` automation
filter. First build the Editor module from the same worktree. Then run a fresh
guarded test from the project root:

```powershell
$taskId = [Guid]::NewGuid().ToString('N')
$run = Join-Path (Get-Location) ('Saved\Validation\LoginSlate-' + $taskId)
$guard = Join-Path $run 'guard\report.json'
$testOutput = Join-Path $run 'automation'
$fixtureArguments = @('-ReportDirectory', $testOutput, '-GuardReport', $guard)
& .\Scripts\Invoke-MRFPSResourceLimited.ps1 `
  -ScriptFile .\Scripts\Test-LoginSlate.ps1 -ScriptArguments $fixtureArguments `
  -MinimumFreeGB 10 -MaximumCommitPercent 75 -ReportPath $guard
```

The script requires a live resource-guard report and Windows job, a fresh
output directory, and exactly one successful test in the exported report. A
missing renderer, Slate, test, report, or hit-test grid fails the run. Do not
run another heavy build or test concurrently. An Editor result does not prove
that Quest hardware tracking, controller aim, or its OS UI works.
