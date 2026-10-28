#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "MRFPSOSC.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSGloveOSCValidationTest, "MRFPS.Gloves.OSCValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMRFPSGloveOSCValidationTest::RunTest(const FString& Parameters)
{
    // /a, types ,if: 1 and 1.0f encoded in network byte order.
    const uint8 Valid[] = { '/', 'a', 0, 0, ',', 'i', 'f', 0, 0, 0, 0, 1, 0x3f, 0x80, 0, 0 };
    MRFPSOSC::FMessage Message;
    TestTrue(TEXT("Valid OSC packet"), MRFPSOSC::Parse(Valid, sizeof(Valid), Message));
    TestEqual(TEXT("Integer byte order"), Message.Arguments[0].Integer, 1);
    TestEqual(TEXT("Float byte order"), Message.Arguments[1].Number, 1.0f);
    TestTrue(TEXT("Typed schema"), MRFPSOSC::Match(Message, TEXT("if")));
    TestFalse(TEXT("Wrong typed schema"), MRFPSOSC::Match(Message, TEXT("fi")));
    TestFalse(TEXT("Truncated packet"), MRFPSOSC::Parse(Valid, sizeof(Valid) - 1, Message));
    uint8 NonFinite[sizeof(Valid)];
    FMemory::Memcpy(NonFinite, Valid, sizeof(Valid));
    NonFinite[12] = 0x7f;
    NonFinite[13] = 0xc0;
    TestFalse(TEXT("NaN cannot become tracked input"), MRFPSOSC::Parse(NonFinite, sizeof(NonFinite), Message));
    TestFalse(TEXT("Never-received data is invalid"), MRFPSOSC::IsFresh(-1.0, 10.0, 0.15));
    TestFalse(TEXT("Stale data is invalid"), MRFPSOSC::IsFresh(1.0, 1.2, 0.15));
    TestFalse(TEXT("Future receipt is invalid"), MRFPSOSC::IsFresh(1.0, 0.9, 0.15));
    TestTrue(TEXT("Fresh data"), MRFPSOSC::IsFresh(1.0, 1.1, 0.15));
    TestFalse(TEXT("Fresh default samples from a disconnected glove are invalid"), MRFPSOSC::IsConnectedSample(false, 1.0, 1.0, 1.1, 0.15));
    TestFalse(TEXT("Unknown connection state cannot validate hand data"), MRFPSOSC::IsConnectedSample(true, -1.0, 1.0, 1.1, 0.15));
    TestFalse(TEXT("A continuing hand stream cannot refresh a dead connection heartbeat"), MRFPSOSC::IsConnectedSample(true, 0.8, 1.0, 1.1, 0.15));
    TestTrue(TEXT("Fresh connected hand data is valid"), MRFPSOSC::IsConnectedSample(true, 1.0, 1.0, 1.1, 0.15));
    const FTransform Mount(FQuat::Identity, FVector(10, 0, 0));
    const FTransform Tracker(FQuat(FRotator(0, 90, 0)), FVector(100, 0, 0));
    const FTransform Hand = Mount * Tracker;
    TestTrue(TEXT("Mount offset rotates in tracker local space before translation"), Hand.GetLocation().Equals(FVector(100, 10, 0), 0.001));
    return true;
}
#endif
