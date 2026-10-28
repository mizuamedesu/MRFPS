#include "MRFPSAuthClientSubsystem.h"

#include "MRFPSAuthCrypto.h"
#include "MRFPSAuthLogPrivacy.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"

namespace
{
bool ReadAsciiFile(const FString& Path, int64 Limit, FString& Out)
{
    Out.Empty();
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
    if (!File) return false;
    const int64 Size = File->TotalSize();
    if (Size <= 0 || Size > Limit) return false;
    TArray<uint8> Bytes;
    Bytes.SetNumUninitialized(static_cast<int32>(Size));
    File->Serialize(Bytes.GetData(), Size);
    if (File->IsError()) return false;
    for (uint8 Byte : Bytes) if (Byte == 0 || Byte > 127) return false;
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    Out = FString(Converted.Length(), Converted.Get());
    return true;
}
}

UMRFPSAuthClientSubsystem::UMRFPSAuthClientSubsystem() = default;
UMRFPSAuthClientSubsystem::~UMRFPSAuthClientSubsystem() = default;

void UMRFPSAuthClientSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    FMRFPSAuthLogPrivacy::SuppressNetworkTokenLogs();
    Verifier = MakeUnique<FMRFPSAuthVerifier>();
    Audience = TEXT("mrfps");
    FParse::Value(FCommandLine::Get(), TEXT("MRFPSAuthAudience="), Audience);
#if PLATFORM_ANDROID
    const FString Directory = FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), TEXT("MRFPS"));
#else
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Auth"));
#endif
    PublicKeyPath = FPaths::Combine(Directory, TEXT("issuer-public.pem"));
    TokenPath = FPaths::Combine(Directory, TEXT("access.jwt"));
    FParse::Value(FCommandLine::Get(), TEXT("MRFPSAuthPublicKeyFile="), PublicKeyPath);
    FParse::Value(FCommandLine::Get(), TEXT("MRFPSAuthTokenFile="), TokenPath);
    FString Error;
    if (LoadPublicKey(Error) && IFileManager::Get().FileExists(*TokenPath)) ReloadTokenFile(Error);
    LastSafeError = Error;
}

void UMRFPSAuthClientSubsystem::Deinitialize()
{
    ClearAccessToken();
    Verifier.Reset();
    Super::Deinitialize();
}

bool UMRFPSAuthClientSubsystem::LoadPublicKey(FString& SafeError)
{
    if (!Verifier) Verifier = MakeUnique<FMRFPSAuthVerifier>();
    FString Pem;
    SafeError = TEXT("AUTH_KEY_FILE");
    if (!ReadAsciiFile(PublicKeyPath, 1024, Pem)) return false;
    return Verifier->InitializeFromPublicKeyPem(Pem, Audience, SafeError);
}

void UMRFPSAuthClientSubsystem::ClearAccessToken()
{
    if (!AccessToken.IsEmpty())
        FMemory::Memzero(AccessToken.GetCharArray().GetData(), AccessToken.GetCharArray().Num() * sizeof(TCHAR));
    AccessToken.Empty();
    CachedClaims = FMRFPSAuthClaims();
    LastSafeError = TEXT("AUTH_MISSING");
}

bool UMRFPSAuthClientSubsystem::SetAccessToken(const FString& Token, FString& SafeError)
{
    ClearAccessToken();
    const FString Clean = Token.TrimStartAndEnd();
    SafeError = Clean.IsEmpty() ? TEXT("AUTH_MISSING") : TEXT("AUTH_INVALID");
    if (!FMRFPSAuthVerifier::IsWellFormedToken(Clean)) { LastSafeError = SafeError; return false; }
    if ((!Verifier || !Verifier->IsReady()) && !LoadPublicKey(SafeError)) { LastSafeError = SafeError; return false; }
    FMRFPSAuthClaims Parsed;
    bool Valid = Verifier->VerifyOnly(Clean, TEXT("player"), Parsed, SafeError);
    if (!Valid && SafeError == TEXT("AUTH_ROLE")) Valid = Verifier->VerifyOnly(Clean, TEXT("inspector"), Parsed, SafeError);
    if (!Valid && SafeError == TEXT("AUTH_ROLE")) Valid = Verifier->VerifyOnly(Clean, TEXT("admin"), Parsed, SafeError);
    if (!Valid) { LastSafeError = SafeError; return false; }
    if (Parsed.ExpiresAt <= FDateTime::UtcNow().ToUnixTimestamp())
    {
        SafeError = LastSafeError = TEXT("AUTH_EXPIRED");
        return false;
    }
    AccessToken = Clean;
    CachedClaims = MoveTemp(Parsed);
    SafeError.Empty();
    LastSafeError.Empty();
    return true;
}

bool UMRFPSAuthClientSubsystem::HasAccessToken() const
{
    if (AccessToken.IsEmpty() || !Verifier || !Verifier->IsReady()) return false;
    const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
    return Now < CachedClaims.ExpiresAt && Now + MRFPSAuthCore::ClockSkewSeconds >= CachedClaims.NotBefore &&
        Now + MRFPSAuthCore::ClockSkewSeconds >= CachedClaims.IssuedAt;
}

bool UMRFPSAuthClientSubsystem::ValidateForRole(const FString& ExpectedRole, FString& SafeError)
{
    FMRFPSAuthLogPrivacy::SuppressNetworkTokenLogs();
    if (AccessToken.IsEmpty())
    {
        // File loading and SetAccessToken already distinguished a bad signature,
        // expiry, or audience. Preserve that safe diagnostic for the join UI.
        SafeError = LastSafeError.IsEmpty() ? TEXT("AUTH_MISSING") : LastSafeError;
        return false;
    }
    if (!Verifier || !Verifier->IsReady()) { SafeError = LastSafeError = TEXT("AUTH_CONFIG"); return false; }
    FMRFPSAuthClaims Parsed;
    if (!Verifier->VerifyOnly(AccessToken, ExpectedRole, Parsed, SafeError)) { LastSafeError = SafeError; return false; }
    if (Parsed.ExpiresAt <= FDateTime::UtcNow().ToUnixTimestamp()) { SafeError = LastSafeError = TEXT("AUTH_EXPIRED"); return false; }
    CachedClaims = MoveTemp(Parsed);
    LastSafeError.Empty();
    return true;
}

bool UMRFPSAuthClientSubsystem::ReloadTokenFile(FString& SafeError)
{
    FString Token;
    const int64 Size = IFileManager::Get().FileSize(*TokenPath);
    SafeError = Size <= 0 ? TEXT("AUTH_MISSING") : TEXT("AUTH_INVALID");
    if (Size <= 0 || !ReadAsciiFile(TokenPath, 4096, Token)) { ClearAccessToken(); LastSafeError = SafeError; return false; }
    return SetAccessToken(Token, SafeError);
}

FString UMRFPSAuthClientSubsystem::GetServerFingerprint() const
{
    return HasAccessToken() ? CachedClaims.ServerFingerprint : FString();
}

FString UMRFPSAuthClientSubsystem::GetCredentialStatus() const
{
    if (HasAccessToken()) return TEXT("Signed access token ready");
    if (!AccessToken.IsEmpty()) return TEXT("AUTH_EXPIRED");
    return LastSafeError.IsEmpty() ? TEXT("AUTH_MISSING") : LastSafeError;
}
