#include "MRFPSGame.h"
#include "../MRFPSHandPose.h"
#include "../MRFPSGloveInput.h"
#include "../MRFPSDeviceAuthUrl.h"
#include "../MRFPSConnectionSettings.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "MRFPSHapticsComponent.h"
#include "Misc/AutomationTest.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSGloveFingerSourceTest,"MRFPS.Gloves.FingerSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSGloveFingerSourceTest::RunTest(const FString& Parameters)
{
    FMRFPSGloveHandFrame Left,Right;
    FMRFPSFingerPose LeftPose,RightPose;
    Left.bFlexValid=Right.bFlexValid=true;
    Left.FingerCurls={.18f,.12f,.07f,.10f,.37f};
    Left.Flexion[0]=.25f;Left.Splay[0]=-.5f;
    Right.Flexion[1]=.75f;Right.Splay[1]=.5f;
    MRFPSGloveInput::CopyFingers(Left,LeftPose);MRFPSGloveInput::CopyFingers(Right,RightPose);
    TestTrue(TEXT("Both hands use their own ContactGlove finger samples"),LeftPose.bValid && RightPose.bValid);
    TestEqual(TEXT("Left glove flex is quantized for replication"),int32(LeftPose.Joint[0]),64);
    TestEqual(TEXT("Right glove flex is independently quantized"),int32(RightPose.Joint[1]),191);
    TestEqual(TEXT("Left glove spread is preserved"),int32(LeftPose.Splay[0]),64);
    TestEqual(TEXT("Right glove spread is preserved"),int32(RightPose.Splay[1]),191);
    TestEqual(TEXT("Fresh open glove activates the palm gesture"),MRFPSGloveInput::LeftOpenness(Left),1.f);
    Left.bFlexValid=false;MRFPSGloveInput::CopyFingers(Left,LeftPose);
    TestFalse(TEXT("Stale glove data cannot be presented as fresh finger tracking"),LeftPose.bValid);
    TestEqual(TEXT("A missing glove cannot fall back to an optical open-palm gesture"),MRFPSGloveInput::LeftOpenness(Left),0.f);
    TestTrue(TEXT("Loss of the left glove does not invalidate the right"),RightPose.bValid);
    return true;
}

namespace MRFPSCoreTests
{
FMRFPSPose TrackedPose()
{
    FMRFPSPose Pose;
    Pose.bHeadTracked = Pose.bLeftTracked = Pose.bRightTracked = true;
    Pose.Head = FVector(0, 0, 170);
    Pose.Left = FVector(30, -30, 125);
    Pose.Right = FVector(40, 25, 140);
    return Pose;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSPoseValidationTest, "MRFPS.Core.PoseValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSPoseValidationTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSCoreTests;
    const FMRFPSPose Valid = TrackedPose();
    TestTrue(TEXT("An ordinary standing player has a usable pose"), Valid.IsSane());
    TestTrue(TEXT("A tracked player at the arena centre is inside"), Valid.IsInsideArena());
    for (const FVector& Boundary : {FVector(-225, 0, 170), FVector(225, 0, 170),
                                   FVector(0, -350, 170), FVector(0, 350, 170)})
    {
        FMRFPSPose Pose = Valid;
        Pose.Head = Boundary;
        TestTrue(TEXT("The fixed 4.5 by 7 metre arena includes its boundary"), Pose.IsInsideArena());
    }
    for (const FVector& Outside : {FVector(-225.1, 0, 170), FVector(225.1, 0, 170),
                                  FVector(0, -350.1, 170), FVector(0, 350.1, 170)})
    {
        FMRFPSPose Pose = Valid;
        Pose.Head = Outside;
        TestFalse(TEXT("Crossing any arena edge disables arena membership"), Pose.IsInsideArena());
    }
    FMRFPSPose Pose = Valid;
    Pose.bHeadTracked = false;
    TestFalse(TEXT("An untracked head cannot count as inside the arena"), Pose.IsInsideArena());

    Pose = Valid;
    Pose.Head.Z = 10;
    TestFalse(TEXT("A head pose below the usable floor range is outside"), Pose.IsInsideArena());
    Pose = Valid;
    Pose.Head.Z = 300;
    TestFalse(TEXT("A head pose above the usable height range is outside"), Pose.IsInsideArena());
    Pose = Valid;
    Pose.Left = FVector(Pose.Head) + FVector(181, 0, 0);
    TestFalse(TEXT("A tracked hand cannot extend beyond plausible arm reach"), Pose.IsSane());
    Pose.bLeftTracked = false;
    TestTrue(TEXT("A finite stale hand location does not invalidate head tracking"), Pose.IsSane());
    Pose = Valid;
    Pose.Right = FVector(Pose.Head) + FVector(0, 0, 181);
    TestFalse(TEXT("The right hand is subject to the same reach validation"), Pose.IsSane());

    for (const float Openness : {-0.1f, 1.1f, std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity()})
    {
        Pose = Valid;
        Pose.LeftOpenness = Openness;
        TestFalse(TEXT("Malformed palm input cannot activate a shield"), Pose.IsSane());
    }
    Pose = Valid;
    Pose.LeftOpenness = 1.f;
    TestTrue(TEXT("A fully open palm is valid input"), Pose.IsSane());
    Pose.Head.X = std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("Non-finite head coordinates are rejected"), Pose.IsSane());
    Pose = Valid;
    Pose.Right.X = std::numeric_limits<double>::infinity();
    TestFalse(TEXT("Non-finite hand coordinates are rejected"), Pose.IsSane());
    Pose = Valid;
    Pose.LeftRotation.Yaw = std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("Non-finite hand orientation is rejected"), Pose.IsSane());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSHandWeaponAimTest, "MRFPS.Core.HandWeaponAim",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSHandWeaponAimTest::RunTest(const FString& Parameters)
{
    FXRHandTrackingState Hand;
    Hand.bValid=true; Hand.TrackingStatus=ETrackingStatus::Tracked;
    Hand.HandKeyLocations.SetNum(EHandKeypointCount);
    Hand.HandKeyRotations.SetNum(EHandKeypointCount);
    TestTrue(TEXT("A tracked controller remains the preferred position source"),
        MRFPSHandPose::SelectPositionSource(true,Hand)==MRFPSHandPose::EPositionSource::Controller);
    TestTrue(TEXT("Optical position remains available independently of glove flex"),
        MRFPSHandPose::SelectPositionSource(false,Hand)==MRFPSHandPose::EPositionSource::OpticalHand);
    Hand.TrackingStatus=ETrackingStatus::NotTracked;
    TestTrue(TEXT("Untracked optical input does not create a position"),
        MRFPSHandPose::SelectPositionSource(false,Hand)==MRFPSHandPose::EPositionSource::None);
    Hand.TrackingStatus=ETrackingStatus::Tracked;
    const int32 Palm=static_cast<int32>(EHandKeypoint::Palm);
    const int32 IndexTip=static_cast<int32>(EHandKeypoint::IndexTip);
    Hand.HandKeyLocations[Palm]=FVector(100,20,130);
    Hand.HandKeyRotations[Palm]=FRotationMatrix::MakeFromXZ(FVector::RightVector,FVector::ForwardVector).ToQuat();
    const FTransform EngineToTracking(FRotator(0,-25,0),FVector(5,10,0));
    const FTransform TrackingToArena(FRotator(0,55,0),FVector(-15,8,0));
    FVector Position; FRotator Rotation;
    TestTrue(TEXT("A tracked palm yields a hand pose"),MRFPSHandPose::RightPalmPose(Hand,EngineToTracking,TrackingToArena,Position,Rotation));
    const FQuat Expected=TrackingToArena.TransformRotation(EngineToTracking.TransformRotation(Hand.HandKeyRotations[Palm])).GetNormalized();
    TestTrue(TEXT("Tracked hand forward follows the palm"),FVector::DotProduct(Rotation.Vector(),Expected.GetAxisX())>.9999);
    const FQuat Weapon=MRFPSHandPose::WeaponFromHand(Rotation.Quaternion());
    TestTrue(TEXT("The corrected optical gun basis follows the palm's forward axis"),FVector::DotProduct(Weapon.GetAxisX(),Expected.GetAxisX())>.9999);
    const FQuat DownwardOldAim=FRotationMatrix::MakeFromXZ(FVector::ForwardVector,FVector::RightVector).ToQuat();
    const FQuat CorrectedAim=MRFPSHandPose::WeaponFromHand(DownwardOldAim);
    TestTrue(TEXT("The reported downward aim is corrected by 90 degrees"),
        FVector::DotProduct(DownwardOldAim.GetAxisY(),-FVector::UpVector)>.9999 &&
        FVector::DotProduct(CorrectedAim.GetAxisX(),FVector::ForwardVector)>.9999 &&
        FVector::DotProduct(CorrectedAim.GetAxisZ(),FVector::UpVector)>.9999);
    TestTrue(TEXT("A controller's existing aim ray can infer the holding hand without changing the ray"),
        MRFPSHandPose::HandFromWeapon(Weapon).Equals(Expected,.0001));
    TestTrue(TEXT("Palm position is transformed into the calibrated arena once"),FVector::Dist(Position,
        TrackingToArena.TransformPosition(EngineToTracking.TransformPosition(Hand.HandKeyLocations[Palm])))<.001);
    Hand.HandKeyLocations[IndexTip]=FVector(-100,-200,-300);
    FVector CurledPosition; FRotator CurledRotation;
    TestTrue(TEXT("A curled finger retains a valid palm pose"),MRFPSHandPose::RightPalmPose(Hand,EngineToTracking,TrackingToArena,CurledPosition,CurledRotation));
    TestTrue(TEXT("Curling the trigger finger does not turn the gun"),FVector::DotProduct(Rotation.Vector(),CurledRotation.Vector())>.9999);
    Hand.TrackingStatus=ETrackingStatus::NotTracked;
    TestFalse(TEXT("Lost hand tracking cannot override a controller pose"),MRFPSHandPose::RightPalmPose(Hand,EngineToTracking,TrackingToArena,Position,Rotation));
    Hand.TrackingStatus=ETrackingStatus::Tracked; Hand.HandKeyRotations[Palm]=FQuat(0,0,0,0);
    TestFalse(TEXT("An invalid palm rotation cannot produce a weapon pose"),MRFPSHandPose::RightPalmPose(Hand,EngineToTracking,TrackingToArena,Position,Rotation));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSRiseGestureTest, "MRFPS.Core.RiseGesture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSRiseGestureTest::RunTest(const FString& Parameters)
{
    FMRFPSRiseGesture Gesture;
    TestFalse(TEXT("The first hand sample establishes a baseline"), Gesture.Update(80, 10, true));
    TestFalse(TEXT("A short initial rise does not erect a wall"), Gesture.Update(90, 10.05, true));
    TestTrue(TEXT("A deliberate upward hand stroke is recognized"), Gesture.Update(120, 10.10, true));
    TestFalse(TEXT("Continuing the same stroke does not retrigger"), Gesture.Update(150, 10.15, true));
    TestFalse(TEXT("Holding the raised hand does not retrigger"), Gesture.Update(150, 10.20, true));
    TestFalse(TEXT("Lowering the hand rearms without firing"), Gesture.Update(80, 10.25, true));
    TestFalse(TEXT("A fresh rise starts a separate gesture"), Gesture.Update(100, 10.30, true));
    TestTrue(TEXT("A second complete stroke can be recognized"), Gesture.Update(120, 10.36, true));

    Gesture.Reset();
    Gesture.Update(80, 20, true);
    TestFalse(TEXT("An implausibly fast jump is not a deliberate stroke"), Gesture.Update(120, 20.02, true));
    TestFalse(TEXT("Waiting after an instantaneous jump cannot turn it into a stroke"), Gesture.Update(120, 20.12, true));

    Gesture.Reset();
    bool bSlowRiseTriggered = false;
    for (int32 Index = 0; Index <= 15; ++Index)
        bSlowRiseTriggered |= Gesture.Update(80 + Index * 4.f, 30 + Index * 0.1, true);
    TestTrue(TEXT("A natural sustained upward arm stroke is recognized"), bSlowRiseTriggered);
    Gesture.Reset(); bool bDriftTriggered=false;
    for (int32 Index=0; Index<=15; ++Index)
        bDriftTriggered |= Gesture.Update(80+Index*.5f, 35+Index*.1, true);
    TestFalse(TEXT("Small slow tracking drift does not erect a wall"), bDriftTriggered);

    Gesture.Reset();
    Gesture.Update(80, 40, true);
    Gesture.Update(100, 40.05, true);
    TestFalse(TEXT("Tracking loss cancels the partial stroke"), Gesture.Update(0, 40.10, false));
    TestFalse(TEXT("Reacquiring an elevated hand does not complete a lost stroke"), Gesture.Update(150, 40.15, true));
    TestFalse(TEXT("A steady reacquired hand remains inactive"), Gesture.Update(150, 40.25, true));

    Gesture.Reset();
    Gesture.Update(80, 50, true);
    Gesture.Update(100, 50.05, true);
    TestFalse(TEXT("A long sample outage resets motion history"), Gesture.Update(150, 50.4, true));
    TestFalse(TEXT("A duplicate timestamp cannot contribute motion"), Gesture.Update(190, 50.4, true));
    TestFalse(TEXT("A backward timestamp cannot contribute motion"), Gesture.Update(230, 49.9, true));
    TestFalse(TEXT("A NaN sample resets safely"), Gesture.Update(std::numeric_limits<float>::quiet_NaN(), 51, true));
    TestFalse(TEXT("An infinite clock value resets safely"), Gesture.Update(80, std::numeric_limits<double>::infinity(), true));
    TestFalse(TEXT("The next finite sample starts a new baseline"), Gesture.Update(80, 60, true));
    TestTrue(TEXT("A valid stroke still works after malformed tracking"), Gesture.Update(120, 60.1, true));
    return true;
}

namespace MRFPSCoreTests
{
// These are actual authority actors in an isolated world with physics queries.
// They have no local controller, so XR sampling and MoBits output never execute.
class FAuthorityScenario final : public IAutomationLatentCommand
{
public:
    explicit FAuthorityScenario(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual ~FAuthorityScenario() override
    {
        if (World)
        {
            World->EndPlay(EEndPlayReason::EndPlayInEditor);
            World->DestroyWorld(false);
            GEngine->DestroyWorldContext(World);
        }
    }

    virtual bool Update() override
    {
        // TimerManager ticks once per engine frame. Keep the lifespan test on
        // real successive automation frames instead of manipulating its clock.
        if (LastFrame == GFrameCounter) return false;
        LastFrame = GFrameCounter;
        if (!World) return !CreateWorld();
        RefreshTracking();
        World->Tick(LEVELTICK_All, 0.05f);
        const double Now = World->GetTimeSeconds();
        if (Now > 15)
        {
            Test->AddError(TEXT("Authority combat scenario exceeded its simulated deadline"));
            return true;
        }

        switch (Stage)
        {
        case 0:
            Test->TestTrue(TEXT("Combat fixture is authoritative"), Shooter->HasAuthority() && Target->HasAuthority());
            Test->TestTrue(TEXT("Both registered aligned fighters can act"), Shooter->CanAct() && Target->CanAct());
            Test->TestTrue(TEXT("The visible gun points along its server muzzle trace"),FVector::DotProduct(
                Shooter->GunVisual->GetForwardVector(),(Shooter->Muzzle()-FVector(Shooter->Pose.Right)).GetSafeNormal())>.999);
            Shooter->ServerFire();
            Test->TestEqual(TEXT("A real weapon trace damages the opposing body"), Target->Health, 80.f);
            Shooter->ServerFire();
            Test->TestEqual(TEXT("Duplicate fire requests in one frame are rate limited"), Shooter->AcceptedShots, 1);
            Test->TestEqual(TEXT("Rejected fire cannot cause extra damage"), Target->Health, 80.f);
            LastShotAt = Now;
            ++Stage;
            break;
        case 1:
            if (Now - LastShotAt < .16) break;
            Shooter->ServerFire();
            Test->TestEqual(TEXT("Fire resumes after the server fire interval"), Shooter->AcceptedShots, 2);
            Test->TestEqual(TEXT("The second accepted trace causes one body hit"), Target->Health, 60.f);
            LastShotAt = Now;
            ++Stage;
            break;
        case 2:
            if (Now - LastShotAt < .16) break;
            Shooter->Pose.bRightTracked = false;
            Shooter->ServerFire();
            Test->TestEqual(TEXT("Losing the weapon hand disables firing"), Shooter->AcceptedShots, 2);
            Shooter->Pose.bRightTracked = true;
            Shooter->ServerFire();
            Test->TestEqual(TEXT("Rejected tracking does not consume the next valid shot"), Shooter->AcceptedShots, 3);
            Test->TestEqual(TEXT("Recovered weapon tracking can damage again"), Target->Health, 40.f);
            Target->Pose.LeftOpenness = 1;
            LastShotAt = Now;
            ++Stage;
            break;
        case 3:
            if (Now - LastShotAt < .16) break;
            {
                Test->TestTrue(TEXT("The open left palm activates the mini shield"), Target->bShieldActive);
                const float Before = Target->ShieldEnergy;
                Shooter->ServerFire();
                Test->TestEqual(TEXT("The real shield collision blocks a shot before the body"), Target->Health, 40.f);
                Test->TestTrue(TEXT("A blocked shot consumes shield energy"), FMath::IsNearlyEqual(Target->ShieldEnergy, Before - 22.f));
                Target->Pose.LeftOpenness = 0;
                LastShotAt = Now;
            }
            VerifyRestrictions();
            Shooter->DeployWall();
            Test->TestEqual(TEXT("An aligned fighter can create one barrier"), CountWalls(), 1);
            for (TActorIterator<AMRFPSBarrier> It(World); It; ++It) FirstWall = *It;
            if (!FirstWall.IsValid()) return true;
            WallSpawnedAt = Now;
            Test->TestEqual(TEXT("Barrier ownership identifies its fighter"), FirstWall->OwnerSlot, 0);
            Test->TestTrue(TEXT("Barrier lifespan is scheduled for six seconds"), FMath::IsNearlyEqual(FirstWall->GetLifeSpan(), 6.f, .1f));
            Shooter->DeployWall();
            Test->TestEqual(TEXT("Repeated wall requests obey the cooldown"), CountWalls(), 1);
            Target->ApplyShot(40, Shooter, false);
            Test->TestEqual(TEXT("Lethal authority damage eliminates the fighter"), Target->Health, 0.f);
            Test->TestFalse(TEXT("An eliminated fighter cannot act"), Target->CanAct());
            Target->ServerFire();
            Test->TestEqual(TEXT("An eliminated fighter cannot shoot"), Target->AcceptedShots, 0);
            DeathAt = Now;
            ++Stage;
            break;
        case 4:
            if (Now - LastShotAt < .16) break;
            Shooter->ServerFire();
            Test->TestTrue(TEXT("The wall remains alive after one bullet"), FirstWall.IsValid());
            if (FirstWall.IsValid()) Test->TestEqual(TEXT("A real bullet damages the wall in its path"), FirstWall->Health, 100.f);
            ++Stage;
            break;
        case 5:
            if (!bCheckedDead && Now - DeathAt >= 2.8)
            {
                Test->TestEqual(TEXT("The respawn delay keeps the fighter eliminated"), Target->Health, 0.f);
                bCheckedDead = true;
            }
            if (Now - DeathAt < 3.15) break;
            Test->TestEqual(TEXT("Normal authority ticking respawns the fighter after the delay"), Target->Health, 100.f);
            Test->TestTrue(TEXT("A respawned aligned fighter can act"), Target->CanAct());
            ++Stage;
            break;
        case 6:
            if (Now - WallSpawnedAt < 3.8) break;
            Shooter->DeployWall();
            Test->TestEqual(TEXT("The wall cooldown remains active before four seconds"), CountWalls(), 1);
            ++Stage;
            break;
        case 7:
            if (Now - WallSpawnedAt < 4.15) break;
            Shooter->DeployWall();
            Test->TestEqual(TEXT("A new wall is allowed after the actual cooldown"), CountWalls(), 2);
            ++Stage;
            break;
        case 8:
            if (Now - WallSpawnedAt < 6.2) break;
            Test->TestTrue(TEXT("The first wall is destroyed by its real lifespan timer"),
                !FirstWall.IsValid() || FirstWall->IsActorBeingDestroyed());
            Test->TestEqual(TEXT("The later wall retains its own lifespan"), CountWalls(), 1);
            ++Stage;
            break;
        case 9:
            if (Now - WallSpawnedAt < 10.5) break;
            Test->TestEqual(TEXT("The second wall also expires without manual cleanup"), CountWalls(), 0);
            return true;
        default: return true;
        }
        return false;
    }

private:
    bool CreateWorld()
    {
        if (!GEngine)
        {
            Test->AddError(TEXT("An engine is required for the authority world test"));
            return false;
        }
        const UWorld::InitializationValues Init = UWorld::InitializationValues()
            .AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false).EnableTraceCollision(true)
            .CreateFXSystem(false).SetTransactional(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
        if (!World)
        {
            Test->AddError(TEXT("Failed to create the authority test world"));
            return false;
        }
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        State = World->SpawnActor<AMRFPSGameState>();
        World->SetGameState(State);
        World->InitializeActorsForPlay(FURL());
        World->BeginPlay();
        World->GetWorldSettings()->NotifyBeginPlay();
        State->Phase = EMRFPSMatchPhase::Playing;
        State->bNominalDesktopSession=true;
        Shooter = SpawnFighter(0, FVector(-200, 0, 170), 0);
        Target = SpawnFighter(1, FVector(200, 0, 170), 180);
        if (!Shooter || !Target)
        {
            Test->AddError(TEXT("Failed to spawn both authority fighters"));
            return false;
        }
        auto* Inspector = World->SpawnActor<AMRFPSInspectorPawn>();
        Test->TestNotNull(TEXT("An inspector has its own free-view pawn"), Inspector);
        if (Inspector) Test->TestNull(TEXT("An inspector does not own a MoBits output component"), Inspector->FindComponentByClass<UMRFPSHapticsComponent>());
        return true;
    }

    AMRFPSPawn* SpawnFighter(int32 Slot, const FVector& Head, float Yaw)
    {
        auto* Pawn = World->SpawnActor<AMRFPSPawn>();
        auto* Player = World->SpawnActor<AMRFPSPlayerState>();
        if (!Pawn || !Player) return nullptr;
        Player->Slot = Slot;
        Player->bAuthenticated = true;
        Player->AuthExpiresAt = FDateTime::UtcNow().ToUnixTimestamp()+3600;
        Pawn->SetPlayerState(Player);
        Pawn->Haptics->bAutoConnectTracking = false;
        Pawn->Pose = TrackedPose();
        Pawn->Pose.bDesktop=true;
        Pawn->Pose.Head = Head;
        const FRotator Rotation(0, Yaw, 0);
        Pawn->Pose.HeadRotation = Pawn->Pose.LeftRotation = Pawn->Pose.RightRotation = Pawn->Pose.RightHandRotation = Rotation;
        Pawn->Pose.Left = Head + Rotation.RotateVector(FVector(40, 0, -40));
        Pawn->Pose.Right = Head + Rotation.RotateVector(FVector(40, 0, -40));
        Pawn->bCalibrated = true;
        Pawn->LastPoseReceived = World->GetTimeSeconds();
        Pawn->OnRepPose();
        return Pawn;
    }

    void RefreshTracking()
    {
        if (Shooter) Shooter->LastPoseReceived = World->GetTimeSeconds();
        if (Target) Target->LastPoseReceived = World->GetTimeSeconds();
    }

    int32 CountWalls() const
    {
        int32 Count = 0;
        for (TActorIterator<AMRFPSBarrier> It(World); It; ++It) ++Count;
        return Count;
    }

    void VerifyRestrictions()
    {
        auto* Player = Shooter->GetPlayerState<AMRFPSPlayerState>();
        const float Before = Target->Health;
        Player->bInspector = true;
        Test->TestFalse(TEXT("An inspector role cannot act through a fighter pawn"), Shooter->CanAct());
        Target->ApplyShot(20, Shooter, false);
        Test->TestEqual(TEXT("An inspector cannot cause damage"), Target->Health, Before);
        Player->bInspector = false;
        Player->Slot = -1;
        Test->TestFalse(TEXT("An unregistered player cannot act"), Shooter->CanAct());
        Player->Slot = 0;

        Shooter->bCalibrated = false;
        Test->TestFalse(TEXT("An unaligned fighter cannot act"), Shooter->CanAct());
        Target->ApplyShot(20, Shooter, false);
        Test->TestEqual(TEXT("An unaligned shooter cannot damage another fighter"), Target->Health, Before);
        Shooter->bCalibrated = true;
        Shooter->LastPoseReceived -= 1;
        Test->TestFalse(TEXT("Stale tracking disables combat"), Shooter->CanAct());
        Shooter->LastPoseReceived = World->GetTimeSeconds();
        const FVector OldHead = Shooter->Pose.Head;
        Shooter->Pose.Head.X = 600;
        Test->TestFalse(TEXT("Crossing the physical arena boundary disables combat"), Shooter->CanAct());
        Shooter->Pose.Head = OldHead;
        State->Phase = EMRFPSMatchPhase::Finished;
        Test->TestFalse(TEXT("A finished match cannot accept combat"), Shooter->CanAct());
        State->Phase = EMRFPSMatchPhase::Playing;

        Target->ApplyShot(std::numeric_limits<float>::quiet_NaN(), Shooter, false);
        Target->ApplyShot(-20, Shooter, false);
        Target->ApplyShot(20, Target, false);
        Test->TestEqual(TEXT("Malformed or self-inflicted damage cannot change health"), Target->Health, Before);
    }

    FAutomationTestBase* Test = nullptr;
    UWorld* World = nullptr;
    AMRFPSGameState* State = nullptr;
    AMRFPSPawn* Shooter = nullptr;
    AMRFPSPawn* Target = nullptr;
    TWeakObjectPtr<AMRFPSBarrier> FirstWall;
    uint64 LastFrame = MAX_uint64;
    int32 Stage = 0;
    double LastShotAt = 0;
    double WallSpawnedAt = 0;
    double DeathAt = 0;
    bool bCheckedDead = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSAuthorityCombatTest, "MRFPS.Core.AuthorityCombat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSAuthorityCombatTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(MRFPSCoreTests::FAuthorityScenario(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSReconnectOriginTest,"MRFPS.Auth.ReconnectOrigin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSReconnectOriginTest::RunTest(const FString& Parameters)
{
    auto* Legacy=NewObject<UMRFPSGameInstance>();
    Legacy->RememberConnectionAttempt(TEXT("example.test:7777"),TEXT("inspector"));
    Legacy->PrepareForNetworkFailure(true);
    TestTrue(TEXT("An explicitly provisioned token file retains automatic fresh-token reconnect"),Legacy->IsAutoReconnectPending());
    TestFalse(TEXT("File reconnect does not prematurely force a login screen"),Legacy->IsLoginScreenRequested());
    Legacy->RequestLoginScreen(); Legacy->PrepareForNetworkFailure(true);
    TestTrue(TEXT("An explicit request to change server still opens login"),Legacy->IsLoginScreenRequested());
    TestFalse(TEXT("An explicit login request cancels file reconnect"),Legacy->IsAutoReconnectPending());

    auto* Device=NewObject<UMRFPSGameInstance>();
    Device->MarkNextConnectionAsDeviceLogin();
    Device->RememberConnectionAttempt(TEXT("example.test:7777"),TEXT("player"));
    Device->PrepareForNetworkFailure(true);
    TestTrue(TEXT("Device-code login requests a fresh code even when an old file path exists"),Device->IsLoginScreenRequested());
    TestFalse(TEXT("Device-code sessions never replay a file credential automatically"),Device->IsAutoReconnectPending());

    auto* NoFile=NewObject<UMRFPSGameInstance>();
    NoFile->RememberConnectionAttempt(TEXT("example.test:7777"),TEXT("player"));
    NoFile->PrepareForNetworkFailure(false);
    TestTrue(TEXT("Without an explicit token file, login is requested"),NoFile->IsLoginScreenRequested());
    TestFalse(TEXT("No missing file is polled as a refresh source"),NoFile->IsAutoReconnectPending());

    auto* Cancelled=NewObject<UMRFPSGameInstance>();
    Cancelled->MarkNextConnectionAsDeviceLogin(); Cancelled->CancelNextDeviceConnection();
    Cancelled->RememberConnectionAttempt(TEXT("example.test:7777"),TEXT("player"));
    Cancelled->PrepareForNetworkFailure(true);
    TestTrue(TEXT("A cancelled device login does not misclassify a later file connection"),Cancelled->IsAutoReconnectPending());
    auto* NoAttempt=NewObject<UMRFPSGameInstance>(); NoAttempt->PrepareForNetworkFailure(true);
    TestTrue(TEXT("A file without a prior destination still requests login"),NoAttempt->IsLoginScreenRequested());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSConnectionSettingsTest,"MRFPS.Auth.ConnectionSettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSConnectionSettingsTest::RunTest(const FString& Parameters)
{
    using MRFPSConnectionSettings::InitialServerAddress;
    const FString Filename=TEXT("MRFPSConnectionSettingsAutomation.ini");
    const FString Default=TEXT("mrfps.mizuame.app:7777");
    TestEqual(TEXT("Missing config falls back to the public endpoint"),InitialServerAddress(nullptr,Filename),Default);

    FConfigCacheIni Config(EConfigCacheType::Temporary);
    Config.DisableFileOperations();
    Config.Add(Filename,FConfigFile());
    TestEqual(TEXT("Missing saved value uses the same endpoint"),InitialServerAddress(&Config,Filename),Default);
    Config.SetString(TEXT("MRFPS.Connection"),TEXT("Server"),TEXT("custom.test:7778"),Filename);
    TestEqual(TEXT("A saved custom endpoint takes precedence"),InitialServerAddress(&Config,Filename),FString(TEXT("custom.test:7778")));
    Config.SetString(TEXT("MRFPS.Connection"),TEXT("Server"),TEXT(""),Filename);
    TestEqual(TEXT("An explicitly saved empty value is preserved"),InitialServerAddress(&Config,Filename),FString());
    const FString LongAddress=FString::ChrN(300,TCHAR('x'));
    Config.SetString(TEXT("MRFPS.Connection"),TEXT("Server"),*LongAddress,Filename);
    TestEqual(TEXT("Saved addresses remain limited to 255 characters"),InitialServerAddress(&Config,Filename),LongAddress.Left(255));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSDeviceAuthTestUrlTest,"MRFPS.Auth.DeviceAuthTestUrl",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSDeviceAuthTestUrlTest::RunTest(const FString& Parameters)
{
    using MRFPSDeviceAuthUrl::IsSafeTestUrl;
    TestTrue(TEXT("Explicit numeric loopback and valid port are accepted"),IsSafeTestUrl(TEXT("http://127.0.0.1:18787/api/device/exchange")));
    TestTrue(TEXT("Localhost with no path is accepted"),IsSafeTestUrl(TEXT("http://localhost:1")));
    TestFalse(TEXT("Userinfo cannot redirect the request to another host"),IsSafeTestUrl(TEXT("http://127.0.0.1:1234@outside.test/api")));
    TestFalse(TEXT("A hostname suffix is not loopback"),IsSafeTestUrl(TEXT("http://localhost.evil.test:1234/api")));
    TestFalse(TEXT("Encoded authority cannot redirect the request"),IsSafeTestUrl(TEXT("http://localhost:1234%40outside.test/api")));
    TestFalse(TEXT("A port is required and bounded"),IsSafeTestUrl(TEXT("http://127.0.0.1:0/api")) || IsSafeTestUrl(TEXT("http://localhost:65536/api")) || IsSafeTestUrl(TEXT("http://localhost:abc/api")));
    TestFalse(TEXT("Queries, fragments and backslashes are forbidden"),IsSafeTestUrl(TEXT("http://localhost:99/a?x=y")) || IsSafeTestUrl(TEXT("http://localhost:99/a#x")) || IsSafeTestUrl(TEXT("http://localhost:99/a\\b")));
    return true;
}

#endif
