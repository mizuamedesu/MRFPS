#include "MRFPSAuthVerifier.h"

#include "MRFPSAuthCrypto.h"
#include "MRFPSAuthLogPrivacy.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformMisc.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeLock.h"

namespace
{
std::string Utf8(const FString& Value)
{
    const FTCHARToUTF8 Converted(*Value, Value.Len());
    return std::string(Converted.Get(), Converted.Length());
}

FString Unreal(const std::string& Value)
{
    const FUTF8ToTCHAR Converted(Value.data(), static_cast<int32>(Value.size()));
    return FString(Converted.Length(), Converted.Get());
}

void CopyClaims(const MRFPSAuthCore::Claims& Source, FMRFPSAuthClaims& Out)
{
    Out.Subject = Unreal(Source.Subject);
    Out.Role = Unreal(Source.Role);
    Out.Issuer = Unreal(Source.Issuer);
    Out.Audience = Unreal(Source.Audience);
    Out.TokenId = Unreal(Source.TokenId);
    Out.ServerFingerprint = Unreal(Source.ServerFingerprint);
    Out.IssuedAt = Source.IssuedAt;
    Out.NotBefore = Source.NotBefore;
    Out.ExpiresAt = Source.ExpiresAt;
}
}

struct FMRFPSAuthVerifier::FImpl
{
    explicit FImpl(int32 InCapacity) : Capacity(FMath::Clamp(InCapacity, 1, 65536)) {}
    mutable FCriticalSection Mutex;
    MRFPSAuthCore::PublicKey Key;
    std::string Audience;
    std::string ExpectedFingerprint;
    TMap<FString, int64> UsedIds;
    int32 Capacity;
    int64 LastPrune = -1;
};

FMRFPSAuthVerifier::FMRFPSAuthVerifier(int32 MaxReplayEntries) : Impl(MakeUnique<FImpl>(MaxReplayEntries)) {}
FMRFPSAuthVerifier::~FMRFPSAuthVerifier() = default;

void FMRFPSAuthVerifier::Reset()
{
    FScopeLock Lock(&Impl->Mutex);
    Impl->Key.Reset();
    Impl->Audience.clear();
    Impl->ExpectedFingerprint.clear();
    Impl->UsedIds.Empty();
    Impl->LastPrune = -1;
}

bool FMRFPSAuthVerifier::InitializeFromEnvironment(FString& SafeError)
{
    FMRFPSAuthLogPrivacy::SuppressNetworkTokenLogs();
    Reset();
    SafeError = TEXT("AUTH_CONFIG");
    const FString Encoded = FPlatformMisc::GetEnvironmentVariable(TEXT("MRFPS_AUTH_PUBLIC_KEY_B64"));
    FString Pem = FPlatformMisc::GetEnvironmentVariable(TEXT("MRFPS_AUTH_PUBLIC_KEY_PEM"));
    if (Encoded.IsEmpty() == Pem.IsEmpty()) return false;
    if (!Encoded.IsEmpty())
    {
        std::string Decoded;
        if (!MRFPSAuthCore::DecodePublicKeyBase64(Utf8(Encoded), Decoded)) return false;
        Pem = Unreal(Decoded);
    }
    FString Audience = FPlatformMisc::GetEnvironmentVariable(TEXT("MRFPS_AUTH_AUDIENCE"));
    if (Audience.IsEmpty()) Audience = TEXT("mrfps");
    return InitializeFromPublicKeyPem(Pem, Audience, SafeError);
}

bool FMRFPSAuthVerifier::InitializeFromPublicKeyPem(const FString& Pem, const FString& Audience, FString& SafeError)
{
    FScopeLock Lock(&Impl->Mutex);
    SafeError = TEXT("AUTH_CONFIG");
    Impl->Key.Reset();
    Impl->Audience.clear();
    Impl->ExpectedFingerprint.clear();
    Impl->UsedIds.Empty();
    Impl->LastPrune = -1;
    const std::string NewAudience = Utf8(Audience);
    if (Pem.Len() > 1024 || !MRFPSAuthCore::ValidAudience(NewAudience) || !Impl->Key.LoadPem(Utf8(Pem))) return false;
    Impl->Audience = NewAudience;
    SafeError.Empty();
    return true;
}

bool FMRFPSAuthVerifier::SetExpectedServerFingerprint(const FString& Fingerprint, FString& SafeError)
{
    FScopeLock Lock(&Impl->Mutex);
    Impl->ExpectedFingerprint.clear();
    SafeError = TEXT("AUTH_CONFIG");
    const std::string Value = Utf8(Fingerprint);
    if (!MRFPSAuthCore::ValidFingerprint(Value)) return false;
    Impl->ExpectedFingerprint = Value;
    SafeError.Empty();
    return true;
}

bool FMRFPSAuthVerifier::IsReady() const
{
    FScopeLock Lock(&Impl->Mutex);
    return Impl->Key.IsReady();
}

bool FMRFPSAuthVerifier::VerifyOnly(const FString& Token, const FString& ExpectedRole, FMRFPSAuthClaims& Claims, FString& SafeError) const
{
    Claims = FMRFPSAuthClaims();
    FScopeLock Lock(&Impl->Mutex);
    MRFPSAuthCore::Claims Parsed;
    std::string Error;
    const bool Result = Token.Len() <= static_cast<int32>(MRFPSAuthCore::MaxTokenBytes) &&
        MRFPSAuthCore::Verify(Utf8(Token), Impl->Key, Impl->Audience, Utf8(ExpectedRole),
            FDateTime::UtcNow().ToUnixTimestamp(), Parsed, Error, Impl->ExpectedFingerprint);
    SafeError = Error.empty() && !Result ? TEXT("AUTH_INVALID") : Unreal(Error);
    if (Result) CopyClaims(Parsed, Claims);
    return Result;
}

bool FMRFPSAuthVerifier::VerifyAndReserve(const FString& Token, const FString& ExpectedRole, FMRFPSAuthClaims& Claims, FString& SafeError)
{
    Claims = FMRFPSAuthClaims();
    FScopeLock Lock(&Impl->Mutex);
    const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
    if (Impl->ExpectedFingerprint.empty()) { SafeError = TEXT("AUTH_CONFIG"); return false; }
    MRFPSAuthCore::Claims Parsed;
    std::string Error;
    if (Token.Len() > static_cast<int32>(MRFPSAuthCore::MaxTokenBytes) ||
        !MRFPSAuthCore::Verify(Utf8(Token), Impl->Key, Impl->Audience, Utf8(ExpectedRole), Now, Parsed, Error, Impl->ExpectedFingerprint))
    {
        SafeError = Error.empty() ? TEXT("AUTH_INVALID") : Unreal(Error);
        return false;
    }
    if (Now != Impl->LastPrune)
    {
        for (auto It = Impl->UsedIds.CreateIterator(); It; ++It)
            if (It.Value() < Now) It.RemoveCurrent();
        Impl->LastPrune = Now;
    }
    const FString Id = Unreal(Parsed.TokenId);
    if (Impl->UsedIds.Contains(Id)) { SafeError = TEXT("AUTH_REPLAY"); return false; }
    if (Impl->UsedIds.Num() >= Impl->Capacity) { SafeError = TEXT("AUTH_CAPACITY"); return false; }
    // Retain through the entire acceptance leeway. Never discard a still-valid
    // reservation to make space; that would permit replay under memory pressure.
    Impl->UsedIds.Add(Id, Parsed.ExpiresAt + MRFPSAuthCore::ClockSkewSeconds);
    CopyClaims(Parsed, Claims);
    SafeError.Empty();
    return true;
}

bool FMRFPSAuthVerifier::IsWellFormedToken(const FString& Token)
{
    return Token.Len() <= static_cast<int32>(MRFPSAuthCore::MaxTokenBytes) && MRFPSAuthCore::WellFormed(Utf8(Token));
}
