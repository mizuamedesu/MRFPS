[CmdletBinding()]
param(
    [string]$TargetAddress,
    [ValidateRange(1,65535)][int]$TargetPort = 7777,
    [string]$BindAddress = '0.0.0.0',
    [ValidateRange(1,65535)][int]$BindPort = 7777,
    [string]$WslDistribution = 'Ubuntu',
    [ValidateRange(0,86400)][int]$RunSeconds = 0,
    [string]$StatusFile
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
if (-not $TargetAddress) {
    $addresses = (& wsl -d $WslDistribution -- hostname -I | Out-String).Trim() -split '\s+'
    if ($LASTEXITCODE -ne 0 -or -not $addresses[0]) { throw 'Cannot discover WSL address; supply -TargetAddress.' }
    $TargetAddress = $addresses[0]
}
$targetIp = @([Net.Dns]::GetHostAddresses($TargetAddress) | Where-Object AddressFamily -EQ InterNetwork | Select-Object -First 1)
if ($targetIp.Count -ne 1) { throw 'The relay requires an IPv4 upstream address.' }
if (-not $StatusFile) { $StatusFile = Join-Path (Get-MRFPSRoot) 'Saved\Validation\quest-relay.json' }
$StatusFile = [IO.Path]::GetFullPath($StatusFile)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $StatusFile) | Out-Null
if (-not ('MRFPS.Network.QuestUdpRelay' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;

namespace MRFPS.Network {
    public sealed class QuestUdpRelay : IDisposable {
        private sealed class Peer {
            public UdpClient Socket;
            public IPEndPoint Endpoint;
            public DateTime LastActive;
        }
        private readonly UdpClient listener;
        private readonly IPEndPoint target;
        private readonly Dictionary<string, Peer> peers = new Dictionary<string, Peer>();
        public long ForwardedPackets { get; private set; }
        public long ReturnedPackets { get; private set; }
        public long DroppedPackets { get; private set; }
        public int SessionCount { get { return peers.Count; } }
        public QuestUdpRelay(IPAddress bindAddress, int bindPort, IPAddress targetAddress, int targetPort) {
            target = new IPEndPoint(targetAddress, targetPort);
            listener = new UdpClient(new IPEndPoint(bindAddress, bindPort));
            listener.Client.ReceiveBufferSize = 4 * 1024 * 1024;
        }
        public void Pump() {
            int processed = 0;
            while (listener.Available > 0 && processed++ < 256) {
                IPEndPoint endpoint = new IPEndPoint(IPAddress.Any, 0);
                byte[] data = listener.Receive(ref endpoint);
                string key = endpoint.ToString();
                Peer peer;
                if (!peers.TryGetValue(key, out peer)) {
                    if (peers.Count >= 128) { DroppedPackets++; continue; }
                    var socket = new UdpClient(AddressFamily.InterNetwork);
                    socket.Client.ReceiveBufferSize = 1024 * 1024;
                    socket.Connect(target);
                    peer = new Peer { Socket = socket, Endpoint = new IPEndPoint(endpoint.Address, endpoint.Port), LastActive = DateTime.UtcNow };
                    peers.Add(key, peer);
                }
                try {
                    peer.Socket.Send(data, data.Length);
                    peer.LastActive = DateTime.UtcNow;
                    ForwardedPackets++;
                } catch (SocketException) { DroppedPackets++; }
            }
            var expired = new List<string>();
            foreach (var pair in peers) {
                Peer peer = pair.Value;
                processed = 0;
                try {
                    while (peer.Socket.Available > 0 && processed++ < 256) {
                        IPEndPoint remote = target;
                        byte[] data = peer.Socket.Receive(ref remote);
                        listener.Send(data, data.Length, peer.Endpoint);
                        peer.LastActive = DateTime.UtcNow;
                        ReturnedPackets++;
                    }
                } catch (SocketException) { DroppedPackets++; }
                if ((DateTime.UtcNow - peer.LastActive).TotalSeconds > 180) { expired.Add(pair.Key); }
            }
            foreach (string key in expired) { peers[key].Socket.Close(); peers.Remove(key); }
        }
        public void Dispose() {
            listener.Close();
            foreach (var peer in peers.Values) { peer.Socket.Close(); }
            peers.Clear();
        }
    }
}
'@
}
$relay = [MRFPS.Network.QuestUdpRelay]::new([Net.IPAddress]::Parse($BindAddress), $BindPort, $targetIp[0], $TargetPort)
$started = [DateTime]::UtcNow
$nextReport = $started
Write-Host "MRFPS UDP relay: $($BindAddress):$BindPort -> $($targetIp[0]):$TargetPort. Stop this process to close the relay."
try {
    while ($RunSeconds -eq 0 -or ([DateTime]::UtcNow - $started).TotalSeconds -lt $RunSeconds) {
        $relay.Pump()
        if ([DateTime]::UtcNow -ge $nextReport) {
            $state = [ordered]@{ TimestampUtc = [DateTime]::UtcNow.ToString('o'); ProcessId = $PID; Bind = "$($BindAddress):$BindPort"; Target = "$($targetIp[0]):$TargetPort"; Sessions = $relay.SessionCount; ForwardedPackets = $relay.ForwardedPackets; ReturnedPackets = $relay.ReturnedPackets; DroppedPackets = $relay.DroppedPackets }
            [IO.File]::WriteAllText($StatusFile, ($state | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
            $nextReport = [DateTime]::UtcNow.AddSeconds(1)
        }
        Start-Sleep -Milliseconds 2
    }
} finally { $relay.Dispose() }
