#if WITH_DEV_AUTOMATION_TESTS
#include "MRFPSRosterPublisher.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSRosterHmacTest,"MRFPS.Admin.RosterHmac",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSRosterHmacTest::RunTest(const FString& Parameters)
{
    const FString Key=FString::ChrN(64,TEXT('a'));
    TestEqual(TEXT("HMAC-SHA256 agrees with an independent known vector"),
        FMRFPSRosterPublisher::HmacSha256(Key,TEXT("fixture-roster-payload")),
        FString(TEXT("c1d3d66447b7e7165ddcd072bf23dd47e8a74a11f93a85c92a5119dcf9aa19c0")));
    TestNotEqual(TEXT("A changed body produces a different signature"),
        FMRFPSRosterPublisher::HmacSha256(Key,TEXT("fixture-roster-payload")),
        FMRFPSRosterPublisher::HmacSha256(Key,TEXT("fixture-roster-payload!")));
    return true;
}
#endif
