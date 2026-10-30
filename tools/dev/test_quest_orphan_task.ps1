$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '../../Scripts/MRFPS.Tools.psm1') -Force
$empty = @'
  * Task{abcd #100 type=home sz=1}
    * Hist  #0: ActivityRecord{beef u0 com.example/.Home t100}
'@
$prompt = @'
  * Task{abcd #484 type=standard sz=1}
    * Task{abcd #485 type=standard rootTaskId=484 sz=1}
      * Hist  #0: ActivityRecord{dead u0 com.meta.systemui/.media.MediaProjectionPermissionActivity t485}
        launchedFromUid=10172 launchedFromPackage=org.mrfps.arena userId=0
        resultTo=ActivityRecord{dead u0 org.mrfps.arena/com.epicgames.unreal.GameActivity t-1 f}}
'@
function RequireBlocked([string]$dump,[string]$expected) {
    $blocked=$false
    try { Assert-MRFPSQuestSharePermissionClear -ActivityDump $dump }
    catch { if ($_.Exception.Message -notmatch $expected) { throw }; $blocked=$true }
    if (-not $blocked) { throw 'Expected unsafe or ambiguous activity dump to be rejected.' }
}
Assert-MRFPSQuestSharePermissionClear -ActivityDump $empty
Assert-MRFPSQuestSharePermissionClear -ActivityDump ($empty -replace "`r?`n","`r`n")
foreach ($dump in @($prompt,($prompt.Replace('t-1 f}}','t101}}')),
                    ($prompt.Replace('org.mrfps.arena','other.package')),
                    'unparsed MediaProjectionPermissionActivity','GrantPermissionsActivity')) {
    RequireBlocked $dump 'permission prompt'
}
foreach ($dump in @('', 'error: device offline',($empty+"`n"+$empty))) {
    RequireBlocked $dump 'activity roots'
}
Write-Host 'Quest permission inspection checks passed.'
