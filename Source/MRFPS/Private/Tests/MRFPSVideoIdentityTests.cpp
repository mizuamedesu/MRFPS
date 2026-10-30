#include "MRFPSIpConnection.h"

#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoIdentityTest, "MRFPS.Video.MeasurementIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMRFPSVideoIdentityTest::RunTest(const FString& Parameters)
{
    auto* First=NewObject<UMRFPSIpConnection>();
    auto* Second=NewObject<UMRFPSIpConnection>();
    if (!TestNotNull(TEXT("First connection exists"),First) || !TestNotNull(TEXT("Second connection exists"),Second)) return false;
    TestTrue(TEXT("Connection identifiers are nonzero and distinct"),First->GetDiagnosticConnectionId().IsValid() &&
        Second->GetDiagnosticConnectionId().IsValid() && First->GetDiagnosticConnectionId()!=Second->GetDiagnosticConnectionId());
    const FString Pin=FString::ChrN(64,TCHAR('a'));
    First->RecordDiagnosticServerPin(Pin.ToUpper());
    TestEqual(TEXT("A valid handshake pin is canonicalized"),First->GetDiagnosticServerPin(),Pin);
    First->RecordDiagnosticServerPin(Pin);
    TestEqual(TEXT("Repeated matching ack retains its pin"),First->GetDiagnosticServerPin(),Pin);
    First->RecordDiagnosticServerPin(FString::ChrN(64,TCHAR('b')));
    TestTrue(TEXT("Conflicting pins are permanently ambiguous"),First->GetDiagnosticServerPin().IsEmpty());
    First->RecordDiagnosticServerPin(Pin);
    TestTrue(TEXT("An ambiguous connection cannot silently regain identity"),First->GetDiagnosticServerPin().IsEmpty());
    Second->RecordDiagnosticServerPin(TEXT("invalid"));
    Second->RecordDiagnosticServerPin(Pin);
    TestTrue(TEXT("An invalid first pin also fails closed"),Second->GetDiagnosticServerPin().IsEmpty());
    return true;
}
#endif
