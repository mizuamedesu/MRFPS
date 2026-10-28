# MRFPSTransport

This is a thin policy layer around UE5.8's OpenSSL-backed `FDTLSHandlerComponent`. It does not implement a cipher, key exchange, certificate parser, or certificate signature scheme. The native component supports DTLS 1.2 or later on Win64, Android and Unix targets, including the Linux dedicated server. Its `.uplugin` is experimental in the installed engine, so packaged builds and the real login path still require integration testing.

The server calls `EnsureServerCertificate(Fingerprint, SafeError)` before accepting any joins. The native certificate store generates an independent RSA private key and self-signed certificate in memory. The public SHA-256 fingerprint is logged as `MRFPS_DTLS_SERVER_SHA256=<64 lowercase hex>`. The local JWT issuer must bind that fingerprint in the signed `dtls_fp` claim. The server receives only the issuer's JWT verification public key, never the JWT signing private key. No DTLS private-key file or extra private-key environment variable is required.

The certificate lasts seven days. A process restart generates a new key and fingerprint; issue new local tokens for that server instance. The native PEM importer only loads the X509 certificate, not its private key, so it cannot be used as a persistent server identity without changing the engine API. Certificate expiry or fingerprint mismatch closes the handshake.

Before travel, the client must verify the local token's signature, time, audience and role using its separately provisioned issuer public key. Only then call `SetClientServerFingerprint(Claims.ServerFingerprint, SafeError)`. An invalid replacement clears the previous pin. `ClearClientServerFingerprint()` is available for failed/replaced connection attempts.

The client prepares its native certificate with the verified pin. On a cold process, the product's `MRFPSConnect` waits with a world timer until that certificate is six seconds old, then revalidates the token before starting `ClientTravel`. Epic generates `notBefore` at the device's current wall time, so this handles peers whose clocks differ by at most five seconds, including the approximately half-second difference measured on this Quest. JWT validation also has a five-second boundary tolerance; it does not guarantee synchronized clocks, so devices still need normal time synchronization. Subsequent connections reuse the activated in-memory certificate. Native certificate time, signature and fingerprint verification remain enabled. A new connect attempt replaces the timer, and its callback is weakly bound to the controller. The encryption-ack callback remains synchronous and rejects an unactivated client certificate; launch clients through the validated connect path.

Forward the `UGameInstance` encryption overrides to these exact methods:

```cpp
FMRFPSTransportModule::HandleServerEncryptionToken(EncryptionToken, Delegate);
FMRFPSTransportModule::HandleClientEncryptionAck(Delegate);
```

The plaintext initial Hello carries only `?EncryptionToken=mrfps-dtls-v1`. It must never carry a JWT. The bearer JWT is added only by the local player's `GetGameLoginOptions`, which must first require that the current pending connection has encryption enabled. This guard is necessary because UE accepts `NMT_Challenge` before `EncryptionAck`; an unauthenticated server could otherwise solicit a token in plaintext. After encryption is enabled, the strict handler rejects plaintext, so a Challenge that reaches the local player has passed native DTLS. Client identity and role authorization remain the JWT verifier's responsibility; accepting a self-signed client transport certificate does not authorize that client.

Required project settings:

```ini
[PacketHandlerComponents]
EncryptionComponent=MRFPSTransport

[SystemSettings]
net.AllowEncryption=2
DTLS.PreSharedKeys=0
```

The game instance must reject encryption failures and the game mode must deny all login attempts when transport initialization or its required configuration is missing. In particular, the native `UNetDriver::IsEncryptionRequired()` also checks `DoesSupportEncryption()`, so `net.AllowEncryption=2` alone does not prove a handler was actually loaded. Do not provide a plaintext development bypass.

After negotiation is accepted, this handler rejects incoming plaintext, including after a native context failure. The native handler initially leaves the server in `Unencrypted` until the client's first DTLS record arrives. The wrapper buffers those outgoing application packets in a bounded queue, and releases them through native DTLS once encrypted output is possible. It never reopens a plaintext path. The initial nonsecret `EncryptionAck` is sent by the engine before enabling encryption and remains permitted.

The wrapper checks the two native framing flags and minimum payload length before calling the native reader, and immediately advances native handshake processing after an incoming handshake record. This lets an application record follow the final handshake flight in the same network dispatch batch without being dropped while the native state is waiting for the next Tick. All certificate verification and record processing still use the engine's DTLS implementation.

UE5.8 logs a full Login URL in `World.cpp` before `PreLogin`. Therefore the root application's logging policy must suppress credential-bearing engine categories before login, and a real negative-test harness must scan logs for JWT leakage. This plugin never logs token values, credentials, packet payloads or private keys.

Automation tests:

- `MRFPS.Transport.NoPlaintextDowngrade`: fingerprint shape, malformed input, and post-negotiation plaintext rejection.
- `MRFPS.Transport.NativeDTLSHandshake`: two actual native PacketHandlers exchange DTLS records in memory, recover a deliberately lost first ClientHello using the native retransmission timer, release the held application packet, decrypt it at the peer, and reject plaintext and truncated encrypted packets. It uses the configured production handler and does not replace native cryptography with a fixture. The loss test waits for real elapsed time because OpenSSL's timer does not use the engine Tick delta.

The test uses UE's `MAX_PACKET_SIZE` connection MTU (1024 bytes in this engine), matching `UIpConnection` and the native DTLS temporary buffer. It also asserts that every captured native datagram fits that limit. Increasing the fixture's MTU independently can make OpenSSL produce certificate fragments larger than the native handler's fixed buffer.

Compilation or these tests alone do not prove the full auth deployment. The real server/client harness must additionally check valid and invalid roles, signature/time/audience failures, replay rejection, mismatched server fingerprint, plaintext client rejection, and absence of bearer tokens in captures and logs.

Primary API references: [Epic encryption callback](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UGameInstance/ReceivedNetworkEncryptionToken), [native DTLS module](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/DTLSHandlerComponent/FDTLSHandlerComponentModule), [encryption component API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/PacketHandler/FEncryptionComponent). Behavior above was also checked against the installed `Engine/Plugins/Runtime/PacketHandlers/DTLSHandlerComponent/Source` and `Engine/Source/Runtime/Engine/Private/{PendingNetGame,NetConnection,World,NetDriver}.cpp`.
