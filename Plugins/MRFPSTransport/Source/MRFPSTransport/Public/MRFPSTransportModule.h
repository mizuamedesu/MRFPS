#pragma once

#include "CoreMinimal.h"
#include "PacketHandler.h"
#include "Engine/NetworkDelegates.h"

class MRFPSTRANSPORT_API FMRFPSTransportModule : public FPacketHandlerComponentModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual TSharedPtr<HandlerComponent> CreateComponentInstance(FString& Options) override;

    // This is a public protocol marker, never an authentication credential.
    static const TCHAR* GetHelloMarker() { return TEXT("mrfps-dtls-v1"); }

    // Generates an independent in-memory server DTLS key/certificate. Only its public
    // SHA-256 fingerprint is returned; issuer JWT private keys are never read here.
    static bool EnsureServerCertificate(FString& FingerprintHex, FString& SafeError);
    static FString GetServerFingerprint();

    // Call only after verifying a locally provisioned JWT signature/time/audience.
    // An invalid replacement clears the previous pin, so callers cannot reuse stale trust.
    static bool SetClientServerFingerprint(const FString& FingerprintHex, FString& SafeError);
    static void ClearClientServerFingerprint();
    // After setting verified trust, wait this long before starting ClientTravel.
    // Revalidate the credential when the wait finishes. Never delay EncryptionAck.
    static double GetClientCertificateActivationDelay();

    // UGameInstance must forward both overrides and always complete the delegate.
    static void HandleServerEncryptionToken(const FString& Marker, const FOnEncryptionKeyResponse& Delegate);
    static void HandleClientEncryptionAck(const FOnEncryptionKeyResponse& Delegate);
};
