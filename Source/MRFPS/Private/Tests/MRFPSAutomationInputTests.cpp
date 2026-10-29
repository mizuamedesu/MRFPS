#if WITH_DEV_AUTOMATION_TESTS
#include "MRFPSLocalInputLease.h"
#include "MRFPSLocalPoseStatus.h"
#include "Misc/AutomationTest.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSLocalInputLeaseTest,"MRFPS.Automation.DesktopInputLease",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSLocalInputLeaseTest::RunTest(const FString& Parameters)
{
    FMRFPSLocalInputLease Lease;
    TestFalse(TEXT("No input is active initially"),Lease.Active(10));
    TestTrue(TEXT("A bounded lease is accepted"),Lease.Set(true,true,FVector2D(1,0),FVector2D(90,-45),.5,10));
    TestEqual(TEXT("A request never applies the frame time before its acceptance"),Lease.ConsumeStep(10,1),0.);
    TestTrue(TEXT("An ordinary frame uses only elapsed time"),FMath::IsNearlyEqual(Lease.ConsumeStep(10.02,1),.02,1.e-6));
    TestEqual(TEXT("A long frame caps automatic movement"),Lease.ConsumeStep(10.3,1),.05);
    TestTrue(TEXT("Desired input is held before expiry"),Lease.Active(10.49) && Lease.Fire && Lease.Shield);
    TestFalse(TEXT("At expiry it cannot fire or move"),Lease.Active(10.5));
    TestEqual(TEXT("An expired lease cannot advance movement"),Lease.ConsumeStep(10.5,1),0.);
    TestFalse(TEXT("Long duration is rejected"),Lease.Set(true,false,FVector2D::ZeroVector,FVector2D::ZeroVector,1.01,11));
    TestFalse(TEXT("An out of range movement is rejected"),Lease.Set(true,false,FVector2D(1.01,0),FVector2D::ZeroVector,.1,11));
    TestFalse(TEXT("An out of range look rate is rejected"),Lease.Set(true,false,FVector2D::ZeroVector,FVector2D(181,0),.1,11));
    TestFalse(TEXT("Non-finite values are rejected"),Lease.Set(true,false,FVector2D::ZeroVector,FVector2D::ZeroVector,
        std::numeric_limits<double>::quiet_NaN(),11));
    TestFalse(TEXT("An invalid request cannot reactivate the expired lease"),Lease.Active(11));
    TestTrue(TEXT("A new bounded command may be applied"),Lease.Set(false,true,FVector2D(0,1),FVector2D::ZeroVector,.2,11));
    Lease.Clear();
    TestFalse(TEXT("Clear releases input immediately"),Lease.Active(11.01));
    TestFalse(TEXT("Clear removes the desired fire and shield"),Lease.Fire || Lease.Shield);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSLocalPoseStatusTest,"MRFPS.Automation.LocalPoseStatus",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSLocalPoseStatusTest::RunTest(const FString& Parameters)
{
    FJsonObject Info;
    const FVector Position(12,34,170);
    const FRotator Rotation(10,20,30);
    MRFPSLocalPoseStatus::Write(Info,Position,Rotation,true,true);
    TestEqual(TEXT("Pose provenance is explicit"),Info.GetStringField(TEXT("local_head_pose_source")),FString(TEXT("local_sample_not_server_confirmed")));
    TestTrue(TEXT("Fresh tracked head pose is available"),Info.GetBoolField(TEXT("local_head_pose_fresh")));
    const auto& Coordinates=Info.GetArrayField(TEXT("local_head_position_cm"));
    const auto& Angles=Info.GetArrayField(TEXT("local_view_rotation_degrees"));
    if (TestEqual(TEXT("Position contains three coordinates"),Coordinates.Num(),3) &&
        TestEqual(TEXT("Rotation contains pitch, yaw, roll"),Angles.Num(),3)) {
        TestTrue(TEXT("World position is preserved"),Coordinates[0]->AsNumber()==12 && Coordinates[1]->AsNumber()==34 && Coordinates[2]->AsNumber()==170);
        TestTrue(TEXT("View rotation uses pitch/yaw/roll order"),Angles[0]->AsNumber()==10 && Angles[1]->AsNumber()==20 && Angles[2]->AsNumber()==30);
    }
    MRFPSLocalPoseStatus::Write(Info,Position,Rotation,false,true);
    TestFalse(TEXT("A stale sample is unavailable"),Info.GetBoolField(TEXT("local_head_pose_fresh")));
    TestFalse(TEXT("Stale coordinates are removed"),Info.HasField(TEXT("local_head_position_cm")) || Info.HasField(TEXT("local_view_rotation_degrees")));
    MRFPSLocalPoseStatus::Write(Info,Position,Rotation,true,false);
    TestFalse(TEXT("An untracked head is unavailable"),Info.GetBoolField(TEXT("local_head_pose_fresh")));
    FVector Invalid=Position; Invalid.X=std::numeric_limits<double>::infinity();
    MRFPSLocalPoseStatus::Write(Info,Invalid,Rotation,true,true);
    TestFalse(TEXT("Non-finite coordinates are unavailable"),Info.GetBoolField(TEXT("local_head_pose_fresh")));
    TestFalse(TEXT("Invalid coordinates never enter JSON"),Info.HasField(TEXT("local_head_position_cm")) || Info.HasField(TEXT("local_view_rotation_degrees")));
    return true;
}
#endif
