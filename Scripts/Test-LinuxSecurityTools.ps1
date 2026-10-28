# Lightweight local-only unit/mock tests; invoke through the resource guard.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root=Get-MRFPSRoot
$tokens=$null; $parseErrors=$null
[void][Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Test-LinuxSecurity.ps1'),[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'LINUX_SECURITY_POWERSHELL_PARSE_FAILED'}
Invoke-MRFPSCommand (Join-Path $root '.local/auth-tools/Scripts/python.exe') @('-m','unittest','discover','-s',(Join-Path $root 'tools/auth'),'-p','test_linux_security_probe.py','-v')
Invoke-MRFPSCommand (Join-Path $root '.local/auth-tools/Scripts/python.exe') @('-m','unittest','discover','-s',(Join-Path $root 'tools/auth'),'-p','test_linux_multiplayer_probe.py','-v')
foreach($script in @('Test-LinuxMultiplayer.ps1','Start-LinuxContainer.ps1')){
    $tokens=$null; $parseErrors=$null
    [void][Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $script),[ref]$tokens,[ref]$parseErrors)
    if($parseErrors.Count){throw 'LINUX_DEPLOYMENT_POWERSHELL_PARSE_FAILED'}
}
