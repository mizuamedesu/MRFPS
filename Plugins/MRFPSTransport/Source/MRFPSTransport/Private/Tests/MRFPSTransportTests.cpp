#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "MRFPSTransportPolicy.h"
#include "MRFPSTransportModule.h"
#include "EncryptionComponent.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "UObject/CoreNet.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSTransportPolicyTest, "MRFPS.Transport.NoPlaintextDowngrade",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMRFPSTransportPolicyTest::RunTest(const FString& Parameters)
{
    TArray<uint8> Pin;
    TestTrue(TEXT("SHA-256 hex decodes to 32 bytes"), MRFPSTransportPolicy::ParseFingerprint(TEXT("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"), Pin));
    TestEqual(TEXT("Fingerprint byte count"), Pin.Num(), 32);
    TestFalse(TEXT("Short fingerprints rejected"), MRFPSTransportPolicy::ParseFingerprint(TEXT("abcd"), Pin));
    TestEqual(TEXT("Invalid replacement clears prior trust"), Pin.Num(), 0);
    TestFalse(TEXT("Nonhex cannot become certificate pin"), MRFPSTransportPolicy::ParseFingerprint(FString::ChrN(64, TEXT('x')), Pin));
    const uint8 Clear[] = { 0 };
    FBitReader Before(Clear, 8);
    TestTrue(TEXT("Initial nonsecret Hello/Ack allowed before encryption"), MRFPSTransportPolicy::MayAcceptIncoming(false, Before));
    TestFalse(TEXT("Plaintext rejected after negotiation starts"), MRFPSTransportPolicy::MayAcceptIncoming(true, Before));
    const uint8 Cipher[] = { 1, 0 };
    FBitReader After(Cipher, 16);
    TestTrue(TEXT("Encrypted record passed to native DTLS verification"), MRFPSTransportPolicy::MayAcceptIncoming(true, After));
    FBitReader TruncatedHeader(Cipher, 1);
    TestFalse(TEXT("Truncated native header rejected before the native reader"), MRFPSTransportPolicy::MayAcceptIncoming(true, TruncatedHeader));
    FBitReader Empty(nullptr, 0);
    TestFalse(TEXT("Malformed empty packet rejected"), MRFPSTransportPolicy::MayAcceptIncoming(true, Empty));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSNativeDTLSHandshakeTest, "MRFPS.Transport.NativeDTLSHandshake",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMRFPSNativeDTLSHandshakeTest::RunTest(const FString& Parameters)
{
    FString Fingerprint;
    FString Error;
    if (!TestTrue(TEXT("Native server certificate available"), FMRFPSTransportModule::EnsureServerCertificate(Fingerprint, Error))) return false;
    if (!TestTrue(TEXT("Trusted test pin accepted"), FMRFPSTransportModule::SetClientServerFingerprint(Fingerprint, Error))) return false;
    ON_SCOPE_EXIT { FMRFPSTransportModule::ClearClientServerFingerprint(); };
    // Match the product's pre-travel wait without reentering the global ticker
    // (which also runs the AutomationWorker). Native Ack stays synchronous.
    const double ActivationDeadline = FPlatformTime::Seconds() + 8.0;
    while (FMRFPSTransportModule::GetClientCertificateActivationDelay() > 0.0 && FPlatformTime::Seconds() < ActivationDeadline)
        FPlatformProcess::SleepNoStats(0.01f);
    if (!TestEqual(TEXT("Client certificate activates before opening the connection"), FMRFPSTransportModule::GetClientCertificateActivationDelay(), 0.0)) return false;

    PacketHandler Server;
    PacketHandler Client;
    // Match UIpConnection's production limit. Native DTLS also sizes its temporary
    // certificate-flight buffer to MAX_PACKET_SIZE, rather than an arbitrary UDP MTU.
    constexpr int32 WireMtu = MAX_PACKET_SIZE;
    Server.Initialize(UE::Handler::Mode::Server, WireMtu * 8);
    Client.Initialize(UE::Handler::Mode::Client, WireMtu * 8);
    const TSharedPtr<FEncryptionComponent> ServerCipher = Server.GetEncryptionComponent();
    const TSharedPtr<FEncryptionComponent> ClientCipher = Client.GetEncryptionComponent();
    if (!TestTrue(TEXT("Configured server encryption component exists"), ServerCipher.IsValid())
        || !TestTrue(TEXT("Configured client encryption component exists"), ClientCipher.IsValid())) return false;

    struct FWirePacket { TArray<uint8> Bytes; bool bToServer = false; };
    TArray<FWirePacket> Wire;
    bool bOnlyEncryptedRecords = true;
    bool bWithinProductionMtu = true;
    bool bDroppedInitialClientHandshake = false;
    const auto Capture = [&](bool bToServer, void* Data, int32 Bits)
    {
        if (Bits <= 0) return;
        FWirePacket Packet;
        Packet.Bytes.Append(static_cast<const uint8*>(Data), (Bits + 7) / 8);
        Packet.bToServer = bToServer;
        bOnlyEncryptedRecords &= (Packet.Bytes[0] & 1) != 0;
        bWithinProductionMtu &= Packet.Bytes.Num() <= WireMtu;
        if (bToServer && !bDroppedInitialClientHandshake && (Packet.Bytes[0] & 3) == 3)
        {
            // Deliberately lose the first real ClientHello. The native DTLS timer,
            // rather than a fabricated transport fixture, must recover the loss.
            bDroppedInitialClientHandshake = true;
            return;
        }
        Wire.Add(MoveTemp(Packet));
    };
    Server.InitializeDelegates(FPacketHandlerLowLevelSendTraits::CreateLambda([&](void* Data, int32 Bits, FOutPacketTraits&) { Capture(false, Data, Bits); }));
    Client.InitializeDelegates(FPacketHandlerLowLevelSendTraits::CreateLambda([&](void* Data, int32 Bits, FOutPacketTraits&) { Capture(true, Data, Bits); }));
    Server.InitializeComponents();
    Client.InitializeComponents();

    FEncryptionKeyResponse ServerResponse;
    FEncryptionKeyResponse ClientResponse;
    FMRFPSTransportModule::HandleServerEncryptionToken(FMRFPSTransportModule::GetHelloMarker(), FOnEncryptionKeyResponse::CreateLambda([&](const FEncryptionKeyResponse& R) { ServerResponse = R; }));
    FMRFPSTransportModule::HandleClientEncryptionAck(FOnEncryptionKeyResponse::CreateLambda([&](const FEncryptionKeyResponse& R) { ClientResponse = R; }));
    if (!TestTrue(TEXT("Native server data response"), ServerResponse.Response == EEncryptionResponse::Success)
        || !TestTrue(TEXT("Native client data response"), ClientResponse.Response == EEncryptionResponse::Success)) return false;
    ServerCipher->SetEncryptionData(ServerResponse.EncryptionData);
    ClientCipher->SetEncryptionData(ClientResponse.EncryptionData);
    ServerCipher->EnableEncryption();
    ClientCipher->EnableEncryption();

    // Exercise the actual pre-handshake server application packet. Its payload must
    // be held, then decrypted by the peer; a policy-only mock would not prove this.
    ANSICHAR Payload[] = "MRFPS_NONSECRET_HANDSHAKE_TEST";
    const int32 PayloadBytes = sizeof(Payload) - 1;
    FOutPacketTraits Traits;
    const ProcessedPacket Initial = Server.Outgoing(reinterpret_cast<uint8*>(Payload), PayloadBytes * 8, Traits);
    TestFalse(TEXT("Initial packet must not fail"), Initial.bError);
    TestEqual(TEXT("No application bytes sent in plaintext before handshake"), Initial.CountBits, 0);
    bool bPayloadDelivered = false;
    for (int32 Step = 0; Step < 800 && !bPayloadDelivered; ++Step)
    {
        Server.Tick(0.01f);
        Client.Tick(0.01f);
        TArray<FWirePacket> Batch = MoveTemp(Wire);
        Wire.Reset();
        for (FWirePacket& Packet : Batch)
        {
            const ProcessedPacket Result = Packet.bToServer ? Server.Incoming(Packet.Bytes.GetData(), Packet.Bytes.Num()) : Client.Incoming(Packet.Bytes.GetData(), Packet.Bytes.Num());
            if (!Packet.bToServer && !Result.bError && Result.CountBits == PayloadBytes * 8)
            {
                bPayloadDelivered |= FMemory::Memcmp(Result.Data, Payload, PayloadBytes) == 0;
            }
        }
        // OpenSSL's retransmission clock uses real elapsed time, not Tick's DeltaTime.
        if (!bPayloadDelivered) FPlatformProcess::SleepNoStats(0.01f);
    }
    TestTrue(TEXT("A native handshake record was deliberately lost"), bDroppedInitialClientHandshake);
    TestTrue(TEXT("Native DTLS handshake releases queued application packet"), bPayloadDelivered);
    TestTrue(TEXT("All captured post-enable records carry the native encryption flag"), bOnlyEncryptedRecords);
    TestTrue(TEXT("Every native DTLS datagram fits the production connection MTU"), bWithinProductionMtu);
    const uint8 Plaintext[] = { 0, 0x61 };
    FBitReader Injected(Plaintext, 16);
    ServerCipher->Incoming(Injected);
    TestTrue(TEXT("Actual configured component rejects injected plaintext after encryption"), Injected.IsError());
    const uint8 Truncated[] = { 1 };
    FBitReader Malformed(Truncated, 1);
    ServerCipher->Incoming(Malformed);
    TestTrue(TEXT("Truncated native encrypted record rejected"), Malformed.IsError());
    return bPayloadDelivered && bOnlyEncryptedRecords && bDroppedInitialClientHandshake && bWithinProductionMtu;
}
#endif
