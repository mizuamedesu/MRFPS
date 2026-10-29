#include "MRFPSAuthClientSubsystem.h"

#include "MRFPSAuthCrypto.h"
#include "MRFPSAuthLogPrivacy.h"
#include "MRFPSAuthClientUsePolicy.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#if PLATFORM_ANDROID && USE_ANDROID_JNI
#include "Android/AndroidJavaEnv.h"
#endif

namespace
{
#if PLATFORM_ANDROID && USE_ANDROID_JNI
FString AndroidInternalFilesDirectory()
{
    JNIEnv* Env=AndroidJavaEnv::GetJavaEnv(); jobject Activity=AndroidJavaEnv::GetGameActivityThis();
    if (!Env || !Activity || Env->PushLocalFrame(8)!=JNI_OK) { if (Env && Env->ExceptionCheck()) Env->ExceptionClear(); return FString(); }
    struct FLocalFrame { JNIEnv* Env; ~FLocalFrame() { Env->PopLocalFrame(nullptr); } } Frame{Env};
    const auto Failed=[Env](bool Invalid) { if (Env->ExceptionCheck()) { Env->ExceptionClear(); return true; } return Invalid; };
    jclass ActivityClass=Env->GetObjectClass(Activity); if (Failed(!ActivityClass)) return FString();
    jmethodID GetFilesDir=Env->GetMethodID(ActivityClass,"getFilesDir","()Ljava/io/File;"); if (Failed(!GetFilesDir)) return FString();
    jobject Directory=Env->CallObjectMethod(Activity,GetFilesDir); if (Failed(!Directory)) return FString();
    jclass FileClass=Env->GetObjectClass(Directory); if (Failed(!FileClass)) return FString();
    jmethodID GetAbsolutePath=Env->GetMethodID(FileClass,"getAbsolutePath","()Ljava/lang/String;"); if (Failed(!GetAbsolutePath)) return FString();
    jstring Path=static_cast<jstring>(Env->CallObjectMethod(Directory,GetAbsolutePath)); if (Failed(!Path)) return FString();
    FString Result=FJavaHelper::FStringFromLocalRef(Env,Path); return Failed(Result.IsEmpty())?FString():Result;
}
#endif
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

FString CanonicalPem(const FString& Pem)
{
    FString Result;
    for (TCHAR C : Pem) if (!FChar::IsWhitespace(C)) Result.AppendChar(C);
    return Result;
}
}

UMRFPSAuthClientSubsystem::UMRFPSAuthClientSubsystem() : ConnectionUses(MakeUnique<FMRFPSAuthClientUseState>()) {}
UMRFPSAuthClientSubsystem::~UMRFPSAuthClientSubsystem() = default;

FString UMRFPSAuthClientSubsystem::PrivateDirectory()
{
#if PLATFORM_ANDROID
#if USE_ANDROID_JNI
    const FString Root=AndroidInternalFilesDirectory();
    return Root.IsEmpty() || FPaths::IsRelative(Root) ? FString() : FPaths::Combine(Root,TEXT("MRFPS"));
#else
    return FString();
#endif
#elif PLATFORM_WINDOWS
    const FString Root=FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
    return Root.IsEmpty() || FPaths::IsRelative(Root) ? FString() : FPaths::Combine(Root,TEXT("MRFPS/Auth"));
#else
    return FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Auth"));
#endif
}

void UMRFPSAuthClientSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    FMRFPSAuthLogPrivacy::SuppressNetworkTokenLogs();
    Verifier = MakeUnique<FMRFPSAuthVerifier>();
    Audience = TEXT("mrfps");
    FParse::Value(FCommandLine::Get(), TEXT("MRFPSAuthAudience="), Audience);
    const FString Directory = PrivateDirectory();
    PublicKeyPath = Directory.IsEmpty()?FString():FPaths::Combine(Directory, TEXT("issuer-public.pem"));
    TokenPath = Directory.IsEmpty()?FString():FPaths::Combine(Directory, TEXT("access.jwt"));
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
    ConnectionUses.Reset();
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

bool UMRFPSAuthClientSubsystem::AcceptDeviceExchange(const FString& Token,const FString& PublicKeyPem,const FString& ExpectedRole,FString& SafeError)
{
    SafeError=TEXT("AUTH_INVALID");
    if (PublicKeyPath.IsEmpty()) { SafeError=TEXT("AUTH_KEY_FILE"); return false; }
    if (Token.Len()>2048 || PublicKeyPem.Len()>1024 ||
        (ExpectedRole!=TEXT("player") && ExpectedRole!=TEXT("inspector") && ExpectedRole!=TEXT("admin"))) return false;
    // Verify the role/profile/signature before changing either the pinned key
    // or current credential. The TLS endpoint is the first-use trust source.
    auto Candidate=MakeUnique<FMRFPSAuthVerifier>();
    FMRFPSAuthClaims Claims;
    if (!Candidate->InitializeFromPublicKeyPem(PublicKeyPem,Audience,SafeError) ||
        !Candidate->VerifyOnly(Token,ExpectedRole,Claims,SafeError) ||
        Claims.ExpiresAt<=FDateTime::UtcNow().ToUnixTimestamp()) return false;
    FString Existing;
    if (IFileManager::Get().FileExists(*PublicKeyPath)) {
        if (!ReadAsciiFile(PublicKeyPath,1024,Existing)) { SafeError=TEXT("AUTH_KEY_FILE"); return false; }
        if (CanonicalPem(Existing)!=CanonicalPem(PublicKeyPem)) { SafeError=TEXT("AUTH_KEY_CHANGED"); return false; }
    } else {
        if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(PublicKeyPath),true)) { SafeError=TEXT("AUTH_KEY_FILE"); return false; }
        // Several spectator processes can first log in concurrently. Never
        // overwrite a key that another process has pinned in the meantime.
        if (!FFileHelper::SaveStringToFile(PublicKeyPem,*PublicKeyPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
            &IFileManager::Get(),FILEWRITE_NoReplaceExisting)) {
            if (!ReadAsciiFile(PublicKeyPath,1024,Existing)) { SafeError=TEXT("AUTH_KEY_FILE"); return false; }
            if (CanonicalPem(Existing)!=CanonicalPem(PublicKeyPem)) { SafeError=TEXT("AUTH_KEY_CHANGED"); return false; }
        }
    }
    Verifier=MoveTemp(Candidate);
    return SetAccessToken(Token,SafeError);
}

bool UMRFPSAuthClientSubsystem::ClearIssuerTrust(FString& SafeError)
{
    SafeError=TEXT("AUTH_KEY_FILE");
    if (IFileManager::Get().FileExists(*PublicKeyPath) && !IFileManager::Get().Delete(*PublicKeyPath,false,true)) return false;
    ClearAccessToken();
    if (Verifier) Verifier->Reset();
    SafeError.Empty();
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

bool UMRFPSAuthClientSubsystem::ValidateForNewConnection(const FString& ExpectedRole,FString& SafeError)
{
    if (!ValidateForRole(ExpectedRole,SafeError)) return false;
    if (!ConnectionUses) ConnectionUses=MakeUnique<FMRFPSAuthClientUseState>();
    const auto Result=ConnectionUses->Uses.Check(TCHAR_TO_UTF8(*CachedClaims.TokenId),CachedClaims.ExpiresAt,FDateTime::UtcNow().ToUnixTimestamp());
    if (Result==MRFPSAuthClientPolicy::EUse::Fresh) return true;
    SafeError=LastSafeError=Result==MRFPSAuthClientPolicy::EUse::AlreadyAttempted ? TEXT("AUTH_FRESH_TOKEN_REQUIRED") :
        Result==MRFPSAuthClientPolicy::EUse::Capacity ? TEXT("AUTH_CAPACITY") : TEXT("AUTH_INVALID");
    return false;
}

bool UMRFPSAuthClientSubsystem::BeginConnectionAttempt(const FString& ExpectedRole,FString& SafeError)
{
    if (!ValidateForNewConnection(ExpectedRole,SafeError)) return false;
    const auto Result=ConnectionUses->Uses.Begin(TCHAR_TO_UTF8(*CachedClaims.TokenId),CachedClaims.ExpiresAt,FDateTime::UtcNow().ToUnixTimestamp());
    if (Result==MRFPSAuthClientPolicy::EUse::Fresh) return true;
    SafeError=LastSafeError=TEXT("AUTH_FRESH_TOKEN_REQUIRED");
    return false;
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
    if (HasAccessToken()) return ConnectionUses && ConnectionUses->Uses.WasAttempted(TCHAR_TO_UTF8(*CachedClaims.TokenId)) ?
        TEXT("再接続には新しい認証情報が必要です") : TEXT("認証情報を設定済み");
    if (!AccessToken.IsEmpty()) return TEXT("AUTH_EXPIRED");
    return LastSafeError.IsEmpty() ? TEXT("AUTH_MISSING") : LastSafeError;
}
