#include "MRFPSGame.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSWeaponAuthorityTest,"MRFPS.Weapon.GrabFireReload",
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
    TestTrue(TEXT("A gun spawns ahead of the tracked player"),Pawn->bGunSpawned && !Pawn->bGunHeld && FVector::Dist(Pawn->GunRestLocation,FVector(55,0,132))<1);
    Pawn->ServerFire();TestEqual(TEXT("An unheld gun cannot fire"),Pawn->AcceptedShots,0);
    Pose.Right=Pawn->GunRestLocation;Pose.RightFingers.bValid=true;
    for (int32 I=6;I<15;++I) Pose.RightFingers.Joint[I]=255;
    Send(.04f);TestTrue(TEXT("Closing the tracked right hand on the grip picks up the gun"),Pawn->bGunHeld);
    Pawn->ServerFire();TestEqual(TEXT("An accepted shot spends one cartridge"),Pawn->MagazineRounds,29);
    Pawn->ServerFire();TestEqual(TEXT("A rate-limited shot spends no ammunition"),Pawn->MagazineRounds,29);
    Pose.LeftFingers.bValid=true;Pose.Left=FVector(Pose.Right)+FVector(0,0,-25);
    Send(.04f);Pose.Left=FVector(Pose.Right)+FVector(0,0,-9);Send(.12f);
    TestEqual(TEXT("The other tracked hand sliding along the magazine refills it"),Pawn->MagazineRounds,AMRFPSPawn::MagazineCapacity);
    Pawn->MagazineRounds=0;World->Tick(LEVELTICK_All,.16f);Pawn->LastPoseReceived=World->GetTimeSeconds();
    Pawn->ServerFire();TestEqual(TEXT("An empty magazine cannot fire"),Pawn->AcceptedShots,1);
    Pose.RightFingers.bValid=false;Send(.04f);
    TestFalse(TEXT("Loss of glove input drops the gun"),Pawn->bGunHeld);
    Pawn->ServerFire();TestEqual(TEXT("A dropped gun cannot fire"),Pawn->AcceptedShots,1);
    TestEqual(TEXT("A disconnected hand cannot reload"),Pawn->MagazineRounds,0);
    World->EndPlay(EEndPlayReason::EndPlayInEditor);World->DestroyWorld(false);GEngine->DestroyWorldContext(World);
    return true;
}
#endif
