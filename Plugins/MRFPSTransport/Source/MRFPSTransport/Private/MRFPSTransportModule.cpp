#include "MRFPSTransportModule.h"
#include "MRFPSTransportPolicy.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Timespan.h"

#if UE_WITH_DTLS
#include "DTLSHandlerComponent.h"
#include "DTLSCertStore.h"
#include "DTLSCertificate.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSTransport, Log, All);

namespace
{
    const FString ServerCertificateId = TEXT("MRFPS.Server.DTLS.v1");
    const FString ClientCertificateId = TEXT("MRFPS.Client.DTLS.v1");
    FString ServerFingerprint;
    TArray<uint8> ExpectedServerFingerprint;

#if UE_WITH_DTLS
    // Epic creates notBefore at the device's current wall time. Let that time
    // pass on both peers within the same five-second clock-skew budget used by
    // JWT validation. Native certificate validity checks remain unchanged.
    constexpr double CertificateActivationSeconds = 6.0;
    double ClientCertificateReadyAt = 0.0;
#endif

    bool IsCertificateMode(FString& SafeError)
    {
#if UE_WITH_DTLS
        const IConsoleVariable* Mode = IConsoleManager::Get().FindConsoleVariable(TEXT("DTLS.PreSharedKeys"));
        if (Mode && Mode->GetInt() == 0) return true;
        SafeError = TEXT("Certificate-based DTLS is required");
#else
        SafeError = TEXT("Native DTLS is unavailable on this build platform");
#endif
        return false;
    }

#if UE_WITH_DTLS
    bool EnsureCertificate(const FString& Id, TSharedPtr<FDTLSCertificate>& Certificate, FString& SafeError)
    {
        if (!IsInGameThread()) { SafeError = TEXT("Native DTLS certificates must initialize on the game thread"); return false; }
        if (!IsCertificateMode(SafeError)) return false;
        Certificate = FDTLSCertStore::Get().GetCert(Id);
        if (!Certificate.IsValid())
        {
            Certificate = FDTLSCertStore::Get().CreateCert(FTimespan::FromDays(7), Id);
            if (Id == ClientCertificateId) ClientCertificateReadyAt = FPlatformTime::Seconds() + CertificateActivationSeconds;
        }
        if (!Certificate.IsValid() || Certificate->GetPKey() == nullptr || Certificate->GetFingerprint().Num() != 32)
        {
            SafeError = TEXT("Native DTLS certificate initialization failed");
            return false;
        }
        return true;
    }

    // Cryptography, DTLS records, certificate parsing and fingerprint verification stay
    // in Epic's OpenSSL-backed FDTLSHandlerComponent. This wrapper only closes its
    // permissive plaintext path after the encryption negotiation has been accepted.
    class FMRFPSStrictDTLSHandler final : public FDTLSHandlerComponent
    {
    public:
        virtual void SetEncryptionData(const FEncryptionData& Data) override
        {
            FString Error;
            bDataAccepted = IsCertificateMode(Error) && !Data.Identifier.IsEmpty();
            if (Handler && Handler->Mode == UE::Handler::Mode::Client) bDataAccepted &= Data.Fingerprint.Num() == 32;
            if (bDataAccepted) FDTLSHandlerComponent::SetEncryptionData(Data);
        }
        virtual void EnableEncryption() override
        {
            bEncryptionRequested = true;
            if (bDataAccepted) FDTLSHandlerComponent::EnableEncryption();
        }
        virtual void DisableEncryption() override
        {
            // A failed/disabling context may close, but it never reopens plaintext.
            bEncryptionRequested = true;
            FDTLSHandlerComponent::DisableEncryption();
        }
        virtual void Incoming(FBitReader& Packet) override
        {
            if (bEncryptionRequested && !IsEncryptionEnabled()) { Packet.SetError(); return; }
            if (!MRFPSTransportPolicy::MayAcceptIncoming(bEncryptionRequested, Packet))
            {
                Packet.SetError();
                return;
            }
            const bool bHandshakeRecord = bEncryptionRequested && MRFPSTransportPolicy::IsNativeHandshakeRecord(Packet);
            const bool bMeasureApplication = bObservedEncryptedOutput && !bHandshakeRecord && Packet.GetNumBits()>0;
            FDTLSHandlerComponent::Incoming(Packet);
            if (bMeasureApplication) {
                ++ApplicationInputs;
                if (Packet.IsError()) ++ApplicationInputErrors;
                else if (Packet.GetNumBits()==0) ++ApplicationEmptyInputs;
            }
            if (bHandshakeRecord && !Packet.IsError() && IsEncryptionEnabled())
            {
                // Native Incoming only queues handshake bytes; normally Tick advances
                // the state. Complete that work now so an application record arriving
                // in this same dispatch batch can follow the final Finished record.
                FDTLSHandlerComponent::Tick(0.0f);
                if (!IsEncryptionEnabled()) Packet.SetError();
            }
        }
        virtual void Outgoing(FBitWriter& Packet, FOutPacketTraits& Traits) override
        {
            if (bEncryptionRequested && !IsEncryptionEnabled()) { Packet.SetError(); return; }
            FHeldPacket Original;
            const bool bMayNeedHolding = bEncryptionRequested && !bObservedEncryptedOutput;
            if (bMayNeedHolding)
            {
                Original.NumBits = Packet.GetNumBits();
                Original.Bytes.Append(Packet.GetData(), Packet.GetNumBytes());
                Original.Traits = Traits;
            }
            FDTLSHandlerComponent::Outgoing(Packet, Traits);
            if (bEncryptionRequested && !Packet.IsError())
            {
                if (Packet.GetNumBits() > 0 && (Packet.GetData()[0] & 1) != 0)
                {
                    bObservedEncryptedOutput = true;
                    ++ApplicationOutputs;
                }
                else
                {
                    // Keep the original UE NetPacket (including its original packet ID),
                    // not the native handler's temporary plaintext prefix. This removes
                    // any dependency on reliable retransmits to resume the Login flow.
                    if (!bMayNeedHolding || HeldPackets.Num() >= 64 || HeldBytes + Original.Bytes.Num() > 131072)
                    {
                        Packet.SetError();
                        return;
                    }
                    HeldBytes += Original.Bytes.Num();
                    HeldPackets.Add(MoveTemp(Original));
                    Packet.Reset();
                }
            }
        }
        virtual void Tick(float DeltaTime) override
        {
            FDTLSHandlerComponent::Tick(DeltaTime);
            const double Now=FPlatformTime::Seconds();
            if (bObservedEncryptedOutput && Now-LastDiagnosticsAt>=2) {
                LastDiagnosticsAt=Now;
                UE_LOG(LogMRFPSTransport,Display,TEXT("MRFPS_DTLS_PATH mode=%s input=%llu empty=%llu errors=%llu output=%llu"),
                    Handler && Handler->Mode==UE::Handler::Mode::Client?TEXT("client"):TEXT("server"),
                    ApplicationInputs,ApplicationEmptyInputs,ApplicationInputErrors,ApplicationOutputs);
            }
            while (HeldPackets.Num() > 0)
            {
                if (!IsEncryptionEnabled()) { HeldPackets.Reset(); HeldBytes = 0; return; }
                FHeldPacket& Held = HeldPackets[0];
                FBitWriter Packet(Held.NumBits + GetReservedPacketBits(), true);
                Packet.SerializeBits(Held.Bytes.GetData(), Held.NumBits);
                FOutPacketTraits Traits = Held.Traits;
                FDTLSHandlerComponent::Outgoing(Packet, Traits);
                if (Packet.IsError()) { HeldPackets.Reset(); HeldBytes = 0; return; }
                if (Packet.GetNumBits() == 0 || (Packet.GetData()[0] & 1) == 0) return;
                bObservedEncryptedOutput = true;
                ++ApplicationOutputs;
                // The native record is already encrypted; start after this component,
                // just as Epic's DTLS handshake packet path does.
                Handler->SendHandlerPacket(this, Packet, Traits);
                HeldBytes -= Held.Bytes.Num();
                HeldPackets.RemoveAt(0);
            }
        }
    private:
        struct FHeldPacket
        {
            TArray<uint8> Bytes;
            int64 NumBits = 0;
            FOutPacketTraits Traits;
        };
        TArray<FHeldPacket> HeldPackets;
        int64 HeldBytes = 0;
        bool bEncryptionRequested = false;
        bool bDataAccepted = false;
        bool bObservedEncryptedOutput = false;
        uint64 ApplicationInputs=0,ApplicationEmptyInputs=0,ApplicationInputErrors=0,ApplicationOutputs=0;
        double LastDiagnosticsAt=-10;
    };
#endif
}

void FMRFPSTransportModule::StartupModule()
{
    FPacketHandlerComponentModuleInterface::StartupModule();
#if UE_WITH_DTLS
    FModuleManager::LoadModuleChecked<FDTLSHandlerComponentModule>(TEXT("DTLSHandlerComponent"));
    if (IConsoleVariable* Mode = IConsoleManager::Get().FindConsoleVariable(TEXT("DTLS.PreSharedKeys"))) Mode->Set(0, ECVF_SetByCode);
#endif
}

void FMRFPSTransportModule::ShutdownModule()
{
    ExpectedServerFingerprint.Reset();
    ServerFingerprint.Reset();
#if UE_WITH_DTLS
    ClientCertificateReadyAt = 0.0;
    if (FModuleManager::Get().IsModuleLoaded(TEXT("DTLSHandlerComponent")))
    {
        FDTLSCertStore::Get().RemoveCert(ServerCertificateId);
        FDTLSCertStore::Get().RemoveCert(ClientCertificateId);
    }
#endif
    FPacketHandlerComponentModuleInterface::ShutdownModule();
}

TSharedPtr<HandlerComponent> FMRFPSTransportModule::CreateComponentInstance(FString& Options)
{
#if UE_WITH_DTLS
    FString Error;
    if (IsCertificateMode(Error)) return MakeShared<FMRFPSStrictDTLSHandler>();
#endif
    UE_LOG(LogMRFPSTransport, Error, TEXT("Pinned DTLS packet handler unavailable; connection cannot proceed"));
    return nullptr;
}

bool FMRFPSTransportModule::EnsureServerCertificate(FString& FingerprintHex, FString& SafeError)
{
    FingerprintHex.Reset();
    SafeError.Reset();
#if UE_WITH_DTLS
    TSharedPtr<FDTLSCertificate> Certificate;
    if (!EnsureCertificate(ServerCertificateId, Certificate, SafeError)) return false;
    if (ServerFingerprint.IsEmpty())
    {
        const TArrayView<const uint8> Bytes = Certificate->GetFingerprint();
        ServerFingerprint = BytesToHex(Bytes.GetData(), Bytes.Num()).ToLower();
        UE_LOG(LogMRFPSTransport, Display, TEXT("MRFPS_DTLS_SERVER_SHA256=%s"), *ServerFingerprint);
    }
    FingerprintHex = ServerFingerprint;
    return true;
#else
    SafeError = TEXT("Native DTLS is unavailable on this build platform");
    return false;
#endif
}

FString FMRFPSTransportModule::GetServerFingerprint() { return ServerFingerprint; }

bool FMRFPSTransportModule::SetClientServerFingerprint(const FString& Hex, FString& SafeError)
{
    ExpectedServerFingerprint.Reset();
    SafeError.Reset();
    if (!IsCertificateMode(SafeError)) return false;
    if (!MRFPSTransportPolicy::ParseFingerprint(Hex, ExpectedServerFingerprint))
    {
        SafeError = TEXT("A verified server DTLS SHA-256 fingerprint is required");
        return false;
    }
#if UE_WITH_DTLS
    TSharedPtr<FDTLSCertificate> Certificate;
    if (!EnsureCertificate(ClientCertificateId, Certificate, SafeError))
    {
        ExpectedServerFingerprint.Reset();
        return false;
    }
#endif
    return true;
}

void FMRFPSTransportModule::ClearClientServerFingerprint()
{
    ExpectedServerFingerprint.Reset();
}

double FMRFPSTransportModule::GetClientCertificateActivationDelay()
{
#if UE_WITH_DTLS
    return FMath::Max(0.0, ClientCertificateReadyAt - FPlatformTime::Seconds());
#else
    return 0.0;
#endif
}

void FMRFPSTransportModule::HandleServerEncryptionToken(const FString& Marker, const FOnEncryptionKeyResponse& Delegate)
{
    FEncryptionKeyResponse Response(EEncryptionResponse::Failure, TEXT("Pinned DTLS negotiation failed"));
    FString Fingerprint;
    FString Error;
    if (Marker == GetHelloMarker() && EnsureServerCertificate(Fingerprint, Error))
    {
        Response.Response = EEncryptionResponse::Success;
        Response.ErrorMsg.Reset();
        Response.EncryptionData.Identifier = ServerCertificateId;
    }
    Delegate.ExecuteIfBound(Response);
}

void FMRFPSTransportModule::HandleClientEncryptionAck(const FOnEncryptionKeyResponse& Delegate)
{
    FEncryptionKeyResponse Response(EEncryptionResponse::Failure, TEXT("A verified server DTLS fingerprint is required"));
#if UE_WITH_DTLS
    FString Error;
    TSharedPtr<FDTLSCertificate> Certificate;
    if (ExpectedServerFingerprint.Num() == 32 && EnsureCertificate(ClientCertificateId, Certificate, Error) && GetClientCertificateActivationDelay() == 0.0)
    {
        Response.Response = EEncryptionResponse::Success;
        Response.ErrorMsg.Reset();
        Response.EncryptionData.Identifier = ClientCertificateId;
        Response.EncryptionData.Fingerprint = ExpectedServerFingerprint;
    }
#endif
    Delegate.ExecuteIfBound(Response);
}

IMPLEMENT_MODULE(FMRFPSTransportModule, MRFPSTransport)
