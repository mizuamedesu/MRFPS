[CmdletBinding()]
param(
    [string]$ProtectedTokenFile = (Join-Path $env:LOCALAPPDATA 'MRFPS\Admin\cloudflare-api-token.dpapi'),
    [switch]$EnableWorkflow
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $ProtectedTokenFile -PathType Leaf)) { throw 'Protected token file is not present.' }
# ReadAllText auto-detects UTF-16LE/UTF-8 BOM (Windows PowerShell 5.1 and 7).
$protected = [System.IO.File]::ReadAllText($ProtectedTokenFile).Trim()
if (-not $protected -or $protected.Length -gt 8192) { throw 'Protected token file has an invalid size.' }
try { $secure = ConvertTo-SecureString -String $protected } catch { throw 'The protected token cannot be decrypted by this Windows user.' }
$ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try {
    $token = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
    if ($token.Length -lt 32 -or $token.Length -gt 512 -or $token -match '\s') { throw 'The decrypted token has an invalid format.' }
    $accountList = & wrangler whoami 2>$null | Out-String
    $match = [regex]::Match($accountList, '(?<![0-9a-f])[0-9a-f]{32}(?![0-9a-f])')
    if ($LASTEXITCODE -ne 0 -or -not $match.Success) { throw 'Could not identify the Wrangler account.' }
    $accountId = $match.Value
    $authHeader = @{ Authorization = 'Bearer ' + $token }
    $verified = $false
    foreach ($verifyUri in @('https://api.cloudflare.com/client/v4/user/tokens/verify', "https://api.cloudflare.com/client/v4/accounts/$accountId/tokens/verify")) {
        try {
            $result = Invoke-RestMethod -Uri $verifyUri -Headers $authHeader -TimeoutSec 15
            if ($result.success -and $result.result.status -eq 'active') { $verified = $true; break }
        } catch { }
    }
    if (-not $verified) { throw 'Cloudflare did not verify the API token as active.' }
    try {
        $worker = Invoke-WebRequest -Uri "https://api.cloudflare.com/client/v4/accounts/$accountId/workers/scripts/mrfps-admin" -Headers $authHeader -TimeoutSec 15 -UseBasicParsing
        if ($worker.StatusCode -ne 200) { throw 'not available' }
    } catch { throw 'The token is active but cannot read the MRFPS Worker. Check account and Worker permissions.' }
    $gh = (Get-Command gh -CommandType Application -ErrorAction Stop).Source
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $process.StartInfo.FileName = $gh
    $process.StartInfo.Arguments = 'secret set CLOUDFLARE_API_TOKEN'
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardInput = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    [void]$process.Start()
    $process.StandardInput.Write($token)
    $process.StandardInput.Close()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw 'GitHub secret update failed.' }
    if ($EnableWorkflow) {
        & $gh variable set MRFPS_ADMIN_CI_DEPLOY --body true
        if ($LASTEXITCODE -ne 0) { throw 'GitHub workflow variable update failed.' }
    }
    Write-Output 'Cloudflare API token verified and stored in GitHub Actions secrets.'
} finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
    $token = $null
    $authHeader = $null
    if ($secure) { $secure.Dispose() }
}
