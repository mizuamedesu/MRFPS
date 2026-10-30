#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "CoreMinimal.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace
{
struct FFixtureReply
{
    FCriticalSection Mutex;
    bool bDone=false;
    bool bSuccess=false;
    int32 Status=0;
    FString Body;
};

// Explicitly invoked fixture only. Both ports are loopback-only and are
// supplied by tools/auth/tls_fixture.py. No real device codes are sent.
class FPeerScenario final : public IAutomationLatentCommand
{
public:
    FPeerScenario(FAutomationTestBase* InTest,int32 InTrustedPort,int32 InUntrustedPort)
        : Test(InTest),TrustedPort(InTrustedPort),UntrustedPort(InUntrustedPort) {}

    virtual bool Update() override
    {
        if (!Request.IsValid()) {
            const int32 Port=Stage==2?UntrustedPort:TrustedPort;
            const TCHAR* Host=Stage==1?TEXT("127.0.0.1"):TEXT("localhost");
            Reply=MakeShared<FFixtureReply,ESPMode::ThreadSafe>();
            const auto Current=Reply.ToSharedRef();
            Request=FHttpModule::Get().CreateRequest();
            Request->SetURL(FString::Printf(TEXT("https://%s:%d/fixture"),Host,Port));
            Request->SetVerb(TEXT("GET"));
            Request->SetTimeout(8.f);
            Request->OnProcessRequestComplete().BindLambda([Current](FHttpRequestPtr,FHttpResponsePtr Response,bool Success) {
                FScopeLock Lock(&Current->Mutex);
                Current->bSuccess=Success;
                Current->Status=Response.IsValid()?Response->GetResponseCode():0;
                if (Response.IsValid()) Current->Body=Response->GetContentAsString();
                Current->bDone=true;
            });
            Deadline=FPlatformTime::Seconds()+12.;
            if (!Request->ProcessRequest()) {
                Test->AddError(TEXT("TLS fixture request did not start"));
                Request.Reset(); return true;
            }
            return false;
        }
        bool Done,Success; int32 Status; FString Body;
        {
            FScopeLock Lock(&Reply->Mutex);
            Done=Reply->bDone; Success=Reply->bSuccess; Status=Reply->Status; Body=Reply->Body;
        }
        if (!Done) {
            if (FPlatformTime::Seconds()<Deadline) return false;
            Request->OnProcessRequestComplete().Unbind(); Request->CancelRequest(); Request.Reset();
            Test->AddError(TEXT("TLS fixture request timed out")); return true;
        }
        Request.Reset(); Reply.Reset();
        if (Stage==0) {
            if (!Test->TestTrue(TEXT("The fixture CA and matching localhost name succeed"),
                                Success && Status==200 && Body==TEXT("MRFPS_TLS_FIXTURE_OK"))) return true;
        } else if (Stage==1) {
            Test->TestTrue(TEXT("A trusted certificate for localhost rejects the 127.0.0.1 name"),!Success && Status==0);
        } else {
            Test->TestTrue(TEXT("A certificate from the other local CA is rejected"),!Success && Status==0);
        }
        return ++Stage>=3;
    }
private:
    FAutomationTestBase* Test;
    int32 TrustedPort,UntrustedPort,Stage=0;
    double Deadline=0;
    TSharedPtr<IHttpRequest,ESPMode::ThreadSafe> Request;
    TSharedPtr<FFixtureReply,ESPMode::ThreadSafe> Reply;
};
}

// This is outside the normal MRFPS filter because it requires an explicit
// local HTTPS fixture and a per-process test CA; it must not silently skip.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSTlsFixtureTest,"TLSAuthFixture.PeerVerification",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FMRFPSTlsFixtureTest::RunTest(const FString& Parameters)
{
    int32 Trusted=0,Untrusted=0;
    if (!FParse::Value(FCommandLine::Get(),TEXT("MRFPSTlsTrustedPort="),Trusted) ||
        !FParse::Value(FCommandLine::Get(),TEXT("MRFPSTlsUntrustedPort="),Untrusted) ||
        Trusted<1 || Trusted>65535 || Untrusted<1 || Untrusted>65535 || Trusted==Untrusted) {
        AddError(TEXT("The dedicated loopback TLS fixture ports are required")); return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FPeerScenario(this,Trusted,Untrusted));
    return true;
}
#endif
