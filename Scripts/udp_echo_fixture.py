"""A short-lived UDP fixture for Test-QuestRelay.ps1 -WslContainer."""
import socket

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
    server.settimeout(30)
    server.bind(('0.0.0.0', 27778))
    for _ in range(2):
        data, peer = server.recvfrom(65535)
        server.sendto(data + b':' + str(peer[1]).encode('ascii'), peer)
