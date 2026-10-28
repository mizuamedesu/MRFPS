#pragma once

#include "CoreMinimal.h"

/** Verified admission facts. Never contains the access token or its signature. */
struct MRFPSAUTH_API FMRFPSAuthClaims
{
    FString Subject;
    FString Role;
    FString Issuer;
    FString Audience;
    FString TokenId;
    FString ServerFingerprint;
    int64 IssuedAt = 0;
    int64 NotBefore = 0;
    int64 ExpiresAt = 0;
};

/** One server's public verification key and bounded, atomic single-use jti store. */
class MRFPSAUTH_API FMRFPSAuthVerifier
{
public:
    explicit FMRFPSAuthVerifier(int32 MaxReplayEntries = 65536);
    ~FMRFPSAuthVerifier();
    FMRFPSAuthVerifier(const FMRFPSAuthVerifier&) = delete;
    FMRFPSAuthVerifier& operator=(const FMRFPSAuthVerifier&) = delete;

    bool InitializeFromEnvironment(FString& SafeError);
    /** Explicit configuration API; accepts an Ed25519 SPKI PUBLIC KEY only. */
    bool InitializeFromPublicKeyPem(const FString& Pem, const FString& Audience, FString& SafeError);
    /** Bind admission to this server's native DTLS certificate fingerprint. */
    bool SetExpectedServerFingerprint(const FString& Fingerprint, FString& SafeError);
    bool IsReady() const;
    /** Signature/profile/time verification. Does not consume or check replay state. */
    bool VerifyOnly(const FString& Token, const FString& ExpectedRole, FMRFPSAuthClaims& Claims, FString& SafeError) const;
    /** Re-verifies and atomically consumes jti; rejects full stores without evicting live entries. */
    bool VerifyAndReserve(const FString& Token, const FString& ExpectedRole, FMRFPSAuthClaims& Claims, FString& SafeError);
    void Reset();

    /** Structural/profile bounds only, suitable for protecting the client login option. */
    static bool IsWellFormedToken(const FString& Token);

private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
