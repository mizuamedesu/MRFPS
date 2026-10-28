[CmdletBinding()]
param([switch]$WslContainer, [string]$WslDistribution = 'Ubuntu')
$ErrorActionPreference = 'Stop'
# This regression sends actual UDP datagrams through a separate relay process.
$backendPort = 27778
$relayPort = 27779
$backend = $null
$containerName = $null
$upstreamAddress = '127.0.0.1'
if ($WslContainer) {
    $fixture = Join-Path $PSScriptRoot 'udp_echo_fixture.py'
    $linuxFixture = (& wsl -d $WslDistribution -- wslpath -a $fixture.Replace('\','/') | Out-String).Trim()
    $upstreamAddress = ((& wsl -d $WslDistribution -- hostname -I | Out-String).Trim() -split '\s+')[0]
    $containerName = 'mrfps-relay-test-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
    & wsl -d $WslDistribution -u root -- docker run -d --name $containerName -p "$($backendPort):$backendPort/udp" --mount "type=bind,source=$linuxFixture,target=/fixture.py,readonly" mrfps-runtime-base:local python3 /fixture.py
    if ($LASTEXITCODE -ne 0) { throw 'Could not start the WSL UDP echo fixture container.' }
} else {
    $backend = Start-Job -ScriptBlock {
    $socket = [Net.Sockets.UdpClient]::new(27778)
    $socket.Client.ReceiveTimeout = 10000
    try {
        for ($i = 0; $i -lt 2; $i++) {
            $endpoint = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
            $data = $socket.Receive([ref]$endpoint)
            $response = [Text.Encoding]::UTF8.GetBytes(([Text.Encoding]::UTF8.GetString($data)) + ':' + $endpoint.Port)
            [void]$socket.Send($response, $response.Length, $endpoint)
        }
    } finally { $socket.Dispose() }
    }
}
$scriptPath = Join-Path $PSScriptRoot 'Start-QuestRelay.ps1'
$relay = $null
$statusFile = Join-Path (Split-Path -Parent $PSScriptRoot) ('Saved\Validation\quest-relay-test-' + [Guid]::NewGuid().ToString('N') + '.json')
$clients = @()
try {
    $relay = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`" -TargetAddress $upstreamAddress -TargetPort $backendPort -BindAddress 127.0.0.1 -BindPort $relayPort -RunSeconds 20 -StatusFile `"$statusFile`"" -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 3
    $responses = @()
    for ($i = 0; $i -lt 2; $i++) {
        $client = [Net.Sockets.UdpClient]::new()
        $clients += $client
        $client.Client.ReceiveTimeout = 5000
        $client.Connect('127.0.0.1', $relayPort)
        $payload = [Text.Encoding]::UTF8.GetBytes("client$i")
        [void]$client.Send($payload, $payload.Length)
        $endpoint = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
        $responses += [Text.Encoding]::UTF8.GetString($client.Receive([ref]$endpoint))
    }
    if ($responses[0] -notmatch '^client0:(\d+)$') { throw 'Client 0 received the wrong payload.' }
    $firstUpstreamPort = $Matches[1]
    if ($responses[1] -notmatch '^client1:(\d+)$') { throw 'Client 1 received the wrong payload.' }
    if ($Matches[1] -eq $firstUpstreamPort) { throw 'Relay merged two clients into one upstream connection.' }
    Write-Host "Quest relay integration passed: $($responses -join ', ')"
} finally {
    foreach ($client in $clients) { $client.Dispose() }
    if ($relay -and -not $relay.HasExited) { $relay.Kill(); [void]$relay.WaitForExit(5000) }
    if ($backend) {
        if ($backend.State -eq 'Running') { Stop-Job -Job $backend }
        Receive-Job -Job $backend -ErrorAction SilentlyContinue
        Remove-Job -Job $backend -Force
    }
    if ($containerName) { & wsl -d $WslDistribution -u root -- docker rm -f $containerName | Out-Null }
}
