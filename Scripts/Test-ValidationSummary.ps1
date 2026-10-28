[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$fixtureRoot = Join-Path (Get-MRFPSRoot) ('Saved\Validation\SummaryFixtures-' + [Guid]::NewGuid().ToString('N'))
$validation = Join-Path $fixtureRoot 'Saved\Validation'
$sourceDirectory = Join-Path $fixtureRoot 'Source\MRFPS'
$buildDirectory = Join-Path $fixtureRoot 'Saved\BuildLogs'
$automationDirectory = Join-Path $validation 'Automation-fixture'
$multiplayerDirectory = Join-Path $validation 'Multiplayer-fixture'
foreach ($directory in @($sourceDirectory,$buildDirectory,$automationDirectory,$multiplayerDirectory)) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
$project = Join-Path $fixtureRoot 'MRFPS.uproject'
[IO.File]::WriteAllText($project, '{}')
$source = Join-Path $sourceDirectory 'Fixture.cpp'
[IO.File]::WriteAllText($source, 'IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExample, "MRFPS.Core.Fixture", Flags)')
(Get-Item -LiteralPath $source).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-1)
$script = Join-Path $PSScriptRoot 'Get-ValidationSummary.ps1'
$assertions = [Collections.Generic.List[string]]::new()
function Read-FixtureSummary { (& $script -ProjectRoot $fixtureRoot -NoWrite -SkipProcessInspection) | ConvertFrom-Json }
function Assert-Status([object]$Summary, [string]$Id, [string]$Expected) {
    $gate = $Summary.Gates | Where-Object Id -EQ $Id
    if ($gate.Status -ne $Expected) { throw "$Id expected $Expected, got $($gate.Status)." }
    if ($Summary.FullObjectiveVerified -ne $false -or $Summary.OverallStatus -ne 'incomplete') { throw 'The fixture improperly became a full-objective pass.' }
    $assertions.Add("$Id=$Expected, full objective remains unverified")
}

$unrelatedLog = Join-Path $buildDirectory 'Unrelated.log'
[IO.File]::WriteAllText($unrelatedLog, "Command line: MRFPSEditor Win64 Development -Project=C:/different/project/MRFPS.uproject`nResult: Succeeded")
Assert-Status (Read-FixtureSummary) 'EditorBuild' 'pending'

$automationFile = Join-Path $automationDirectory 'index.json'
[IO.File]::WriteAllText($automationFile, '{"tests":[{"fullTestPath":"MRFPS.Other.Subset","state":"Success"}]}')
Assert-Status (Read-FixtureSummary) 'CoreAutomation' 'unverified'
[IO.File]::WriteAllText($automationFile, '{"tests":[{"fullTestPath":"MRFPS.Core.Fixture","state":"Success"}]}')
Assert-Status (Read-FixtureSummary) 'CoreAutomation' 'passed'
[IO.File]::WriteAllText($automationFile, '{"tests":[{"fullTestPath":"MRFPS.Core.Fixture","state":"Fail"}]}')
Assert-Status (Read-FixtureSummary) 'CoreAutomation' 'failed'

$multiplayerFile = Join-Path $multiplayerDirectory 'report.json'
[IO.File]::WriteAllText($multiplayerFile, '{"Passed":true,"InspectorCount":2,"Checks":{},"Processes":[]}')
Assert-Status (Read-FixtureSummary) 'DesktopMultiplayer' 'unverified'
[IO.File]::WriteAllText($multiplayerFile, '{"Passed":false,"Failure":"fixture-only failure"}')
Assert-Status (Read-FixtureSummary) 'DesktopMultiplayer' 'failed'

(Get-Item -LiteralPath $source).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-1)
$mrSuite = Join-Path $validation 'Automation\MRFPS'
$sdkSuite = Join-Path $validation 'Automation\MoBitsSDK'
New-Item -ItemType Directory -Force -Path $mrSuite,$sdkSuite | Out-Null
[IO.File]::WriteAllText((Join-Path $mrSuite 'index.json'), '{"tests":[{"fullTestPath":"MRFPS.Core.Fixture","state":"Success"}]}')
[IO.File]::WriteAllText((Join-Path $sdkSuite 'index.json'), '{"tests":[{"fullTestPath":"MoBits.SDK.Contracts","state":"Fail"}]}')
Assert-Status (Read-FixtureSummary) 'CoreAutomation' 'passed'

$editorLog = Join-Path $buildDirectory 'Editor.log'
[IO.File]::WriteAllText($editorLog, "Command line: MRFPSEditor Win64 Development -Project=$project`nResult: Succeeded")
(Get-Item -LiteralPath $source).LastWriteTimeUtc = [DateTime]::UtcNow.AddSeconds(1)
Assert-Status (Read-FixtureSummary) 'EditorBuild' 'stale'
[IO.File]::WriteAllText($editorLog, "Command line: MRFPSEditor Win64 Development -Project=$project`n[100/200] Compile ongoing")
Assert-Status (Read-FixtureSummary) 'EditorBuild' 'unverified'

[IO.File]::WriteAllText((Join-Path $validation 'physical-acceptance.json'), '{"Passed":true}')
Assert-Status (Read-FixtureSummary) 'MoBitsFiringAndHitPhysicalOutput' 'needs_review'

$nestedFixture = Join-Path $validation 'SummaryFixtures-nested\Multiplayer-pretend'
New-Item -ItemType Directory -Force -Path $nestedFixture | Out-Null
[IO.File]::WriteAllText((Join-Path $nestedFixture 'report.json'), '{"Passed":true}')
Assert-Status (Read-FixtureSummary) 'DesktopMultiplayer' 'failed'

$report = [ordered]@{ TimestampUtc = [DateTime]::UtcNow.ToString('o'); Passed = $true; Scope = 'Validation-summary parser fixtures only; no engine/game/device runtime executed'; Assertions = @($assertions.ToArray()) }
[IO.File]::WriteAllText((Join-Path $fixtureRoot 'assertions.json'), ($report | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
Write-Host "Validation-summary fixtures passed ($($assertions.Count) assertions). Evidence: $fixtureRoot"
