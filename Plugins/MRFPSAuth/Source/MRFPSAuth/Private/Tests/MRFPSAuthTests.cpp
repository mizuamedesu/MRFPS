#include "MRFPSAuthVerifier.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "MRFPSAuthClientSubsystem.h"
#include "MRFPSAuthLogPrivacy.h"
#include "MRFPSAuthLoginOptions.h"
#include "../../../../Tests/AuthTestFixture.h"

#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "EngineLogs.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Subsystems/SubsystemCollection.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
FString U(const std::string& Value)
{
    const FUTF8ToTCHAR Converted(Value.data(), static_cast<int32>(Value.size()));
    return FString(Converted.Length(), Converted.Get());
}

struct FScopedAuthEnvironment
{
    FString Encoded = FPlatformMisc::GetEnvironmentVariable(TEXT("MRFPS_AUTH_PUBLIC_KEY_B64"));
    FString Pem = FPlatformMisc::GetEnvironmentVariable(TEXT("MRFPS_AUTH_PUBLIC_KEY_PEM"));
    FString Audience = FPlatformMisc::GetEnvironmentVariable(TEXT("MRFPS_AUTH_AUDIENCE"));
    ~FScopedAuthEnvironment()
    {
        FPlatformMisc::SetEnvironmentVar(TEXT("MRFPS_AUTH_PUBLIC_KEY_B64"), *Encoded);
        FPlatformMisc::SetEnvironmentVar(TEXT("MRFPS_AUTH_PUBLIC_KEY_PEM"), *Pem);
        FPlatformMisc::SetEnvironmentVar(TEXT("MRFPS_AUTH_AUDIENCE"), *Audience);
    }
};

struct FScopedAuthCommandLine
{
    FString Previous = FCommandLine::Get();
    explicit FScopedAuthCommandLine(const FString& Prefix) { FCommandLine::Set(*(Prefix + TEXT(" ") + Previous)); }
    ~FScopedAuthCommandLine() { FCommandLine::Set(*Previous); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSAuthAdmissionTest, "MRFPS.Auth.AdmissionAndReplay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSAuthAdmissionTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSAuthTests;
    Signer Issuer;
    if (!TestTrue(TEXT("The test issuer has an ephemeral Ed25519 key"), Issuer.Ready())) return false;
    const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
    TokenSpec Spec(Now);
    const FString Token = U(Issuer.Sign(Spec));
    FString Error;
    FMRFPSAuthClaims Claims;
    FMRFPSAuthVerifier Verifier(2);
    TestTrue(TEXT("Explicit public-key configuration succeeds"), Verifier.InitializeFromPublicKeyPem(U(Issuer.PublicPem()), TEXT("mrfps"), Error));
    TestTrue(TEXT("Cryptographic verification succeeds before capacity reservation"), Verifier.VerifyOnly(Token, TEXT("player"), Claims, Error));
    TestEqual(TEXT("Authenticated identity is the signed subject"), Claims.Subject, U(Spec.Subject));
    TestEqual(TEXT("Authenticated DTLS pin is signed"), Claims.ServerFingerprint, U(Spec.Fingerprint));
    TestFalse(TEXT("Server admission cannot reserve before binding its own DTLS certificate"), Verifier.VerifyAndReserve(Token, TEXT("player"), Claims, Error));
    TestEqual(TEXT("Missing server binding is a configuration failure"), Error, FString(TEXT("AUTH_CONFIG")));
    TestTrue(TEXT("Server DTLS certificate binding accepted"), Verifier.SetExpectedServerFingerprint(U(Spec.Fingerprint), Error));
    TestTrue(TEXT("First admission consumes the token ID"), Verifier.VerifyAndReserve(Token, TEXT("player"), Claims, Error));
    TestFalse(TEXT("The same signed token cannot enter twice"), Verifier.VerifyAndReserve(Token, TEXT("player"), Claims, Error));
    TestEqual(TEXT("Replay has a stable safe code"), Error, FString(TEXT("AUTH_REPLAY")));
    TestTrue(TEXT("Read-only verification does not consume or mutate replay state"), Verifier.VerifyOnly(Token, TEXT("player"), Claims, Error));
    Spec.TokenId = "12345678-abcd-4123-8abc-123456789abd";
    TestTrue(TEXT("A new signed token can use the remaining reservation"), Verifier.VerifyAndReserve(U(Issuer.Sign(Spec)), TEXT("player"), Claims, Error));
    Spec.TokenId = "12345678-abcd-4123-8abc-123456789abe";
    TestFalse(TEXT("A full replay store refuses admission without evicting live IDs"), Verifier.VerifyAndReserve(U(Issuer.Sign(Spec)), TEXT("player"), Claims, Error));
    TestEqual(TEXT("Bounded store reports capacity safely"), Error, FString(TEXT("AUTH_CAPACITY")));
    TestFalse(TEXT("Capacity pressure cannot make the first token replayable"), Verifier.VerifyAndReserve(Token, TEXT("player"), Claims, Error));
    TestEqual(TEXT("The first reservation is retained"), Error, FString(TEXT("AUTH_REPLAY")));

    TestFalse(TEXT("A player token cannot request an inspector role"), Verifier.VerifyOnly(Token, TEXT("inspector"), Claims, Error));
    TestEqual(TEXT("Role mismatch is explicit"), Error, FString(TEXT("AUTH_ROLE")));
    TestFalse(TEXT("A signed player cannot request administrator admission"),Verifier.VerifyOnly(Token,TEXT("admin"),Claims,Error));
    TokenSpec AdminSpec=Spec; AdminSpec.Role="admin";
    const FString AdminToken=U(Issuer.Sign(AdminSpec));
    TestTrue(TEXT("Only a correctly signed exact admin role is accepted"),Verifier.VerifyOnly(AdminToken,TEXT("admin"),Claims,Error));
    TestEqual(TEXT("Admin claim survives cryptographic parsing"),Claims.Role,FString(TEXT("admin")));
    TestFalse(TEXT("Admin is not a fighter credential"),Verifier.VerifyOnly(AdminToken,TEXT("player"),Claims,Error));
    TestFalse(TEXT("Admin is not interchangeable with an inspector credential"),Verifier.VerifyOnly(AdminToken,TEXT("inspector"),Claims,Error));
    TokenSpec InspectorSpec=Spec; InspectorSpec.Role="inspector";
    TestFalse(TEXT("An ordinary inspector cannot elevate its role through login options"),Verifier.VerifyOnly(U(Issuer.Sign(InspectorSpec)),TEXT("admin"),Claims,Error));
    Signer OtherIssuer;
    TestFalse(TEXT("An untrusted issuer cannot grant admin"),Verifier.VerifyOnly(U(OtherIssuer.Sign(AdminSpec)),TEXT("admin"),Claims,Error));
    Spec.Fingerprint = std::string(64, 'b');
    TestFalse(TEXT("A correctly signed token for another server certificate is refused"), Verifier.VerifyOnly(U(Issuer.Sign(Spec)), TEXT("player"), Claims, Error));
    TestEqual(TEXT("Certificate mismatch has a stable safe code"), Error, FString(TEXT("AUTH_TRANSPORT")));
    FString HiddenTail = Token;
    HiddenTail.AppendChar(0);
    HiddenTail.Append(TEXT("untrusted-tail"));
    TestFalse(TEXT("An embedded-NUL token cannot lose its suffix through UTF conversion"), FMRFPSAuthVerifier::IsWellFormedToken(HiddenTail));
    Verifier.Reset();
    TestFalse(TEXT("Reset removes the verification key"), Verifier.IsReady());
    TestFalse(TEXT("Reset cannot create a development fallback"), Verifier.VerifyOnly(Token, TEXT("player"), Claims, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSAuthAtomicReservationTest, "MRFPS.Auth.AtomicReservation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSAuthAtomicReservationTest::RunTest(const FString& Parameters)
{
    MRFPSAuthTests::Signer Issuer;
    if (!TestTrue(TEXT("Test issuer initialized"), Issuer.Ready())) return false;
    const MRFPSAuthTests::TokenSpec Spec(FDateTime::UtcNow().ToUnixTimestamp());
    const FString Token = U(Issuer.Sign(Spec));
    FMRFPSAuthVerifier Verifier;
    FString Error;
    if (!Verifier.InitializeFromPublicKeyPem(U(Issuer.PublicPem()), TEXT("mrfps"), Error) ||
        !Verifier.SetExpectedServerFingerprint(U(Spec.Fingerprint), Error)) return false;
    TArray<TFuture<bool>> Attempts;
    for (int32 Index = 0; Index < 8; ++Index)
    {
        Attempts.Add(Async(EAsyncExecution::ThreadPool, [&Verifier, Token]()
        {
            FMRFPSAuthClaims Claims;
            FString SafeError;
            return Verifier.VerifyAndReserve(Token, TEXT("player"), Claims, SafeError);
        }));
    }
    int32 Accepted = 0;
    for (auto& Attempt : Attempts) if (Attempt.Get()) ++Accepted;
    TestEqual(TEXT("Exactly one simultaneous admission can consume a jti"), Accepted, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSAuthConfigurationTest, "MRFPS.Auth.EnvironmentFailsClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSAuthConfigurationTest::RunTest(const FString& Parameters)
{
    const FScopedAuthEnvironment Restore;
    FPlatformMisc::SetEnvironmentVar(TEXT("MRFPS_AUTH_PUBLIC_KEY_B64"), TEXT(""));
    FPlatformMisc::SetEnvironmentVar(TEXT("MRFPS_AUTH_PUBLIC_KEY_PEM"), TEXT(""));
    FPlatformMisc::SetEnvironmentVar(TEXT("MRFPS_AUTH_AUDIENCE"), TEXT("mrfps"));
    FMRFPSAuthVerifier Verifier;
    FString Error;
    TestFalse(TEXT("A server without a public key fails closed"), Verifier.InitializeFromEnvironment(Error));
    TestEqual(TEXT("Configuration errors contain a safe code"), Error, FString(TEXT("AUTH_CONFIG")));
    MRFPSAuthTests::Signer Issuer;
    if (!TestTrue(TEXT("Test issuer initialized"), Issuer.Ready())) return false;
    const FString Pem = U(Issuer.PublicPem());
    FPlatformMisc::SetEnvironmentVar(TEXT("MRFPS_AUTH_PUBLIC_KEY_PEM"), *Pem);
    TestTrue(TEXT("A single PEM environment key is accepted"), Verifier.InitializeFromEnvironment(Error));
    const FTCHARToUTF8 Bytes(*Pem);
    const FString Encoded = FBase64::Encode(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
    FPlatformMisc::SetEnvironmentVar(TEXT("MRFPS_AUTH_PUBLIC_KEY_B64"), *Encoded);
    TestFalse(TEXT("Two competing public-key variables are refused"), Verifier.InitializeFromEnvironment(Error));
    TestFalse(TEXT("Invalid reconfiguration clears the previous key"), Verifier.IsReady());
    FPlatformMisc::SetEnvironmentVar(TEXT("MRFPS_AUTH_PUBLIC_KEY_PEM"), TEXT(""));
    TestTrue(TEXT("Base64 PEM environment key is accepted"), Verifier.InitializeFromEnvironment(Error));
    FPlatformMisc::SetEnvironmentVar(TEXT("MRFPS_AUTH_PUBLIC_KEY_B64"), TEXT("private or malformed input"));
    TestFalse(TEXT("Malformed environment input cannot preserve a prior working key"), Verifier.InitializeFromEnvironment(Error));
    TestFalse(TEXT("A private PEM is not valid public-key configuration"), Verifier.InitializeFromPublicKeyPem(U(Issuer.PrivatePem()), TEXT("mrfps"), Error));
    TestFalse(TEXT("Unsafe audience configuration is refused"), Verifier.InitializeFromPublicKeyPem(Pem, TEXT("unsafe audience?"), Error));
    FMRFPSAuthLogPrivacy::SuppressNetworkTokenLogs();
    TestEqual(TEXT("Native login URLs cannot be printed by LogNet"), LogNet.GetVerbosity(), ELogVerbosity::NoLogging);
    TestEqual(TEXT("Network traffic logging is disabled for credentials"), LogNetTraffic.GetVerbosity(), ELogVerbosity::NoLogging);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSAuthClientCredentialTest, "MRFPS.Auth.ClientVerifiedCredentials",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSAuthClientCredentialTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSAuthTests;
    Signer Issuer;
    if (!TestTrue(TEXT("Test issuer initialized"), Issuer.Ready())) return false;
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/Auth"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString PublicPath = FPaths::Combine(Directory, TEXT("issuer-public.pem"));
    const FString TokenPath = FPaths::Combine(Directory, TEXT("access.jwt"));
    TokenSpec Spec(FDateTime::UtcNow().ToUnixTimestamp());
    const FString Token = U(Issuer.Sign(Spec));
    TestTrue(TEXT("Write the ephemeral test public key"), FFileHelper::SaveStringToFile(U(Issuer.PublicPem()), *PublicPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    TestTrue(TEXT("Write the ephemeral test token"), FFileHelper::SaveStringToFile(Token + TEXT("\n"), *TokenPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    const FScopedAuthCommandLine CommandLine(FString::Printf(TEXT("-MRFPSAuthPublicKeyFile=\"%s\" -MRFPSAuthTokenFile=\"%s\" -MRFPSAuthAudience=mrfps"), *PublicPath, *TokenPath));
    TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>());
    TStrongObjectPtr<UMRFPSAuthClientSubsystem> Credentials(NewObject<UMRFPSAuthClientSubsystem>(Instance.Get()));
    // Initialize this subsystem only; do not start hardware SDK subsystems.
    FSubsystemCollection<UGameInstanceSubsystem> Collection;
    Credentials->Initialize(Collection);
    TestTrue(TEXT("A file credential is retained only after public-key verification"), Credentials->HasAccessToken());
    TestTrue(TEXT("A real verified credential cannot be emitted without a current encrypted pending connection"),
        MRFPSAuthLogin::BuildForCurrentPendingConnection(Instance.Get(), Credentials.Get()).IsEmpty());
    TestTrue(TEXT("Refusing an early challenge preserves the local verified credential"), Credentials->HasAccessToken());
    TestEqual(TEXT("The client trusts the fingerprint from the verified token"), Credentials->GetServerFingerprint(), U(Spec.Fingerprint));
    FString Error;
    TestTrue(TEXT("The verified token authorizes its signed player role"), Credentials->ValidateForRole(TEXT("player"), Error));
    TestFalse(TEXT("The same client token cannot select inspector admission"), Credentials->ValidateForRole(TEXT("inspector"), Error));
    TestEqual(TEXT("Role failure is safe to display"), Error, FString(TEXT("AUTH_ROLE")));
    Signer Attacker;
    TestFalse(TEXT("A syntactically valid forged token is rejected by the client before trusting its pin"), Credentials->SetAccessToken(U(Attacker.Sign(Spec)), Error));
    TestFalse(TEXT("A rejected credential cannot leave an old token active"), Credentials->HasAccessToken());
    TestFalse(TEXT("A join remains rejected after a forged file credential"), Credentials->ValidateForRole(TEXT("player"), Error));
    TestEqual(TEXT("Joining preserves the actual invalid-signature diagnostic"), Error, FString(TEXT("AUTH_INVALID")));
    TokenSpec Expired = Spec;
    Expired.IssuedAt = Expired.NotBefore = FDateTime::UtcNow().ToUnixTimestamp() - 60;
    Expired.ExpiresAt = Expired.IssuedAt + 20;
    TestFalse(TEXT("An expired signed credential is rejected when loaded"), Credentials->SetAccessToken(U(Issuer.Sign(Expired)), Error));
    TestFalse(TEXT("An expired credential cannot join"), Credentials->ValidateForRole(TEXT("player"), Error));
    TestEqual(TEXT("Joining preserves the actual expiry diagnostic"), Error, FString(TEXT("AUTH_EXPIRED")));
    TokenSpec AnotherAudience = Spec;
    AnotherAudience.Audience = "another-arena";
    TestFalse(TEXT("A signed credential for another audience is rejected when loaded"), Credentials->SetAccessToken(U(Issuer.Sign(AnotherAudience)), Error));
    TestFalse(TEXT("A wrong-audience credential cannot join"), Credentials->ValidateForRole(TEXT("player"), Error));
    TestEqual(TEXT("Joining preserves the actual audience diagnostic"), Error, FString(TEXT("AUTH_AUDIENCE")));
    TestTrue(TEXT("The real token can be loaded again"), Credentials->SetAccessToken(Token, Error));
    TokenSpec AdminSpec=Spec; AdminSpec.Role="admin";
    TestTrue(TEXT("The client verifies an exact signed admin credential"),Credentials->SetAccessToken(U(Issuer.Sign(AdminSpec)),Error));
    TestEqual(TEXT("The verified role is exposed without the bearer secret"),Credentials->GetVerifiedRole(),FString(TEXT("admin")));
    TestTrue(TEXT("The admin client validates only its expected signed role"),Credentials->ValidateForRole(TEXT("admin"),Error));
    TestFalse(TEXT("Choosing player does not downgrade an admin JWT into a fighter"),Credentials->ValidateForRole(TEXT("player"),Error));
    Credentials->ClearAccessToken();
    TestTrue(TEXT("Clearing credentials removes the token and fingerprint"), Credentials->GetAccessToken().IsEmpty() && Credentials->GetServerFingerprint().IsEmpty());
    TestFalse(TEXT("An explicitly cleared credential cannot join"), Credentials->ValidateForRole(TEXT("player"), Error));
    TestEqual(TEXT("No credential remains AUTH_MISSING"), Error, FString(TEXT("AUTH_MISSING")));
    IFileManager::Get().Delete(*TokenPath);
    TestFalse(TEXT("A missing credential file cannot be reloaded"), Credentials->ReloadTokenFile(Error));
    TestFalse(TEXT("A missing-file credential cannot join"), Credentials->ValidateForRole(TEXT("player"), Error));
    TestEqual(TEXT("A missing credential file remains AUTH_MISSING"), Error, FString(TEXT("AUTH_MISSING")));
    Credentials->Deinitialize();
    IFileManager::Get().Delete(*PublicPath);
    IFileManager::Get().Delete(*TokenPath);
    IFileManager::Get().DeleteDirectory(*Directory, false, false);
    return true;
}
#endif
