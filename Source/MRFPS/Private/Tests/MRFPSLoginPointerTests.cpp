#if WITH_DEV_AUTOMATION_TESTS
#include "MRFPSLoginPointer.h"
#include "../MRFPSHandPose.h"
#include "MRFPSGame.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
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

    // Position follows Grip while aiming follows Aim, exactly as gameplay.
    Grip=Aim; Grip.ControllerLocation=FVector(20,0,0); Grip.ControllerRotation=FRotator(0,45,0).Quaternion();
    FMRFPSWeaponPose Weapon;
    TestTrue(TEXT("Combined controller pose resolves"),MRFPSHandPose::ResolveRightWeapon(Aim,Grip,Hand,FTransform::Identity,Origin,false,FTransform::Identity,Weapon));
    TestTrue(TEXT("Grip supplies the weapon position"),Weapon.Source==ESource::AimGrip && Weapon.GripWorld.GetLocation().Equals(FVector(100,220,300),.01));
    TestTrue(TEXT("Aim supplies the weapon direction"),Weapon.GripWorld.GetRotation().GetForwardVector().Equals(FVector(0,1,0),.01));
    const FTransform Muzzle=MRFPSWeaponPose::Muzzle(Weapon.GripWorld);
    TestTrue(TEXT("Menu ray starts 22 cm at the firing muzzle"),Muzzle.GetLocation().Equals(FVector(100,242,300),.01));
    TestTrue(TEXT("Menu ray has the firing direction"),Muzzle.GetRotation().GetForwardVector().Equals(FVector(0,1,0),.01));
    Hand.bValid=true; Hand.TrackingStatus=ETrackingStatus::Tracked;
    Hand.HandKeyLocations.Init(FVector(10,0,0),EHandKeypointCount);
    Hand.HandKeyRotations.Init(FQuat::Identity,EHandKeypointCount);
    FMRFPSWeaponPose PalmWeapon; FXRMotionControllerState Missing;
    TestTrue(TEXT("Optical palm uses the same corrected weapon orientation"),
        MRFPSHandPose::ResolveRightWeapon(Missing,Missing,Hand,FTransform::Identity,Origin,false,FTransform::Identity,PalmWeapon));
    TestTrue(TEXT("Palm weapon applies the shared hand mounting"),PalmWeapon.Source==ESource::Hand &&
        PalmWeapon.GripWorld.GetRotation().Equals(MRFPSHandPose::WeaponFromHand(Origin.GetRotation()),.001));

    if (!TestNotNull(TEXT("Engine for login gun fixture"),GEngine)) return false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false)
        .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("Isolated login gun world"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    auto* Pawn=World->SpawnActor<AMRFPSPawn>();
    if (TestNotNull(TEXT("Pre-login pawn"),Pawn)) {
        TestTrue(TEXT("Pre-login slot is unassigned"),Pawn->GetSlot()<0);
        Pawn->SetLoginWeaponPose(&Weapon);
        TestTrue(TEXT("Tracked login gun is visible before player assignment"),Pawn->GunVisual && Pawn->GunVisual->IsVisible());
        Pawn->Pose.Right=Weapon.GripWorld.GetLocation(); Pawn->Pose.RightRotation=Weapon.GripWorld.GetRotation().Rotator();
        TestTrue(TEXT("Game shot and menu ray use the same muzzle"),Pawn->Muzzle().Equals(Muzzle.GetLocation(),.01));
        Pawn->bRequireFireRelease=true; Pawn->SetFiring(false);
        TestTrue(TEXT("Physical release alone cannot unlock a held glove"),Pawn->bRequireFireRelease);
        Pawn->SetFiring(true);
        TestTrue(TEXT("A held physical trigger cannot fire before rearming"),Pawn->bPhysicalFireDown && !Pawn->bFiring);
        Pawn->SetLoginWeaponPose(nullptr);
        TestFalse(TEXT("Untracked login gun is hidden"),Pawn->GunVisual->IsVisible());
    }
    World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    return true;
}
#endif
