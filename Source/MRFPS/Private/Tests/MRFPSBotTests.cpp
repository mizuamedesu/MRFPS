#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "MRFPSBot.h"
#include "MRFPSGame.h"
#include "MRFPSGeometryWorld.h"
#include "MRFPSHapticsComponent.h"
#include "BrainComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"

// Tick this isolated authority world on successive real Editor frames. UE's
// tick manager intentionally suppresses repeated actor ticks within one frame.
class FMRFPSSoloBotScenario final : public IAutomationLatentCommand
{
public:
    explicit FMRFPSSoloBotScenario(FAutomationTestBase* InTest):Test(InTest) {}
    ~FMRFPSSoloBotScenario() override
    {
        if (World) { World->EndPlay(EEndPlayReason::EndPlayInEditor); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    }
    bool Update() override
    {
        if (LastFrame==GFrameCounter) return false;
        LastFrame=GFrameCounter;
        if (!World) return !Create();
        if (!IsValid(Human) || !IsValid(Mode) || !IsValid(GS)) { Test->AddError(TEXT("Bot fixture world became invalid")); return true; }
        Human->LastPoseReceived=World->GetTimeSeconds();
        World->Tick(LEVELTICK_All,.05f);
        const double Now=World->GetTimeSeconds();
        if (Now>35) { Test->AddError(TEXT("Bot scenario exceeded its simulated deadline")); return true; }
        if (Stage==0) return AwaitCombat(Now);
        if (Stage==1) return AwaitRespawn(Now);
        if (Stage==2) return AwaitTrackingRecovery(Now);
        if (Stage==3) return BeginGeometry(Now);
        if (Stage==4) return FinishGeometry(Now);
        if (Stage==5) return AwaitHumanRespawn(Now);
        return true;
    }
private:
    bool Create()
    {
        if (!GEngine) return false;
        const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
            .CreateAISystem(true).ShouldSimulatePhysics(false).EnableTraceCollision(true).CreateFXSystem(false).SetTransactional(false);
        World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
        if (!Test->TestNotNull(TEXT("Bot fixture world"),World)) return false;
        FWorldContext& Context=GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
        auto* Instance=NewObject<UGameInstance>(GEngine); Context.OwningGameInstance=Instance; World->SetGameInstance(Instance);
        FURL URL; URL.AddOption(TEXT("game=/Script/MRFPS.MRFPSGameMode"));
        const bool HasMode=World->SetGameMode(URL); Mode=World->GetAuthGameMode<AMRFPSGameMode>();
        World->InitializeActorsForPlay(URL); World->BeginPlay(); World->GetWorldSettings()->NotifyBeginPlay();
        GS=World->GetGameState<AMRFPSGameState>();
        if (!Test->TestTrue(TEXT("Fixture uses the real game mode and state"),HasMode && Mode && GS)) return false;
        for (TActorIterator<AMRFPSBarrier> It(World);It;++It) if (It->bDefaultStageObject) It->Destroy();
        const FTransform Identity;
        Human=World->SpawnActorDeferred<AMRFPSPawn>(AMRFPSPawn::StaticClass(),Identity,nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!Test->TestNotNull(TEXT("Human fixture pawn"),Human)) return false;
        Human->Haptics->bAutoConnectTracking=false; Human->FinishSpawning(Identity);
        HumanState=World->SpawnActor<AMRFPSPlayerState>();
        if (!Test->TestNotNull(TEXT("Human fixture state"),HumanState)) return false;
        HumanState->Slot=0; HumanState->bAuthenticated=true; HumanState->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()+3600;
        Human->SetPlayerState(HumanState); Human->bCalibrated=true;
        Human->Pose.Head=FVector(0,-300,170); Human->Pose.Left=FVector(-20,-270,130); Human->Pose.Right=FVector(20,-260,140);
        Human->Pose.HeadRotation=Human->Pose.LeftRotation=Human->Pose.RightRotation=FRotator(0,90,0);
        Human->Pose.bHeadTracked=Human->Pose.bLeftTracked=Human->Pose.bRightTracked=true; Human->Pose.bDesktop=true;
        Human->LastPoseReceived=World->GetTimeSeconds(); Human->OnRepPose(); Mode->bRequireAdminStart=true;
        return true;
    }
    bool AwaitCombat(double Now)
    {
        for (TActorIterator<AMRFPSBotController> It(World);It;++It) Bot=*It;
        Fighter=Bot?Bot->GetPawn<AMRFPSPawn>():nullptr;
        if (Fighter && Fighter->Pose.bHeadTracked && !HaveFirstHead) { FirstHead=Fighter->Pose.Head; HaveFirstHead=true; }
        const bool Ready=Bot && Fighter && MRFPSBot::IsPrepared(Mode) && GS->Phase==EMRFPSMatchPhase::Playing &&
            Fighter->AcceptedShots>0 && HaveFirstHead && FVector::Dist2D(FirstHead,Fighter->Pose.Head)>10;
        if (!Ready && Now<10) return false;
        if (!Test->TestTrue(TEXT("Runtime behavior tree and EQS produce movement and shots without a navmesh or scan"),Ready)) {
            Test->AddError(FString::Printf(TEXT("Bot diagnostic bot=%d fighter=%d solo=%d phase=%d brain=%d prepared=%d fresh=%d time=%.2f"),
                IsValid(Bot),IsValid(Fighter),GS->bSoloBotSession,int32(GS->Phase),Bot && Bot->GetBrainComponent() && Bot->GetBrainComponent()->IsRunning(),
                MRFPSBot::IsPrepared(Mode),Fighter && Fighter->IsPoseFresh(),Now)); return true;
        }
        auto* BotState=Bot->GetPlayerState<AMRFPSPlayerState>();
        Test->TestTrue(TEXT("A single authenticated fixture human starts despite required admin start"),GS->Phase==EMRFPSMatchPhase::Playing);
        Test->TestTrue(TEXT("Bot is server-owned and never an authenticated network client"),BotState && BotState->IsABot() && !BotState->bAuthenticated && MRFPSBot::IsServerBot(Fighter));
        HumanState->SetIsABot(true); HumanState->bAuthenticated=false;
        Test->TestFalse(TEXT("A bot flag without the server AI controller cannot bypass authentication"),Human->CanAct());
        HumanState->SetIsABot(false); HumanState->bAuthenticated=true;
        Test->TestTrue(TEXT("No measurement is not reported as received"),!GS->bSoloGeometryReady);
        Test->TestTrue(TEXT("A bot does not consume the second human's signed admission slot"),Mode->RoleCapacityError(TEXT("player")).IsEmpty());
        Test->TestTrue(TEXT("Solo never impersonates two-device shared floor registration"),GS->bSoloBotSession && !GS->bSharedFloorConfirmed && !GS->Layout.bCalibrated);
        const auto Layout=GS->Layout;
        Test->TestTrue(TEXT("Solo keeps fixed dimensions and the one-player anchor"),Layout.WidthCm==450 && Layout.LengthCm==700 &&
            Layout.ContainsHead(Human->Pose.Head) && FVector::Dist2D(Layout.Center,Human->Pose.Head)>349);
        Human->Health=100; Human->LastPoseReceived=Now;
        const int32 BeforeKills=HumanState->Kills;
        for (int32 I=0;I<4;++I) Fighter->ApplyShot(25,Human,false);
        if (!Test->TestTrue(TEXT("Bot death ends the round and scores for the human"),Fighter->Health<=0 && HumanState->Kills==BeforeKills+1 && BotState && BotState->Deaths>0 &&
            GS->Phase==EMRFPSMatchPhase::BetweenRounds && GS->RoundWinsSlot0==1 && GS->CompletedRounds==1)) return true;
        StageStarted=Now; Stage=1; return false;
    }
    bool AwaitRespawn(double Now)
    {
        if (!IsValid(Bot) || !IsValid(Fighter)) { Test->AddError(TEXT("Bot disappeared during respawn")); return true; }
        if (Fighter->Health<=0 && Now-StageStarted<6) return false;
        if (!Test->TestTrue(TEXT("Bot respawns for the second round with the first win retained"),Fighter->Health==100.f &&
            GS->Phase==EMRFPSMatchPhase::Playing && GS->RoundNumber==2 && GS->RoundWinsSlot0==1)) return true;
        Human->Health=100; Human->LastPoseReceived=Fighter->LastPoseReceived=Now;
        for (int32 I=0;I<4;++I) Human->ApplyShot(25,Fighter,false);
        if (!Test->TestTrue(TEXT("A Bot kill ends the second round and scores for the Bot"),Human->Health<=0 &&
            GS->Phase==EMRFPSMatchPhase::BetweenRounds && GS->RoundWinsSlot1==1 && GS->CompletedRounds==2)) return true;
        StageStarted=Now; Stage=5; return false;
    }
    bool AwaitHumanRespawn(double Now)
    {
        if (!IsValid(Bot) || !IsValid(Fighter)) { Test->AddError(TEXT("Bot disappeared before the third round")); return true; }
        if (Human->Health<=0 && Now-StageStarted<6) return false;
        // The behavior tree can fire on the same tick the countdown ends.
        if (!Test->TestTrue(TEXT("The player respawns for the third round with both scores retained"),Human->Health>0.f &&
            GS->Phase==EMRFPSMatchPhase::Playing && GS->RoundNumber==3 && GS->RoundWinsSlot0==1 && GS->RoundWinsSlot1==1)) return true;
        Human->Pose.bHeadTracked=false; Mode->Tick(0);
        Test->TestTrue(TEXT("Lost tracking pauses without destroying the bot or clearing the round"),GS->bCombatPaused && GS->bSoloBotSession && IsValid(Bot));
        Human->Pose.bHeadTracked=true; Human->LastPoseReceived=Now;
        StageStarted=Now; Stage=2; return false;
    }
    bool AwaitTrackingRecovery(double Now)
    {
        if (GS->bCombatPaused && Now-StageStarted<2) return false;
        if (!Test->TestTrue(TEXT("Fresh tracking resumes the same solo session"),!GS->bCombatPaused && GS->bSoloBotSession && IsValid(Bot))) return true;
        auto* Admin=World->SpawnActor<AMRFPSPlayerController>();
        auto* AdminState=World->SpawnActor<AMRFPSPlayerState>(); Admin->SetPlayerState(AdminState);
        AdminState->Slot=-1; AdminState->bAuthenticated=true; AdminState->bInspector=AdminState->bAdmin=true;
        AdminState->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()+3600;
        FString Error;
        Test->TestTrue(TEXT("Admin can reset the solo match"),Mode->HandleAdminAction(Admin,EMRFPSAdminAction::Reset,FVector2D::ZeroVector,Error));
        Bot->bAutoStartPending=true;
        Test->TestTrue(TEXT("Admin can start before the auto-start tick"),Mode->HandleAdminAction(Admin,EMRFPSAdminAction::Start,FVector2D::ZeroVector,Error));
        Test->TestFalse(TEXT("Explicit start cancels pending auto-start synchronously"),Bot->bAutoStartPending);
        Test->TestTrue(TEXT("Admin reset remains explicit"),Mode->HandleAdminAction(Admin,EMRFPSAdminAction::Reset,FVector2D::ZeroVector,Error));
        Mode->Tick(0);
        Test->TestTrue(TEXT("No delayed auto-start can undo admin reset"),GS->Phase==EMRFPSMatchPhase::Waiting);
        Test->TestTrue(TEXT("Admin can start the same prepared bot again"),Mode->HandleAdminAction(Admin,EMRFPSAdminAction::Start,FVector2D::ZeroVector,Error));
        GS->RestartAt=Now; Mode->Tick(0);
        Human->Pose.bHeadTracked=false; Mode->Tick(0);
        Test->TestTrue(TEXT("Tracking loss creates an automatic pause"),MRFPSBot::IsTrackingPause(Mode));
        Test->TestTrue(TEXT("GUI admin pause takes ownership of automatic pause"),Mode->HandleAdminAction(Admin,EMRFPSAdminAction::Pause,FVector2D::ZeroVector,Error));
        Human->Pose.bHeadTracked=true; Human->LastPoseReceived=Now; Mode->Tick(0);
        Test->TestTrue(TEXT("GUI admin pause remains after tracking returns"),GS->bCombatPaused && !MRFPSBot::IsTrackingPause(Mode));
        GS->RoundWinsSlot0=1;
        const double Deadline=GS->RoundEndsAt,PausedAt=GS->RoundPausedAt;
        const uint32 Calibration=GS->SoloHumanCalibration;
        if (!Test->TestTrue(TEXT("Paused solo layout resizes around the human"),
            Mode->HandleAdminAction(Admin,EMRFPSAdminAction::SetDimensions,FVector2D(450,500),Error))) {
            Test->AddError(FString::Printf(TEXT("Solo resize returned %s"),*Error)); return true;
        }
        Test->TestTrue(TEXT("Resize preserves pause, round, deadline and calibration"),GS->bCombatPaused &&
            GS->Phase==EMRFPSMatchPhase::Playing && GS->RoundWinsSlot0==1 && GS->RoundEndsAt==Deadline &&
            GS->RoundPausedAt==PausedAt && GS->SoloHumanCalibration==Calibration);
        Test->TestTrue(TEXT("Resized bot and human remain ready inside fixed dimensions"),GS->Layout.WidthCm==450 &&
            GS->Layout.LengthCm==500 && GS->Layout.ContainsHead(Human->Pose.Head) && Fighter->IsInsideArena() && MRFPSBot::IsPrepared(Mode));
        const auto Current=GS->Layout;
        TArray<AMRFPSBarrier*> Blockers;
        for (double Y:{160.,80.,0.}) {
            const FVector Local(0,Y,100);
            const FTransform Transform(FRotator(0,Current.YawDegrees+90,0),Current.ToWorld(Local));
            auto* Blocker=World->SpawnActorDeferred<AMRFPSBarrier>(AMRFPSBarrier::StaticClass(),Transform,nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
            if (!Test->TestNotNull(TEXT("Future layout blocker"),Blocker)) return true;
            Blocker->bAdminPlaced=true; Blocker->bMapAnchor=true; Blocker->MapLocalPosition=Local; Blocker->MapLocalYaw=90;
            Blocker->FinishSpawning(Transform); Blocker->ApplyBuildKind(); Blockers.Add(Blocker);
        }
        const uint32 ControlRevision=GS->ControlRevision;
        const FVector BotHead=Fighter->Pose.Head;
        Test->TestFalse(TEXT("Resize is rejected when every future bot location is obstructed"),
            Mode->HandleAdminAction(Admin,EMRFPSAdminAction::SetDimensions,FVector2D(450,400),Error));
        Test->TestTrue(TEXT("Rejected resize changes neither match, revisions nor bot pose"),Error==TEXT("BOT_RESIZE_BLOCKED") &&
            GS->Layout.Revision==Current.Revision && GS->Layout.Center.Equals(Current.Center) && GS->Layout.LengthCm==Current.LengthCm &&
            GS->ControlRevision==ControlRevision && GS->bCombatPaused && GS->Phase==EMRFPSMatchPhase::Playing &&
            GS->RoundWinsSlot0==1 && GS->RoundEndsAt==Deadline && Fighter->Pose.Head.Equals(BotHead));
        for (auto* Blocker:Blockers) { Blocker->SetActorEnableCollision(false); Blocker->Destroy(); }
        Test->TestTrue(TEXT("Explicit admin resume releases that pause"),Mode->HandleAdminAction(Admin,EMRFPSAdminAction::Resume,FVector2D::ZeroVector,Error));
        Human->Pose.bHeadTracked=false; Mode->Tick(0);
        const FGuid Operation=FGuid::NewGuid();
        Admin->ServerAutomationCommand(Operation,TEXT("admin.action"),TEXT("{\"action\":\"pause\"}"),nullptr,GS->Layout.Revision,GS->ControlRevision);
        const auto* Receipt=Mode->AutomationReceipts.Find(Operation);
        Test->TestTrue(TEXT("Automation pause reaches authority instead of same-state shortcut"),Receipt && Receipt->Value==TEXT("AUTOMATION_OK"));
        Human->Pose.bHeadTracked=true; Human->LastPoseReceived=Now; Mode->Tick(0);
        Test->TestTrue(TEXT("Automation pause remains after tracking returns"),GS->bCombatPaused && !MRFPSBot::IsTrackingPause(Mode));
        Test->TestTrue(TEXT("Explicit resume releases automation pause"),Mode->HandleAdminAction(Admin,EMRFPSAdminAction::Resume,FVector2D::ZeroVector,Error));
        if (Bot->GetBrainComponent()) Bot->GetBrainComponent()->StopLogic(TEXT("fixture"));
        StageStarted=Now; Stage=3; return false;
    }
    bool BeginGeometry(double Now)
    {
        if (Now-StageStarted<.2) return false;
        if (!IsValid(Fighter)) { Test->AddError(TEXT("Bot pawn missing before geometry test")); return true; }
        GS->Phase=EMRFPSMatchPhase::Playing; GS->bCombatPaused=false; GS->RoundEndsAt=0;
        Human->Health=100; Human->Pose.bDesktop=false; Human->bStageFloorReferenced=true; Human->GeometryCalibrationGeneration=1;
        GS->SoloHumanCalibration=1;
        const auto Layout=GS->Layout;
        Human->Pose.Head=Layout.ToWorld(FVector(0,-150,170)); Human->Pose.Left=Human->Pose.Right=Layout.ToWorld(FVector(0,-130,130));
        Human->LastPoseReceived=Now; Human->OnRepPose();
        Fighter->Pose.Head=Layout.ToWorld(FVector(0,150,170)); Fighter->Pose.Right=Layout.ToWorld(FVector(0,125,130)); Fighter->Pose.Left=Fighter->Pose.Right;
        Fighter->Pose.RightRotation=(FVector(Human->Pose.Head)-FVector(0,0,58)-FVector(Fighter->Pose.Right)).Rotation();
        Fighter->Pose.HeadRotation=Fighter->Pose.RightRotation; Fighter->Pose.LeftRotation=Fighter->Pose.RightRotation;
        Fighter->Pose.bHeadTracked=Fighter->Pose.bLeftTracked=Fighter->Pose.bRightTracked=true;
        Fighter->LastPoseReceived=Now; Fighter->OnRepPose();
        Human->Pose.RightRotation=(FVector(Fighter->Pose.Head)-FVector(0,0,58)-FVector(Human->Pose.Right)).Rotation();
        Human->Pose.RightHandRotation=Human->Pose.RightRotation;
        const FVector Start=Layout.ToWorld(FVector(0,100,0)),End=Layout.ToWorld(FVector(0,-100,0));
        Test->TestTrue(TEXT("No scan permits unobstructed virtual movement"),MRFPSBot::CanMove(World,Fighter,Start,End));
        auto* Wall=World->SpawnActor<AMRFPSBarrier>(Layout.ToWorld(FVector(0,0,100)),FRotator(0,Layout.YawDegrees+90,0));
        if (!Test->TestNotNull(TEXT("Virtual wall fixture"),Wall)) return true;
        Test->TestFalse(TEXT("Actual virtual wall blocks movement"),MRFPSBot::CanMove(World,Fighter,Start,End));
        Wall->SetActorEnableCollision(false);
        Wall->Destroy();
        Test->TestTrue(TEXT("Removing virtual wall opens path before scan is committed"),MRFPSBot::CanMove(World,Fighter,Start,End));
        Geometry=World->GetSubsystem<UMRFPSGeometryWorld>();
        FMRFPSGeometryMesh Mesh;
        Mesh.Vertices={Layout.ToWorld(FVector(-40,0,60)),Layout.ToWorld(FVector(40,0,60)),
            Layout.ToWorld(FVector(40,0,160)),Layout.ToWorld(FVector(-40,0,160))};
        Mesh.Indices={0,1,2,0,2,3};
        FMRFPSGeometryChunk Header; Header.Slot=0; Header.StreamId=Header.Calibration=Header.Sequence=1;
        Header.LayoutRevision=Layout.Revision; Header.CaptureMs=uint32(GS->GetServerWorldTimeSeconds()*1000);
        TArray<uint8> Bytes; HumanState->GeometryStreamId=1;
        if (!Test->TestTrue(TEXT("Measured solo wall encodes"),Mesh.Encode(Header,Bytes)) ||
            !Test->TestNotNull(TEXT("Geometry subsystem"),Geometry) ||
            !Test->TestTrue(TEXT("Solo Stage measurement is accepted"),Geometry->Commit(Human,Header,MoveTemp(Mesh)))) return true;
        Mode->Tick(0);
        Test->TestTrue(TEXT("Received measurement is visible in status"),GS->bSoloGeometryReady);
        const FVector MeasuredCenter=Layout.ToWorld(FVector(0,0,100));
        Test->TestTrue(TEXT("Conservative bot placement detects the real measurement"),
            Geometry->OverlapsMeasuredBounds(FBox(MeasuredCenter-FVector(10),MeasuredCenter+FVector(10))));
        const FVector ClearCenter=Layout.ToWorld(FVector(150,0,100));
        Test->TestFalse(TEXT("Measured placement check leaves distant space available"),
            Geometry->OverlapsMeasuredBounds(FBox(ClearCenter-FVector(10),ClearCenter+FVector(10))));
        Test->TestFalse(TEXT("Measured wall blocks the same movement segment"),MRFPSBot::CanMove(World,Fighter,Start,End));
        BeforeBlockedShot=Fighter->AcceptedShots; Fighter->ServerFire();
        Test->TestEqual(TEXT("Blocked shot was actually fired"),Fighter->AcceptedShots,BeforeBlockedShot+1);
        Test->TestEqual(TEXT("Measured wall blocks the real server bullet"),Human->Health,100.f);
        BeforeHumanBlockedShot=Human->AcceptedShots; BeforeHumanBlockedHealth=Fighter->Health;
        Human->ServerFire();
        Test->TestEqual(TEXT("Blocked human shot was actually fired"),Human->AcceptedShots,BeforeHumanBlockedShot+1);
        Test->TestEqual(TEXT("Measured wall blocks the human's server bullet against the bot"),Fighter->Health,BeforeHumanBlockedHealth);
        Geometry->RemoveOwner(Human); StageStarted=Now; Stage=4; return false;
    }
    bool FinishGeometry(double Now)
    {
        if (Now-StageStarted<.2) return false;
        Human->LastPoseReceived=Fighter->LastPoseReceived=Now; Human->OnRepPose(); Fighter->OnRepPose();
        Fighter->ServerFire();
        Test->TestEqual(TEXT("Unblocked shot was actually fired"),Fighter->AcceptedShots,BeforeBlockedShot+2);
        Test->TestTrue(TEXT("Removing measured wall permits the same authoritative hit"),Human->Health<100.f);
        Human->ServerFire();
        Test->TestEqual(TEXT("Unblocked human shot was actually fired"),Human->AcceptedShots,BeforeHumanBlockedShot+2);
        Test->TestTrue(TEXT("Removing measured wall permits the human's authoritative hit on the bot"),Fighter->Health<BeforeHumanBlockedHealth);
        GS->RoundWinsSlot0=1; GS->RoundWinsSlot1=1; MRFPSBot::BeforeHumanAdmission(Mode);
        Test->TestTrue(TEXT("Second human clears bot and solo score before using slot"),!GS->bSoloBotSession &&
            GS->Phase==EMRFPSMatchPhase::Waiting && GS->RoundWinsSlot0==0 && GS->RoundWinsSlot1==0 && !GS->bSharedFloorConfirmed);
        return true;
    }
    FAutomationTestBase* Test=nullptr;
    UWorld* World=nullptr;
    AMRFPSGameMode* Mode=nullptr;
    AMRFPSGameState* GS=nullptr;
    AMRFPSPawn* Human=nullptr;
    AMRFPSPlayerState* HumanState=nullptr;
    AMRFPSBotController* Bot=nullptr;
    AMRFPSPawn* Fighter=nullptr;
    UMRFPSGeometryWorld* Geometry=nullptr;
    FVector FirstHead=FVector::ZeroVector;
    bool HaveFirstHead=false;
    int32 Stage=0,BeforeBlockedShot=0,BeforeHumanBlockedShot=0;
    float BeforeHumanBlockedHealth=0;
    double StageStarted=0;
    uint64 LastFrame=MAX_uint64;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSSoloBotTest,"MRFPS.Bot.ServerCombatAndGeometry",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSSoloBotTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FMRFPSSoloBotScenario(this));
    return true;
}
#endif
