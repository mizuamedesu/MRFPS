#include "MRFPSGame.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "MRFPSHapticsComponent.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSMagazineGestureTest,"MRFPS.Weapon.MagazineGesture",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSMagazineGestureTest::RunTest(const FString& Parameters)
{
    FMRFPSReloadGesture Gesture;
    TestFalse(TEXT("First sample establishes the stroke"),Gesture.Update(FVector(0,0,-26),1,true));
    TestFalse(TEXT("A short motion does not reload"),Gesture.Update(FVector(0,0,-20),1.05,true));
    TestTrue(TEXT("A deliberate stroke along the magazine reloads"),Gesture.Update(FVector(0,0,-10),1.15,true));
    TestFalse(TEXT("Holding on the magazine cannot reload again"),Gesture.Update(FVector(0,0,-25),1.3,true));
    TestFalse(TEXT("Leaving the magazine resets the latch"),Gesture.Update(FVector(35,0,-25),1.4,true));
    TestFalse(TEXT("An ineligible or lost hand clears the stroke"),Gesture.Update(FVector(0,0,-25),1.5,false));
    TestFalse(TEXT("Reacquiring the hand starts a new stroke"),Gesture.Update(FVector(0,0,-10),1.6,true));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSWeaponAuthorityTest,"MRFPS.Weapon.EquippedFireReload",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSWeaponAuthorityTest::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Engine"),GEngine)) return false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false).EnableTraceCollision(true).CreateFXSystem(false).SetTransactional(false);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("Authority world"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    auto* State=World->SpawnActor<AMRFPSGameState>();World->SetGameState(State);
    State->Layout.bCalibrated=true;State->bSharedFloorConfirmed=true;
    State->SharedFloorRevision=State->Layout.RegistrationRevision=1;
    World->InitializeActorsForPlay(FURL());World->BeginPlay();World->GetWorldSettings()->NotifyBeginPlay();
    State->Phase=EMRFPSMatchPhase::Playing;
    auto* Pawn=World->SpawnActor<AMRFPSPawn>();auto* PS=World->SpawnActor<AMRFPSPlayerState>();
    PS->Slot=0;PS->bAuthenticated=true;PS->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()+3600;
    Pawn->SetPlayerState(PS);Pawn->Haptics->bAutoConnectTracking=false;Pawn->bCalibrated=true;Pawn->bStageFloorReferenced=true;
    FMRFPSPose Pose;Pose.bHeadTracked=Pose.bLeftTracked=Pose.bRightTracked=true;
    Pose.Head=FVector(0,0,170);Pose.Right=FVector(5,0,132);Pose.Left=FVector(5,-30,132);
    const auto Send=[&](float Dt) { World->Tick(LEVELTICK_All,Dt);++Pose.Sequence;Pawn->ServerPose(Pose); };
    Send(.04f);
    TestTrue(TEXT("A gun is equipped as soon as the right hand is tracked"),Pawn->GunVisual->IsVisible());
    const FVector FirstGunPosition=Pawn->GunVisual->GetComponentLocation();
    Pawn->ServerFire();TestEqual(TEXT("A tracked controller can fire without glove finger samples"),Pawn->AcceptedShots,1);
    Pose.Right=FVector(Pose.Right)+FVector(20,0,0);Pose.RightFingers.bValid=true;
    Send(.16f);
    TestTrue(TEXT("The equipped gun follows the right hand without a grip gesture"),
        FVector::Dist(Pawn->GunVisual->GetComponentLocation()-FirstGunPosition,FVector(20,0,0))<1);
    Pawn->ServerFire();TestEqual(TEXT("An accepted shot spends one cartridge"),Pawn->MagazineRounds,28);
    Pawn->ServerFire();TestEqual(TEXT("A rate-limited shot spends no ammunition"),Pawn->MagazineRounds,28);
    Pose.LeftFingers.bValid=true;Pose.Left=FVector(Pose.Right)+FVector(0,0,-25);
    Send(.04f);Pose.Left=FVector(Pose.Right)+FVector(0,0,-9);Send(.12f);
    TestEqual(TEXT("The other tracked hand sliding along the magazine refills it"),Pawn->MagazineRounds,AMRFPSPawn::MagazineCapacity);
    Pawn->MagazineRounds=0;World->Tick(LEVELTICK_All,.16f);Pawn->LastPoseReceived=World->GetTimeSeconds();
    Pawn->ServerFire();TestEqual(TEXT("An empty magazine cannot fire"),Pawn->AcceptedShots,2);
    Pawn->MagazineRounds=5;Pose.RightFingers.bValid=false;Send(.16f);
    TestTrue(TEXT("Loss of glove input does not make the gun leave a tracked hand"),Pawn->GunVisual->IsVisible());
    Pawn->ServerFire();TestEqual(TEXT("Tracked controller fire tolerates a lost glove sample"),Pawn->MagazineRounds,4);
    Pose.bRightTracked=false;Send(.04f);
    TestFalse(TEXT("The gun is hidden while its hand position is unknown"),Pawn->GunVisual->IsVisible());
    Pose.bRightTracked=true;Pose.Right=FVector(Pose.Right)+FVector(10,0,0);Send(.04f);
    TestTrue(TEXT("The gun returns with the reacquired hand"),Pawn->GunVisual->IsVisible());
    Pose.Left=FVector(Pose.Right)+FVector(40,0,-25);Send(.6f);
    Pose.Left=FVector(Pose.Right)+FVector(0,0,-25);Send(.04f);
    Pose.Left=FVector(Pose.Right)+FVector(0,0,-9);Send(.12f);
    TestEqual(TEXT("A left-hand magazine stroke reloads even without a right-hand grip"),Pawn->MagazineRounds,AMRFPSPawn::MagazineCapacity);
    World->EndPlay(EEndPlayReason::EndPlayInEditor);World->DestroyWorld(false);GEngine->DestroyWorldContext(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSPracticeActionsTest,"MRFPS.Weapon.ActionsFromStart",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSPracticeActionsTest::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Engine"),GEngine)) return false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false).EnableTraceCollision(true).CreateFXSystem(false).SetTransactional(false);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("Authority world"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    auto* State=World->SpawnActor<AMRFPSGameState>(); World->SetGameState(State);
    World->InitializeActorsForPlay(FURL()); World->BeginPlay(); World->GetWorldSettings()->NotifyBeginPlay();
    State->Phase=EMRFPSMatchPhase::Waiting; State->Layout.bCalibrated=false; State->bSharedFloorConfirmed=false;
    auto* Pawn=World->SpawnActor<AMRFPSPawn>(); auto* PS=World->SpawnActor<AMRFPSPlayerState>();
    PS->Slot=0; PS->bAuthenticated=true; PS->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()+3600;
    Pawn->SetPlayerState(PS); Pawn->Haptics->bAutoConnectTracking=false; Pawn->bCalibrated=false;
    FMRFPSPose Pose; Pose.bHeadTracked=Pose.bLeftTracked=Pose.bRightTracked=true;
    Pose.Head=FVector(0,0,170); Pose.Right=FVector(30,20,130); Pose.Left=FVector(25,-30,80);
    Pose.LeftOpenness=1;
    const auto Send=[&](float Dt) { World->Tick(LEVELTICK_All,Dt); ++Pose.Sequence; Pawn->ServerPose(Pose); };
    Send(.04f); Pawn->ServerFire();
    TestEqual(TEXT("Firing works before floor calibration and match start"),Pawn->MagazineRounds,29);
    Pawn->Tick(.01f); TestTrue(TEXT("An open hand produces a shield immediately"),Pawn->bShieldActive);
    Pose.Left=FVector(25,-30,100); Send(.1f); Pose.Left=FVector(25,-30,120); Send(.1f);
    TestTrue(TEXT("An upward hand motion deploys a wall before match start"),Pawn->WallReadyAt>World->GetTimeSeconds());
    State->bCombatPaused=true; Pawn->LastPoseReceived=World->GetTimeSeconds();
    Pawn->ServerFire(); TestEqual(TEXT("Admin pause still prevents firing"),Pawn->MagazineRounds,29);
    auto* Other=World->SpawnActor<AMRFPSPawn>(); auto* OtherPS=World->SpawnActor<AMRFPSPlayerState>();
    OtherPS->Slot=1; OtherPS->bAuthenticated=true; OtherPS->AuthExpiresAt=PS->AuthExpiresAt;
    Other->SetPlayerState(OtherPS); Other->Haptics->bAutoConnectTracking=false;
    State->bCombatPaused=false; Other->Pose=Pose; Other->LastPoseReceived=World->GetTimeSeconds(); Pawn->LastPoseReceived=World->GetTimeSeconds();
    Other->ApplyShot(20,Pawn,false); TestEqual(TEXT("Unregistered practice cannot damage another player"),Other->Health,100.f);
    World->EndPlay(EEndPlayReason::EndPlayInEditor); World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    return true;
}
#endif
