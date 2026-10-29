$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '../../Scripts/MRFPS.Tools.psm1') -Force
$package = 'org.mrfps.arena'
$orphan = @'
  * Task{abcd #484 type=standard sz=1}
    * Task{abcd #485 type=standard rootTaskId=484 sz=1}
      * Hist  #0: ActivityRecord{dead u0 com.meta.systemui/.media.MediaProjectionPermissionActivity t485}
        launchedFromUid=10172 launchedFromPackage=org.mrfps.arena launchedFromFeature=null userId=0
        resultTo=ActivityRecord{dead u0 org.mrfps.arena/com.epicgames.unreal.GameActivity t-1 f}} resultCode=58481
'@
function Require([bool]$condition, [string]$message) { if (-not $condition) { throw $message } }
$tasks = @(Find-MRFPSOrphanedQuestShareTask -ActivityDump $orphan -Package $package)
Require ($tasks.Count -eq 1 -and $tasks[0] -eq 484) 'The MRFPS orphan root was not found.'
Require (@(Find-MRFPSOrphanedQuestShareTask -ActivityDump ($orphan -replace "`r?`n","`r`n") -Package $package).Count -eq 1) 'CRLF dumps must also find the orphan.'
Require (@(Find-MRFPSOrphanedQuestShareTask -ActivityDump ($orphan.Replace('t-1 f}}','t101}}')) -Package $package).Count -eq 0) 'A live result target must not be removed.'
Require (@(Find-MRFPSOrphanedQuestShareTask -ActivityDump ($orphan.Replace('org.mrfps.arena','other.package')) -Package $package).Count -eq 0) 'Another package must not be removed.'
Require (@(Find-MRFPSOrphanedQuestShareTask -ActivityDump ($orphan.Replace('sz=1}','sz=2}')) -Package $package).Count -eq 0) 'A multi-child root must not be removed.'
$multiple = $orphan + "`n" + $orphan.Replace('#484','#488')
Require (@(Find-MRFPSOrphanedQuestShareTask -ActivityDump $multiple -Package $package).Count -eq 2) 'Multiple matching roots must remain distinguishable.'
$crossActivity = @'
  * Task{abcd #484 type=standard sz=1}
    * Task{abcd #485 type=standard rootTaskId=484 sz=2}
      * Hist  #1: ActivityRecord{abc u0 com.meta.systemui/.media.MediaProjectionPermissionActivity t485}
        launchedFromUid=1000 launchedFromPackage=other.package launchedFromFeature=null userId=0
      * Hist  #0: ActivityRecord{def u0 com.example/.Activity t485}
        launchedFromUid=10172 launchedFromPackage=org.mrfps.arena launchedFromFeature=null userId=0
        resultTo=ActivityRecord{dead u0 org.mrfps.arena/com.epicgames.unreal.GameActivity t-1 f}} resultCode=58481
'@
Require (@(Find-MRFPSOrphanedQuestShareTask -ActivityDump $crossActivity -Package $package).Count -eq 0) 'Metadata from different activities must not be combined.'
$sameRootTwoActivities = @'
  * Task{abcd #900 type=standard sz=1}
    * Task{abcd #901 type=standard rootTaskId=900 sz=2}
      * Hist  #1: ActivityRecord{abc u0 com.example/.Activity t901}
        launchedFromUid=1000 launchedFromPackage=other.package launchedFromFeature=null userId=0
      * Hist  #0: ActivityRecord{dead u0 com.meta.systemui/.media.MediaProjectionPermissionActivity t901}
        launchedFromUid=10172 launchedFromPackage=org.mrfps.arena launchedFromFeature=null userId=0
        resultTo=ActivityRecord{dead u0 org.mrfps.arena/com.epicgames.unreal.GameActivity t-1 f}} resultCode=58481
'@
Require (@(Find-MRFPSOrphanedQuestShareTask -ActivityDump $sameRootTwoActivities -Package $package).Count -eq 0) 'A root containing a live neighboring activity must not be removed.'
Require (@(Find-MRFPSOrphanedQuestShareTask -ActivityDump ($sameRootTwoActivities.Replace('sz=2}','sz=1}')) -Package $package).Count -eq 0) 'An inconsistent single-child count must not hide a second history record.'
Require (@(Find-MRFPSOrphanedQuestShareTask -ActivityDump ($orphan.Replace('com.meta.systemui/','evil.com.meta.systemui/')) -Package $package).Count -eq 0) 'A similarly named package is not the Meta prompt.'
Require (@(Find-MRFPSOrphanedQuestShareTask -ActivityDump ($orphan.Replace('MediaProjectionPermissionActivity ','MediaProjectionPermissionActivityAlias ')) -Package $package).Count -eq 0) 'A similarly named activity is not the Meta prompt.'
Write-Host 'Quest orphan task checks: 11 passed.'
