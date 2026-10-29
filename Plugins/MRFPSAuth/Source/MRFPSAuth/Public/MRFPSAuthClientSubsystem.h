#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "MRFPSAuthClientUsePolicy.h"
#include "MRFPSAuthVerifier.h"
#include "MRFPSAuthClientSubsystem.generated.h"

// Generated UObject constructors can instantiate TUniquePtr cleanup; keep the
// pointee complete in every translation unit that includes this header.
struct FMRFPSAuthClientUseState { MRFPSAuthClientPolicy::FConnectionUses Uses; };

/** Issuer public key plus transient signed credential. No reflected token data. */
UCLASS()
class MRFPSAUTH_API UMRFPSAuthClientSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    UMRFPSAuthClientSubsystem();
    virtual ~UMRFPSAuthClientSubsystem() override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    bool SetAccessToken(const FString& Token, FString& SafeError);
    bool HasAccessToken() const;
    bool ValidateForRole(const FString& ExpectedRole, FString& SafeError);
    /** A new connection needs a locally unattempted, still-valid signed jti. */
    bool ValidateForNewConnection(const FString& ExpectedRole, FString& SafeError);
    /** Conservatively retire the credential when ClientTravel is about to begin. */
    bool BeginConnectionAttempt(const FString& ExpectedRole, FString& SafeError);
    void ClearAccessToken();
    bool ReloadTokenFile(FString& SafeError);
    FString GetServerFingerprint() const;
    FString GetCredentialStatus() const;
    FString GetVerifiedRole() const { return HasAccessToken() ? CachedClaims.Role : FString(); }
    /** C++ login injector only. Never expose through reflection, replication, or logs. */
    const FString& GetAccessToken() const { return AccessToken; }

private:
    bool LoadPublicKey(FString& SafeError);
    TUniquePtr<FMRFPSAuthVerifier> Verifier;
    TUniquePtr<FMRFPSAuthClientUseState> ConnectionUses;
    FMRFPSAuthClaims CachedClaims;
    FString AccessToken;
    FString Audience;
    FString PublicKeyPath;
    FString TokenPath;
    FString LastSafeError;
};
