#if WITH_DEV_AUTOMATION_TESTS
#include "MRFPSLoginPointer.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSLoginPointerPoseTest,"MRFPS.Login.PointerPose",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FMRFPSLoginPointerPoseTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSLoginPointer;
    FXRMotionControllerState Aim,Grip;
    FXRHandTrackingState Hand;
    TestFalse(TEXT("Untracked controller is invalid"),ValidPose(Aim));
    Aim.bValid=true; Aim.TrackingStatus=ETrackingStatus::Tracked;
    Aim.ControllerLocation=FVector(10,0,0); Aim.ControllerRotation=FQuat::Identity;
    TestTrue(TEXT("Tracked aim is valid"),ValidPose(Aim));
    TestTrue(TEXT("Aim wins over grip and hand"),Select(true,true,true)==ESource::Aim);
    TestTrue(TEXT("Grip is an approximate fallback"),Select(false,true,true)==ESource::Grip);
    TestTrue(TEXT("Optical hand is the final fallback"),Select(false,false,true)==ESource::Hand);
    TestTrue(TEXT("No pose yields no pointer"),Select(false,false,false)==ESource::None);

    const FTransform Origin(FRotator(0,90,0),FVector(100,200,300));
    const FTransform Controller=ControllerWorldPose(Aim,Origin);
    TestTrue(TEXT("Controller origin is transformed into pawn world"),Controller.GetLocation().Equals(FVector(100,210,300),.01));
    TestTrue(TEXT("Controller aim direction is transformed into pawn world"),Controller.GetRotation().GetForwardVector().Equals(FVector(0,1,0),.01));
    Grip=Aim; Grip.ControllerRotation=FQuat(0,0,0,0);
    TestFalse(TEXT("Zero controller orientation is rejected"),ValidPose(Grip));

    Hand.bValid=true; Hand.TrackingStatus=ETrackingStatus::Tracked;
    Hand.HandKeyLocations.Init(FVector::ZeroVector,EHandKeypointCount);
    Hand.HandKeyRotations.Init(FQuat::Identity,EHandKeypointCount);
    Hand.HandKeyLocations[static_cast<int32>(EHandKeypoint::Palm)]=FVector(10,0,0);
    TestTrue(TEXT("Tracked hand palm is valid"),ValidHandPose(Hand));
    const FTransform HandPose=HandWorldPose(Hand,FTransform::Identity,Origin);
    TestTrue(TEXT("Hand palm is transformed into pawn world"),HandPose.GetLocation().Equals(FVector(100,210,300),.01));
    Hand.HandKeyRotations[static_cast<int32>(EHandKeypoint::Palm)]=FQuat(0,0,0,0);
    TestFalse(TEXT("Invalid palm orientation is rejected"),ValidHandPose(Hand));
    Hand.HandKeyLocations.Reset();
    TestFalse(TEXT("Missing hand joints are rejected"),ValidHandPose(Hand));
    return true;
}
#endif
