[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Ensure','Status','Stop')][string]$Action,
    [string]$Target,
    [ValidateRange(15,120)][int]$RateHz = 60,
    [ValidateRange(1024,65535)][int]$ListenPort = 25789,
    [string]$Python
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This relay manager currently requires Windows.' }
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).ProviderPath
$bridge = (Resolve-Path -LiteralPath (Join-Path $root 'tools\gloves\contactglove_bridge.py')).ProviderPath
if (-not $Python) {
    Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
    $Python = Join-Path (Resolve-MRFPSEngine) 'Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
}
$pythonPath = (Resolve-Path -LiteralPath $Python).ProviderPath
$stateDir = Join-Path $root 'Saved\GloveRelay'
New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
$statePath = Join-Path $stateDir 'state.json'
$lockPath = Join-Path $stateDir 'state.lock'
$lock = $null
for ($attempt = 0; $attempt -lt 40 -and -not $lock; $attempt++) {
    try { $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
    catch [IO.IOException] { Start-Sleep -Milliseconds 50 }
}
if (-not $lock) { throw 'Relay manager is busy; no process was changed.' }
try {
    $state = $null
    if (Test-Path -LiteralPath $statePath) {
        $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        if ($state.schema -ne 1 -or $state.pid -le 0 -or -not $state.createdTicks) { throw 'Invalid relay state; no process was changed.' }
    }
    $process = $null
    if ($state) {
        $process = Get-Process -Id ([int]$state.pid) -ErrorAction SilentlyContinue
        if ($process) {
            $createdTicks = $process.StartTime.ToUniversalTime().Ticks
            $details = Get-CimInstance Win32_Process -Filter "ProcessId = $($state.pid)"
            if ($createdTicks -ne [long]$state.createdTicks -or $process.Path -ne $state.python -or
                -not $details -or -not $details.CommandLine.Contains($state.bridge)) {
                throw 'Relay process identity changed; no process was changed.'
            }
        }
    }
    if ($Action -eq 'Status') {
        $listening = $false
        if ($process) { $listening = [bool]@(Get-NetUDPEndpoint -LocalPort ([int]$state.listenPort) -ErrorAction SilentlyContinue | Where-Object OwningProcess -eq $process.Id).Count }
        [ordered]@{ running = [bool]$process; listening = $listening; pid = if ($process) { $process.Id } else { $null }; target = if ($state) { $state.target } else { $null }; listenPort = if ($state) { $state.listenPort } else { $null } } | ConvertTo-Json -Compress
        return
    }
    if ($Action -eq 'Stop') {
        if ($process) {
            # The Process object is bound to the previously verified creation
            # time. Never search for and stop another application by name.
            $process.Kill()
            if (-not $process.WaitForExit(5000)) { throw 'Relay did not exit.' }
        }
        if (Test-Path -LiteralPath $statePath) { Remove-Item -LiteralPath $statePath }
        [ordered]@{ running = $false; status = 'stopped' } | ConvertTo-Json -Compress
        return
    }
    $address = $null
    if (-not [Net.IPAddress]::TryParse($Target, [ref]$address) -or $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
        $address.Equals([Net.IPAddress]::Any) -or $address.Equals([Net.IPAddress]::Broadcast) -or $address.GetAddressBytes()[0] -ge 224) {
        throw 'Ensure requires a unicast IPv4 -Target.'
    }
    $Target = $address.ToString()
    if ($process) {
        if ($state.target -ne $Target -or $state.rateHz -ne $RateHz -or $state.listenPort -ne $ListenPort -or $state.bridge -ne $bridge -or $state.python -ne $pythonPath) {
            throw 'The owned relay has different settings; stop it explicitly first.'
        }
        if (-not @(Get-NetUDPEndpoint -LocalPort $ListenPort -ErrorAction SilentlyContinue | Where-Object OwningProcess -eq $process.Id).Count) {
            throw 'The owned relay is running but is not listening; inspect its logs before stopping it.'
        }
        [ordered]@{ running = $true; status = 'already_running'; pid = $process.Id; target = $Target; listenPort = $ListenPort } | ConvertTo-Json -Compress
        return
    }
    $portOwner = @(Get-NetUDPEndpoint -LocalPort $ListenPort -ErrorAction SilentlyContinue)
    if ($portOwner.Count) { throw "UDP port $ListenPort is already in use; no process was changed." }
    $tag = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff')
    $stdout = Join-Path $stateDir ("relay-$tag.stdout.log")
    $stderr = Join-Path $stateDir ("relay-$tag.stderr.log")
    $arguments = @('-u', ('"' + $bridge + '"'), '--target', $Target, '--listen-port', $ListenPort.ToString(), '--rate-hz', $RateHz.ToString())
    $process = Start-Process -FilePath $pythonPath -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $newState = [ordered]@{ schema = 1; pid = $process.Id; createdTicks = $process.StartTime.ToUniversalTime().Ticks; python = $pythonPath; bridge = $bridge; target = $Target; listenPort = $ListenPort; rateHz = $RateHz; stdout = $stdout; stderr = $stderr }
    $newState | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding UTF8
    $listening = $false
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        if ($process.HasExited) { throw "Relay exited before binding; see $stderr." }
        if (@(Get-NetUDPEndpoint -LocalPort $ListenPort -ErrorAction SilentlyContinue | Where-Object OwningProcess -eq $process.Id).Count) { $listening = $true; break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $listening) { throw "Relay did not bind UDP port $ListenPort; see $stderr." }
    [ordered]@{ running = $true; status = 'started'; pid = $process.Id; target = $Target; listenPort = $ListenPort; log = $stderr } | ConvertTo-Json -Compress
} finally {
    $lock.Dispose()
}
