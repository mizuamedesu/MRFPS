$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$script = Join-Path $PSScriptRoot '../../Scripts/Start-LiveInspectorValidation.ps1'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $script), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Inspector script has PowerShell syntax errors.' }
$function = $ast.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Update-LiveEvidence' }, $true)
if (-not $function) { throw 'Inspector evidence function not found.' }
. ([scriptblock]::Create($function.Extent.Text))

function New-TestInstance([bool]$connected, [int]$snapshotSecondsAgo = 0) {
    $at = [DateTime]::UtcNow.AddSeconds(-$snapshotSecondsAgo)
    [PSCustomObject]@{
        LaunchedAt=$at.AddSeconds(-20); CertificateVerified=$connected; HandshakeComplete=$connected
        CertificateAt=$(if ($connected) { $at.AddSeconds(-10) } else { $null })
        HandshakeAt=$(if ($connected) { $at.AddSeconds(-9) } else { $null })
        Snapshot=$(if ($connected) { [PSCustomObject]@{ AtUtc=$at; Frame=1; Players=0; Role='inspector' } } else { $null })
        SnapshotCount=0; ShaderWarmupObserved=$false; RendererLogObserved=$false; Failure=$null; AuthSafeCode=$null
    }
}
function Require([bool]$condition, [string]$message) { if (-not $condition) { throw $message } }
$connected = New-TestInstance $true
Update-LiveEvidence $connected 'MRFPS_AUTH_CLIENT_REJECT code=AUTH_ROLE role=inspector'
Require ($null -eq $connected.Failure -and $connected.AuthSafeCode -eq 'AUTH_ROLE') 'Local menu rejection killed an active inspector.'
$startup = New-TestInstance $false
Update-LiveEvidence $startup 'MRFPS_AUTH_CLIENT_REJECT code=AUTH_ROLE role=inspector'
Require ($startup.Failure -eq 'AUTH_REJECTED') 'Initial credential rejection must still fail.'
$stale = New-TestInstance $true 10
Update-LiveEvidence $stale 'MRFPS_AUTH_CLIENT_REJECT code=AUTH_ROLE role=inspector'
Require ($stale.Failure -eq 'AUTH_REJECTED') 'A stale snapshot cannot prove an active inspector.'
$server = New-TestInstance $true
Update-LiveEvidence $server 'MRFPS_AUTH_REJECT code=AUTH_REPLAY'
Require ($server.Failure -eq 'AUTH_REJECTED') 'Server auth rejection must still fail.'
$network = New-TestInstance $true
Update-LiveEvidence $network 'MRFPS_NETWORK_FAILURE'
Require ($network.Failure -eq 'NETWORK_FAILURE') 'Network failure must still fail.'
Write-Host 'Inspector evidence checks: 5 passed.'
