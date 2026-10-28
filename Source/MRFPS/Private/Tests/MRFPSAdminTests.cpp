#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "MRFPSGame.h"
#include "MRFPSHapticsComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSFixedLayoutTest,"MRFPS.Admin.FixedLayout",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSFixedLayoutTest::RunTest(const FString& Parameters)
{
    FMRFPSArenaLayout Layout; FString Error;
    TestEqual(TEXT("Default X is 4.5 metres"),Layout.WidthCm,450.f);
    TestEqual(TEXT("Default Y is 7 metres"),Layout.LengthCm,700.f);
    TestTrue(TEXT("Configured dimensions do not imply physical calibration"),Layout.IsSane() && !Layout.bCalibrated);
    TestTrue(TEXT("Nominal starts use opposite Y endpoints"),Layout.NominalStart(0).Equals(FVector(0,-350,170)) && Layout.NominalStart(1).Equals(FVector(0,350,170)));
    TestFalse(TEXT("No shared registration means no measured floor"),Layout.CalibrateFromAnchors(FVector(100,200,120),FVector(700,200,230),0,0,Error));
    TestEqual(TEXT("Rejected calibration leaves the layout revision unchanged"),Layout.Revision,uint32(1));
    TestTrue(TEXT("Registered planar anchors explicitly calibrate a floor"),Layout.CalibrateFromAnchors(FVector(100,200,120),FVector(700,200,230),25,7,Error));
    TestTrue(TEXT("Planar midpoint is used without averaging head heights"),Layout.Center.Equals(FVector(400,200,25)));
    TestTrue(TEXT("Player0 to player1 defines positive floor Y"),(Layout.ToWorld(FVector(0,1,0))-Layout.Center).Equals(FVector(1,0,0),1.e-6));
    TestEqual(TEXT("Six metre measured separation cannot fit or resize the seven metre floor"),Layout.LengthCm,700.f);
    TestEqual(TEXT("Width is also unchanged by measured anchors"),Layout.WidthCm,450.f);
    TestTrue(TEXT("Both measured anchors remain where supplied"),Layout.ContainsHead(FVector(100,200,120)) && Layout.ContainsHead(FVector(700,200,230)));
    TestTrue(TEXT("Oriented rectangular X boundary is inclusive"),Layout.ContainsPlanar(Layout.ToWorld(FVector(225,0,0))));
    TestFalse(TEXT("Rotated width, not a world aligned square, bounds membership"),Layout.ContainsPlanar(Layout.ToWorld(FVector(225.1,0,0))));
    TestTrue(TEXT("Actual geometry uses registered floor-relative height"),Layout.ContainsGeometry(Layout.ToWorld(FVector(0,0,300))));
    TestFalse(TEXT("Geometry above the configured vertical range is rejected"),Layout.ContainsGeometry(Layout.ToWorld(FVector(0,0,301))));
    const FVector Center=Layout.Center; const float Yaw=Layout.YawDegrees; const uint32 Revision=Layout.Revision;
    TestFalse(TEXT("Degenerate coincident anchors cannot mutate layout"),Layout.CalibrateFromAnchors(FVector(0,0,170),FVector(0,0,180),0,7,Error));
    TestTrue(TEXT("Failed calibration retains last fixed transform"),Layout.Center==Center && Layout.YawDegrees==Yaw && Layout.Revision==Revision);
    TestFalse(TEXT("Nonfinite operator dimensions are rejected"),Layout.SetDimensions(std::numeric_limits<double>::infinity(),700,Error));
    TestTrue(TEXT("Only an explicit size command changes dimensions"),Layout.SetDimensions(600,900,Error));
    TestTrue(TEXT("Size changes preserve centre and heading"),Layout.Center==Center && Layout.YawDegrees==Yaw && Layout.WidthCm==600 && Layout.LengthCm==900);
    Layout.Revision=MAX_uint32;
    TestFalse(TEXT("Layout generation cannot wrap into an old shared world"),Layout.SetDimensions(450,700,Error));
    return true;
}

namespace
{
struct FAdminWorld
{
    UWorld* World=nullptr;
    AMRFPSGameState* State=nullptr;
    AMRFPSGameMode* Mode=nullptr;
    AMRFPSPlayerController* Admin=nullptr;
    AMRFPSPlayerController* Observer=nullptr;
    AMRFPSPawn* Players[2]={nullptr,nullptr};
    bool Create()
    {
        if (!GEngine) return false;
        const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false).EnableTraceCollision(true).CreateFXSystem(false).SetTransactional(false);
        World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
        if (!World) return false;
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        State=World->SpawnActor<AMRFPSGameState>(); World->SetGameState(State);
        World->InitializeActorsForPlay(FURL()); World->BeginPlay(); World->GetWorldSettings()->NotifyBeginPlay();
        // Fixture states below bypass cryptography only in this isolated test.
        // The separate Auth tests exercise real Ed25519 admin admission.
        Mode=World->SpawnActor<AMRFPSGameMode>(); Mode->GameState=State;
        // GameMode PreInitializeComponents creates its own GameState. Keep the
        // fixture's authority and pawns on the same state, as a real world does.
        World->SetGameState(State);
        Admin=Controller(true); Observer=Controller(false);
        for (int32 Slot=0;Slot<2;++Slot) {
            Players[Slot]=World->SpawnActor<AMRFPSPawn>(); auto* PS=World->SpawnActor<AMRFPSPlayerState>();
            PS->Slot=Slot; PS->bAuthenticated=true; PS->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()+3600;
            Players[Slot]->SetPlayerState(PS); Players[Slot]->Haptics->bAutoConnectTracking=false;
            Players[Slot]->bCalibrated=true; Players[Slot]->bStageFloorReferenced=true; Players[Slot]->GeometryCalibrationGeneration=1;
            Players[Slot]->Pose.bHeadTracked=Players[Slot]->Pose.bLeftTracked=Players[Slot]->Pose.bRightTracked=true;
            Players[Slot]->Pose.bDesktop=true; Move(Slot,FVector(0,Slot?-300:300,170));
        }
        return Mode && Admin && Observer && Players[0] && Players[1];
    }
    AMRFPSPlayerController* Controller(bool IsAdmin)
    {
        auto* PC=World->SpawnActor<AMRFPSPlayerController>(); auto* PS=World->SpawnActor<AMRFPSPlayerState>();
        PC->SetPlayerState(PS); PS->Slot=-1; PS->bInspector=true; PS->bAdmin=IsAdmin;
        PS->bAuthenticated=true; PS->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()+3600; return PC;
    }
    void Move(int32 Slot,const FVector& Head)
    {
        auto* Fighter=Players[Slot]; Fighter->Pose.Head=Head;
        Fighter->Pose.Left=Head+FVector(20,-20,-40); Fighter->Pose.Right=Head+FVector(30,20,-40);
        Fighter->LastPoseReceived=World->GetTimeSeconds(); Fighter->OnRepPose();
    }
    int32 Walls() const { int32 Count=0; for (TActorIterator<AMRFPSBarrier> It(World);It;++It) ++Count; return Count; }
    ~FAdminWorld()
    {
        if (World) { World->EndPlay(EEndPlayReason::EndPlayInEditor); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSAdminAuthorityTest,"MRFPS.Admin.AuthorityAndFixedCalibration",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSAdminAuthorityTest::RunTest(const FString& Parameters)
{
    FAdminWorld F; if (!TestTrue(TEXT("Real authority fixture starts without sockets or hardware output"),F.Create())) return false;
    FString Error;
    TestFalse(TEXT("A signed ordinary observer state cannot start a match"),F.Mode->HandleAdminAction(F.Observer,EMRFPSAdminAction::Start,FVector2D::ZeroVector,Error));
    TestEqual(TEXT("Observer rejection is authoritative"),Error,FString(TEXT("ADMIN_REQUIRED")));
    TestFalse(TEXT("Observer cannot place a wall via the authority helper"),F.Mode->PlaceAdminWall(F.Observer,FVector(0,0,500),FVector(0,0,-1),0,Error));
    TestEqual(TEXT("Rejected mutations spawn nothing"),F.Walls(),0);
    TestTrue(TEXT("Authenticated admin explicitly starts a ready desktop session"),F.Mode->HandleAdminAction(F.Admin,EMRFPSAdminAction::Start,FVector2D::ZeroVector,Error));
    TestTrue(TEXT("Desktop starts are labelled nominal, not measured physical calibration"),F.State->bNominalDesktopSession && !F.State->Layout.bCalibrated && F.Players[0]->CanAct());
    TestFalse(TEXT("Live combat cannot silently resize its floor"),F.Mode->HandleAdminAction(F.Admin,EMRFPSAdminAction::SetDimensions,FVector2D(500,800),Error));
    TestTrue(TEXT("Admin pause is accepted"),F.Mode->HandleAdminAction(F.Admin,EMRFPSAdminAction::Pause,FVector2D::ZeroVector,Error));
    TestTrue(TEXT("Pause keeps match playing while disabling combat"),F.State->Phase==EMRFPSMatchPhase::Playing && F.State->bCombatPaused && !F.Players[0]->CanAct());
    const int32 Shots=F.Players[0]->AcceptedShots; const float Shield=F.Players[0]->ShieldEnergy;
    F.Players[0]->ServerFire(); F.Players[0]->DeployWall(); F.Players[0]->Tick(.25f);
    TestTrue(TEXT("Paused fire and gestures cannot create combat effects"),F.Players[0]->AcceptedShots==Shots && F.Walls()==0);
    TestEqual(TEXT("Paused authority ticking freezes shield recharge and drain"),F.Players[0]->ShieldEnergy,Shield);
    TestFalse(TEXT("World itself is not paused"),F.World->IsPaused());
    TestTrue(TEXT("Authorized floor click creates a real replicated barrier"),F.Mode->PlaceAdminWall(F.Admin,FVector(0,0,500),FVector(0,0,-1),0,Error));
    TestEqual(TEXT("One stage click produces one wall"),F.Walls(),1);
    for (TActorIterator<AMRFPSBarrier> It(F.World);It;++It) TestTrue(TEXT("Admin wall is persistent, replicated and has no fake fighter owner"),It->bAdminPlaced && It->GetIsReplicated() && It->OwnerSlot==-1 && It->GetLifeSpan()==0);
    TestFalse(TEXT("All snapped wall corners must fit the configured width"),F.Mode->PlaceAdminWall(F.Admin,FVector(274,0,500),FVector(0,0,-1),0,Error));
    TestFalse(TEXT("Nonfinite placement is refused"),F.Mode->PlaceAdminWall(F.Admin,FVector(std::numeric_limits<double>::quiet_NaN(),0,500),FVector(0,0,-1),0,Error));
    F.Mode->MaxAdminWalls=1;
    TestFalse(TEXT("Authoritative wall cap bounds actor allocation"),F.Mode->PlaceAdminWall(F.Admin,FVector(100,0,500),FVector(0,0,-1),0,Error));
    TestEqual(TEXT("Capacity rejection is precise"),Error,FString(TEXT("ADMIN_WALL_CAPACITY")));
    TestTrue(TEXT("Reset clears walls and returns to waiting"),F.Mode->HandleAdminAction(F.Admin,EMRFPSAdminAction::Reset,FVector2D::ZeroVector,Error));
    TestTrue(TEXT("Reset does not start a new round implicitly"),F.State->Phase==EMRFPSMatchPhase::Waiting && !F.State->bCombatPaused && F.Walls()==0);
    for (int32 Slot=0;Slot<2;++Slot) F.Players[Slot]->Pose.bDesktop=false;
    F.Move(0,FVector(100,200,120)); F.Move(1,FVector(700,200,230));
    TestFalse(TEXT("Physical calibration is unavailable until operator confirms shared basis"),F.Mode->HandleAdminAction(F.Admin,EMRFPSAdminAction::CalibrateFloor,FVector2D::ZeroVector,Error));
    TestTrue(TEXT("Both live Stage-floor origins can be explicitly registered"),F.Mode->HandleAdminAction(F.Admin,EMRFPSAdminAction::ConfirmSharedFloor,FVector2D::ZeroVector,Error));
    TestTrue(TEXT("Registration alone does not move or calibrate the floor"),!F.State->Layout.bCalibrated && F.State->Layout.Center.IsZero());
    TestTrue(TEXT("Admin calibration computes a static measured transform"),F.Mode->HandleAdminAction(F.Admin,EMRFPSAdminAction::CalibrateFloor,FVector2D::ZeroVector,Error));
    TestTrue(TEXT("Head height difference cannot lift floor plane"),F.State->Layout.Center.Equals(FVector(400,200,0)));
    TestTrue(TEXT("Measured six metre anchors do not resize the fixed seven metre floor"),F.State->Layout.LengthCm==700 && F.State->MeasuredAnchorDistanceCm==600);
    const auto Frozen=F.State->Layout;
    F.Move(0,FVector(120,220,120)); F.Move(1,FVector(680,220,230)); F.Mode->Tick(0);
    TestTrue(TEXT("Ordinary tracked movement never chases or recentres the floor"),F.State->Layout.Center==Frozen.Center && F.State->Layout.YawDegrees==Frozen.YawDegrees && F.State->Layout.Revision==Frozen.Revision);
    ++F.Players[0]->GeometryCalibrationGeneration; F.Mode->Tick(0);
    TestTrue(TEXT("Recenter epoch invalidates shared calibration"),!F.State->bSharedFloorConfirmed && !F.State->Layout.bCalibrated);
    TestTrue(TEXT("Unavailable calibration retains last floor transform"),F.State->Layout.Center==Frozen.Center && F.State->Layout.YawDegrees==Frozen.YawDegrees);
    F.Admin->GetPlayerState<AMRFPSPlayerState>()->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()-1;
    TestFalse(TEXT("Expired admin cannot mutate state between authentication ticks"),F.Mode->HandleAdminAction(F.Admin,EMRFPSAdminAction::Reset,FVector2D::ZeroVector,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSBuildingTest,"MRFPS.Admin.BuildGridKindsAndRemoval",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSBuildingTest::RunTest(const FString& Parameters)
{
    FMRFPSArenaLayout Layout;Layout.Center=FVector(300,-200,20);Layout.YawDegrees=35;
    FTransform Snapped;
    TestTrue(TEXT("Grid coordinates are floor local even after rotated calibration"),MRFPSBuild::Snap(Layout,EMRFPSBuildKind::Crate,Layout.ToWorld(FVector(71,-119,0)),89,Snapped));
    TestTrue(TEXT("Fifty centimetre grid does not alter the configured floor"),Layout.ToLocal(Snapped.GetLocation()).Equals(FVector(50,-100,50),.01));
    TestTrue(TEXT("Rotation snaps to floor-relative quarter turns"),FMath::IsNearlyEqual(Snapped.Rotator().Yaw,125.,.01));
    TestFalse(TEXT("Invalid object kind cannot allocate a preview or server actor"),MRFPSBuild::Snap(Layout,EMRFPSBuildKind(255),Layout.Center,0,Snapped));
    TestFalse(TEXT("NaN yaw is rejected"),MRFPSBuild::Snap(Layout,EMRFPSBuildKind::Wall,Layout.Center,std::numeric_limits<float>::quiet_NaN(),Snapped));
    TestFalse(TEXT("High stacks are bounded"),MRFPSBuild::Snap(Layout,EMRFPSBuildKind::Crate,Layout.ToWorld(FVector(0,0,600)),0,Snapped));
    FAdminWorld F;if(!TestTrue(TEXT("Isolated authority world"),F.Create())) return false;
    FString Error;
    for(int32 Kind=0;Kind<4;++Kind) {
        const FVector Origin(-150+Kind*100,0,1000);
        TestFalse(TEXT("Observers cannot place any kind"),F.Mode->PlaceAdminObject(F.Observer,EMRFPSBuildKind(Kind),Origin,FVector(0,0,-1),0,Error));
        TestTrue(TEXT("Admin can place each kind"),F.Mode->PlaceAdminObject(F.Admin,EMRFPSBuildKind(Kind),Origin,FVector(0,0,-1),0,Error));
    }
    TestEqual(TEXT("Four server actors are replicated"),F.Walls(),4);
    AMRFPSBarrier* Stairs=nullptr;
    for(TActorIterator<AMRFPSBarrier> It(F.World);It;++It) {
        TestTrue(TEXT("All build objects have their actual kind replicated"),It->GetIsReplicated()&&It->bAdminPlaced);
        if(It->BuildKind==EMRFPSBuildKind::Stairs) Stairs=*It;
    }
    if(TestNotNull(TEXT("Stair actor"),Stairs)) {
        TestEqual(TEXT("Stair body is not an invisible solid box"),Stairs->Collision->GetCollisionEnabled(),ECollisionEnabled::NoCollision);
        for(const auto& Step:Stairs->BuildSteps) TestTrue(TEXT("Each visible stair step has real bullet collision"),Step&&Step->IsVisible()&&Step->GetCollisionResponseToChannel(ECC_GameTraceChannel1)==ECR_Block);
    }
    TestFalse(TEXT("Observers cannot delete an object"),F.Mode->RemoveAdminObject(F.Observer,FVector(-150,0,1000),FVector(0,0,-1),Error));
    TestTrue(TEXT("Admin ray deletes the selected object"),F.Mode->RemoveAdminObject(F.Admin,FVector(-150,0,1000),FVector(0,0,-1),Error));
    TestEqual(TEXT("Deletion leaves the other kinds intact"),F.Walls(),3);
    return true;
}
namespace
{
class FAdminPauseScenario final : public IAutomationLatentCommand
{
public:
    explicit FAdminPauseScenario(FAutomationTestBase* InTest):Test(InTest) {}
    virtual bool Update() override
    {
        if (LastFrame==GFrameCounter) return false; LastFrame=GFrameCounter;
        FString Error;
        if (!Fixture.World) {
            if (!Test->TestTrue(TEXT("Pause fixture creates actual authority actors"),Fixture.Create())) return true;
            Fixture.Move(0,FVector(-150,0,170)); Fixture.Move(1,FVector(150,0,170));
            if (!Fixture.Mode->HandleAdminAction(Fixture.Admin,EMRFPSAdminAction::Start,FVector2D::ZeroVector,Error)) { Test->AddError(Error); return true; }
            Fixture.Players[0]->DeployWall();
            for (TActorIterator<AMRFPSBarrier> It(Fixture.World);It;++It) Wall=*It;
            if (!Test->TestTrue(TEXT("A real fighter barrier has a running lifetime before pause"),Wall.IsValid() && Wall->GetLifeSpan()>5.9f)) return true;
            Fixture.Players[1]->ApplyShot(100,Fixture.Players[0],false); Fixture.Players[0]->ShieldEnergy=50;
            Cooldown=Fixture.Players[0]->WallReadyAt; Respawn=Fixture.Players[1]->RespawnAt;
            if (!Fixture.Mode->HandleAdminAction(Fixture.Admin,EMRFPSAdminAction::Pause,FVector2D::ZeroVector,Error)) { Test->AddError(Error); return true; }
            PausedAt=Fixture.World->GetTimeSeconds(); return false;
        }
        for (auto* Fighter:Fixture.Players) Fighter->LastPoseReceived=Fixture.World->GetTimeSeconds();
        Fixture.World->Tick(LEVELTICK_All,.05f);
        const double Now=Fixture.World->GetTimeSeconds();
        if (Now>23) { Test->AddError(TEXT("Pause lifetime scenario exceeded deadline")); return true; }
        if (Stage==0 && Now-PausedAt>=7) {
            Test->TestTrue(TEXT("A seven second combat pause does not expire a six second wall"),Wall.IsValid() && !Wall->IsActorBeingDestroyed() && Wall->GetLifeSpan()==0);
            Test->TestEqual(TEXT("Paused time cannot respawn an eliminated player"),Fixture.Players[1]->Health,0.f);
            Test->TestEqual(TEXT("Paused time cannot recharge shield"),Fixture.Players[0]->ShieldEnergy,50.f);
            if (!Fixture.Mode->HandleAdminAction(Fixture.Admin,EMRFPSAdminAction::Resume,FVector2D::ZeroVector,Error)) { Test->AddError(Error); return true; }
            ResumedAt=Now;
            Test->TestTrue(TEXT("Resume restores the remaining wall lifetime"),Wall.IsValid() && FMath::IsNearlyEqual(Wall->GetLifeSpan(),6.f,.1f));
            Test->TestTrue(TEXT("Cooldown and respawn deadlines are shifted by paused duration"),
                FMath::IsNearlyEqual(Fixture.Players[0]->WallReadyAt-Cooldown,Now-PausedAt,.05) &&
                FMath::IsNearlyEqual(Fixture.Players[1]->RespawnAt-Respawn,Now-PausedAt,.05));
            Fixture.Players[0]->DeployWall(); Test->TestEqual(TEXT("Pause does not consume the fighter wall cooldown"),Fixture.Walls(),1);
            ++Stage;
        } else if (Stage==1 && Now-ResumedAt>=2.7) {
            Test->TestEqual(TEXT("Respawn still waits for the remaining unpaused interval"),Fixture.Players[1]->Health,0.f); ++Stage;
        } else if (Stage==2 && Now-ResumedAt>=3.2) {
            Test->TestEqual(TEXT("Normal authority ticking eventually completes remaining respawn"),Fixture.Players[1]->Health,100.f); ++Stage;
        } else if (Stage==3 && Now-ResumedAt>=6.2) {
            Test->TestTrue(TEXT("Real engine lifespan expires the wall after its remaining combat time"),!Wall.IsValid() || Wall->IsActorBeingDestroyed());
            Test->TestEqual(TEXT("No paused wall is leaked"),Fixture.Walls(),0); return true;
        }
        return false;
    }
private:
    FAutomationTestBase* Test;
    FAdminWorld Fixture;
    TWeakObjectPtr<AMRFPSBarrier> Wall;
    uint64 LastFrame=MAX_uint64;
    double PausedAt=0,ResumedAt=0,Cooldown=0,Respawn=0;
    int32 Stage=0;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSAdminPauseTest,"MRFPS.Admin.CombatPauseLifetime",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSAdminPauseTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FAdminPauseScenario(this)); return true;
}
#endif
