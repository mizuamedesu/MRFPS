param([Parameter(Mandatory)][string]$ConfigPath)
$ErrorActionPreference = 'Stop'
$config = Get-Content -Raw -LiteralPath $ConfigPath -Encoding UTF8 | ConvertFrom-Json
& (Join-Path $PSScriptRoot 'Invoke-MRFPSResourceLimited.ps1') `
    -ScriptFile (Join-Path $PSScriptRoot 'Start-DeviceLoginInstance.ps1') `
    -ScriptArguments @($ConfigPath) -MinimumFreeGB 10 -MaximumCommitPercent 75 `
    -ReportPath $config.report
exit $LASTEXITCODE
