[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Python,
    [Parameter(Mandatory)][string]$Editor,
    [Parameter(Mandatory)][string]$Project,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$GuardReport
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
foreach($file in @($Python,$Editor,$Project,$GuardReport)) {
    if(-not (Test-Path -LiteralPath $file -PathType Leaf)){throw 'TLS_FIXTURE_INPUT_MISSING'}
}
# tls_fixture.py refuses to launch the editor outside a Windows job and a
# live 10-GiB/75%-guard report. Invoke through Invoke-MRFPSResourceLimited.ps1.
& $Python (Join-Path $root 'tools\auth\tls_fixture.py') run --editor ([IO.Path]::GetFullPath($Editor)) `
    --project ([IO.Path]::GetFullPath($Project)) --report-dir ([IO.Path]::GetFullPath($OutputDirectory)) `
    --guard-report ([IO.Path]::GetFullPath($GuardReport))
exit $LASTEXITCODE
