#include "MRFPSBot.h"
#include "MRFPSGame.h"
#include "MRFPSBuild.h"
#include "MRFPSGeometryWorld.h"
#include "MRFPSHapticsComponent.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BrainComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_SimpleGrid.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformTime.h"
#include <cfloat>

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSBot,Log,All);

namespace
{
constexpr ECollisionChannel BotChannel=ECC_GameTraceChannel1;
constexpr int32 MaxCandidates=8;
constexpr double QueryInterval=1.5;
TMap<TWeakObjectPtr<UWorld>,double> RetryAfter;

AMRFPSBotController* FindBot(UWorld* World)
{
    if (!World) return nullptr;
    TActorIterator<AMRFPSBotController> It(World);
    return It ? *It : nullptr;
}
AMRFPSPawn* FighterForSlot(UWorld* World,int32 Slot)
{
    AMRFPSPawn* Found=nullptr;
    for (TActorIterator<AMRFPSPawn> It(World);It;++It) if (It->GetSlot()==Slot) {
        if (Found) return nullptr;
        Found=*It;
    }
    return Found;
}
bool RealHumanReady(const AMRFPSPawn* Human)
{
    const auto* PS=Human?Human->GetPlayerState<AMRFPSPlayerState>():nullptr;
    return PS && !PS->IsABot() && PS->bAuthenticated && !PS->bInspector && !PS->bAdmin && PS->Slot>=0 && PS->Slot<2 &&
        PS->AuthExpiresAt>FDateTime::UtcNow().ToUnixTimestamp() && Human->bCalibrated && Human->Pose.bHeadTracked &&
        Human->Pose.IsSane() && Human->IsPoseFresh() &&
        (Human->Pose.bDesktop || (Human->bStageFloorReferenced && Human->GeometryCalibrationGeneration!=0));
}
bool PathClear(UWorld* World,const AMRFPSPawn* Bot,const FVector& Start,const FVector& End)
{
    if (!World || !Bot || Start.ContainsNaN() || End.ContainsNaN()) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MRFPSBotMovement),false,Bot);
    FHitResult Hit;
    const FVector Lift(0,0,90);
    if (World->SweepSingleByChannel(Hit,Start+Lift,End+Lift,FQuat::Identity,BotChannel,FCollisionShape::MakeCapsule(22,75),Params)) return false;
    if (const auto* Geometry=World->GetSubsystem<UMRFPSGeometryWorld>()) {
        const FVector Side=FVector::CrossProduct(FVector::UpVector,(End-Start).GetSafeNormal2D())*20;
        const FVector Offsets[]={FVector(0,0,30),FVector(0,0,90),FVector(0,0,155),Side+FVector(0,0,90),-Side+FVector(0,0,90)};
        for (const FVector& Offset:Offsets) { FVector Point; if (Geometry->Trace(Start+Offset,End+Offset,Point)) return false; }
    }
    return true;
}
bool FindRoundSpawn(UWorld* World,const AMRFPSPawn* Bot,const FMRFPSArenaLayout& Layout,FVector& OutFoot)
{
    if (!World || !Bot || Bot->GetSlot()<0 || Bot->GetSlot()>1) return false;
    for (double Inset:{40.,120.,200.}) for (double X:{0.,-80.,80.}) {
        const FVector Local(X,(Bot->GetSlot()==0?-1.:1.)*(Layout.LengthCm*.5-Inset),0);
        const FVector Foot=Layout.ToWorld(Local);
        if (Layout.ContainsPlanar(Foot,25) && PathClear(World,Bot,Foot,Foot+FVector(1,0,0))) {
            OutFoot=Foot; return true;
        }
    }
    return false;
}
// Layout edits relocate admin pieces. Check their proposed locations rather
// than their current locations; treat openings as solid for a safe placement.
bool FutureStageBlocks(UWorld* World,const FMRFPSArenaLayout& Layout,const FVector& Foot)
{
    const FVector Center=Foot+FVector(0,0,90);
    for (TActorIterator<AMRFPSBarrier> It(World);It;++It) {
        const auto* Piece=*It;
        if (!Piece->bAdminPlaced || !Piece->bMapAnchor) continue;
        const FTransform Transform(FRotator(0,Layout.YawDegrees+Piece->MapLocalYaw,0),Layout.ToWorld(Piece->MapLocalPosition));
        const bool Fits=Piece->BuildKind==EMRFPSBuildKind::Wall
            ? MRFPSBuild::FitsWall(Layout,Transform,Piece->WallShape) : MRFPSBuild::Fits(Layout,Piece->BuildKind,Transform);
        if (!Fits) continue; // ApplyLayoutChange removes pieces outside the new layout.
        const FVector P=Transform.InverseTransformPosition(Center);
        if (Piece->BuildKind==EMRFPSBuildKind::Wall) {
            const double Bottom=-100,Top=Bottom+Piece->WallShape.HeightRows*MRFPSBuild::WallRowHeightCm;
            if (FMath::Abs(P.X)<=MRFPSBuild::WallHalfThicknessCm+22 && FMath::Abs(P.Y)<=MRFPSBuild::WallWidthCm*.5+22 &&
                P.Z+75>=Bottom && P.Z-75<=Top) return true;
        } else {
            const FVector Extent=MRFPSBuild::Extent(Piece->BuildKind);
            if (FMath::Abs(P.X)<=Extent.X+22 && FMath::Abs(P.Y)<=Extent.Y+22 && FMath::Abs(P.Z)<=Extent.Z+75) return true;
        }
    }
    return false;
}
bool ResizeSpawnClear(UWorld* World,const AMRFPSPawn* Bot,const AMRFPSPawn* Human,const FMRFPSArenaLayout& Layout,const FVector& Foot)
{
    if (!World || !Bot || !Human || !Layout.ContainsPlanar(Foot,25) ||
        FVector::DistSquared2D(Foot,Human->Pose.Head)<FMath::Square(110.) || FutureStageBlocks(World,Layout,Foot)) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MRFPSBotResize),false,Bot);
    // Every existing barrier is either relocated or removed by ApplyLayoutChange.
    for (TActorIterator<AMRFPSBarrier> It(World);It;++It) Params.AddIgnoredActor(*It);
    const FVector Center=Foot+FVector(0,0,90);
    if (World->OverlapBlockingTestByChannel(Center,FQuat::Identity,BotChannel,FCollisionShape::MakeCapsule(22,75),Params)) return false;
    const auto* Geometry=World->GetSubsystem<UMRFPSGeometryWorld>();
    const FVector Extent(22,22,75);
    return Geometry && !Geometry->OverlapsMeasuredBounds(FBox(Center-Extent,Center+Extent));
}
bool ClearShot(UWorld* World,const AMRFPSPawn* Bot,const AMRFPSPawn* Human)
{
    if (!World || !Bot || !Human) return false;
    const FVector Start=Bot->Muzzle(), End=Start+Bot->Pose.RightRotation.Vector()*2000;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MRFPSBotSight),false,Bot);
    FHitResult Hit;
    if (!World->LineTraceSingleByChannel(Hit,Start,End,BotChannel,Params) || Hit.GetActor()!=Human) return false;
    if (const auto* Geometry=World->GetSubsystem<UMRFPSGeometryWorld>()) {
        FVector Point;
        if (Geometry->Trace(Start,End,Point) && FVector::DistSquared(Start,Point)<=FVector::DistSquared(Start,Hit.ImpactPoint)) return false;
    }
    return true;
}
void RetireBot(AMRFPSGameMode* Mode,const TCHAR* Reason)
{
    if (!Mode || !Mode->HasAuthority()) return;
    auto* GS=Mode->GetGameState<AMRFPSGameState>();
    auto* Bot=FindBot(Mode->GetWorld());
    if (!Bot && (!GS || !GS->bSoloBotSession)) return;
    if (Bot) {
        if (APawn* ControlledPawn=Bot->GetPawn()) { Bot->UnPossess(); ControlledPawn->Destroy(); }
        Bot->Destroy();
    }
    if (GS && GS->bSoloBotSession) {
        GS->bSoloBotSession=false; GS->bSoloGeometryReady=false; GS->SoloHumanSlot=-1; GS->SoloHumanCalibration=0;
        GS->bSharedFloorConfirmed=false; GS->Layout.bCalibrated=false; GS->Layout.RegistrationRevision=0;
        if (GS->Layout.Revision<MAX_uint32) ++GS->Layout.Revision;
        Mode->ApplyBotLayoutChange(); GS->ForceNetUpdate();
    }
    Mode->ResetForFighterChange();
    UE_LOG(LogMRFPSBot,Display,TEXT("MRFPS_BOT_REMOVED reason=%s"),Reason);
}
bool SoloLayout(const AMRFPSPawn* Human,const FMRFPSArenaLayout& Current,FMRFPSArenaLayout& Result)
{
    if (!RealHumanReady(Human) || !Current.IsSane() || Current.Revision==MAX_uint32) return false;
    const int32 Slot=Human->GetSlot();
    const double ForwardYaw=Human->Pose.HeadRotation.Yaw+(Slot==1?180.:0.);
    Result=Current;
    Result.YawDegrees=FRotator::NormalizeAxis(ForwardYaw-90.);
    const FVector Y=FRotator(0,Result.YawDegrees,0).RotateVector(FVector(0,1,0));
    const FVector Head=Human->Pose.Head;
    Result.Center=FVector(Head.X,Head.Y,Human->Pose.bDesktop?Current.Center.Z:0)+Y*(Slot==0?Current.LengthCm*.5:-Current.LengthCm*.5);
    Result.Center.Z=Human->Pose.bDesktop?Current.Center.Z:0;
    Result.bCalibrated=false; Result.RegistrationRevision=0; ++Result.Revision;
    return Result.IsSane() && Result.ContainsHead(Head);
}
}

UMRFPSBotCombatTask::UMRFPSBotCombatTask() { NodeName=TEXT("MRFPS bot combat"); bNotifyTick=true; }
EBTNodeResult::Type UMRFPSBotCombatTask::ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory)
{ return Cast<AMRFPSBotController>(OwnerComp.GetAIOwner())?EBTNodeResult::InProgress:EBTNodeResult::Failed; }
void UMRFPSBotCombatTask::TickTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,float DeltaSeconds)
{ if (auto* Bot=Cast<AMRFPSBotController>(OwnerComp.GetAIOwner())) Bot->Step(DeltaSeconds); }

AMRFPSBotController::AMRFPSBotController() { bWantsPlayerState=true; }
bool AMRFPSBotController::InitializeBot(AMRFPSPawn* Fighter,AMRFPSPawn* Human)
{
    if (!HasAuthority() || !Fighter || !Human || !GetWorld()) return false;
    HumanPlayer=Human; Possess(Fighter);
    if (GetPawn()!=Fighter) return false;
    RuntimeQuery=NewObject<UEnvQuery>(this);
    auto* Option=NewObject<UEnvQueryOption>(RuntimeQuery);
    auto* Grid=NewObject<UEnvQueryGenerator_SimpleGrid>(Option);
    Grid->GridSize.DefaultValue=200; Grid->SpaceBetween.DefaultValue=100;
    Grid->ProjectionData.TraceMode=EEnvQueryTrace::None; // no baked NavMesh required
    Option->Generator=Grid; RuntimeQuery->GetOptionsMutable().Add(Option);
    RuntimeTree=NewObject<UBehaviorTree>(this);
    auto* Root=NewObject<UBTComposite_Sequence>(RuntimeTree);
    FBTCompositeChild Child; Child.ChildTask=NewObject<UMRFPSBotCombatTask>(RuntimeTree);
    Root->Children.Add(Child); RuntimeTree->RootNode=Root;
    bBrainReady=RunBehaviorTree(RuntimeTree);
    if (!bBrainReady) { UE_LOG(LogMRFPSBot,Error,TEXT("MRFPS_BOT_AI_UNAVAILABLE")); }
    else { UE_LOG(LogMRFPSBot,Display,TEXT("MRFPS_BOT_BT_READY")); }
    return bBrainReady;
}
bool AMRFPSBotController::IsPreparedFor(const AMRFPSPawn* Human) const
{
    const auto* Fighter=GetPawn<AMRFPSPawn>();
    return bBrainReady && GetBrainComponent() && GetBrainComponent()->IsRunning() && bQuerySucceeded &&
        HumanPlayer.Get()==Human && Fighter && Fighter->bCalibrated && Fighter->Pose.bHeadTracked && Fighter->IsPoseFresh();
}
void AMRFPSBotController::ResetNavigationForLayout()
{
    if (bQueryInFlight && QueryId!=INDEX_NONE) if (auto* Manager=UEnvQueryManager::GetCurrent(GetWorld())) Manager->AbortQuery(QueryId);
    bQueryInFlight=false; QueryId=INDEX_NONE; bHasDestination=false; NextQueryAt=0;
}
bool AMRFPSBotController::ResetForRound()
{
    auto* Fighter=GetPawn<AMRFPSPawn>(); auto* Human=HumanPlayer.Get();
    const auto* GS=GetWorld()?GetWorld()->GetGameState<AMRFPSGameState>():nullptr;
    if (!HasAuthority() || !Fighter || !Human || !GS || !GS->bSoloBotSession) return false;
    const double Now=GetWorld()->GetTimeSeconds(); if (Now<NextSpawnRetryAt) return false;
    FVector Foot;
    if (!FindRoundSpawn(GetWorld(),Fighter,GS->Layout,Foot)) {
        NextSpawnRetryAt=Now+.5;
        if (Now>=NextFailureLogAt) { UE_LOG(LogMRFPSBot,Warning,TEXT("MRFPS_BOT_ROUND_SPAWN_BLOCKED")); NextFailureLogAt=Now+5; }
        return false;
    }
    NextSpawnRetryAt=0; ResetNavigationForLayout(); bWasDead=false; NextShotAt=ReloadAt=0;
    FMRFPSPose Next=Fighter->Pose; const FVector Head=Foot+FVector(0,0,170);
    const FVector Delta=Head-FVector(Next.Head);
    Next.Head=Head; Next.Left=FVector(Next.Left)+Delta; Next.Right=FVector(Next.Right)+Delta; ++Next.Sequence;
    Fighter->SetActorLocation(Foot,false,nullptr,ETeleportType::TeleportPhysics);
    Fighter->LastPoseReceived=-100; Fighter->ServerPose(Next); Fighter->ForceNetUpdate();
    UE_LOG(LogMRFPSBot,Display,TEXT("MRFPS_BOT_ROUND_SPAWN slot=%d foot=%s"),Fighter->GetSlot(),*Foot.ToString());
    return true;
}
void AMRFPSBotController::Step(float DeltaSeconds)
{
    auto* Fighter=GetPawn<AMRFPSPawn>(); auto* Human=HumanPlayer.Get();
    auto* GS=GetWorld()?GetWorld()->GetGameState<AMRFPSGameState>():nullptr;
    if (!HasAuthority() || !Fighter || !Human || !GS || !GS->bSoloBotSession || !RealHumanReady(Human)) return;
    const double Now=GetWorld()->GetTimeSeconds();
    // Bound measured-mesh tracing even when the server ticks at 60+ FPS.
    if (Now<NextStepAt) return;
    const float StepSeconds=LastStepAt<0?0.f:FMath::Clamp(float(Now-LastStepAt),0.f,.12f);
    LastStepAt=Now; NextStepAt=Now+.10;
    if (!Human->Pose.bDesktop && !GS->bSoloGeometryReady && Now>=NextGeometryLogAt) {
        UE_LOG(LogMRFPSBot,Warning,TEXT("MRFPS_BOT_GEOMETRY_UNAVAILABLE")); NextGeometryLogAt=Now+15;
    }
    FVector Head=Fighter->Pose.Head;
    if (Fighter->Health<=0) { bWasDead=true; bHasDestination=false; }
    else if (bWasDead) {
        FVector Spawn=GS->Layout.ClampPlanar(GS->Layout.NominalStart(Fighter->GetSlot()),40);
        Spawn.Z=GS->Layout.Center.Z;
        if (PathClear(GetWorld(),Fighter,Spawn,Spawn+FVector(1,0,0))) {
            Head=Spawn+FVector(0,0,170); Fighter->LastPoseReceived=-100;
        } else { UE_LOG(LogMRFPSBot,Warning,TEXT("MRFPS_BOT_RESPAWN_BLOCKED")); }
        bWasDead=false;
    }
    const FVector Foot=Head-FVector(0,0,170);
    if (Fighter->Health>0 && GS->Phase==EMRFPSMatchPhase::Playing && !GS->bCombatPaused && bHasDestination) {
        const FVector Delta=Destination-Foot;
        const FVector Move=Delta.GetClampedToMaxSize(StepSeconds*110.f);
        if (Move.SizeSquared()>1.e-4 && GS->Layout.ContainsPlanar(Foot+Move,25) && PathClear(GetWorld(),Fighter,Foot,Foot+Move)) {
            Head+=Move;
            if (!bLoggedMove) { bLoggedMove=true; UE_LOG(LogMRFPSBot,Display,TEXT("MRFPS_BOT_MOVED")); }
        } else bHasDestination=false;
        if (Delta.SizeSquared2D()<FMath::Square(8.)) bHasDestination=false;
    }
    FMRFPSPose Pose=Fighter->Pose;
    const FVector Toward=FVector(Human->Pose.Head)-Head;
    const FRotator Facing=FRotator(0,Toward.Rotation().Yaw,0);
    Pose.Head=Head; Pose.HeadRotation=Facing; Pose.bHeadTracked=Pose.bLeftTracked=Pose.bRightTracked=true; Pose.bDesktop=false;
    Pose.Left=Head+Facing.RotateVector(FVector(25,-25,-40));
    Pose.Right=Head+Facing.RotateVector(FVector(35,18,-30));
    Pose.RightRotation=(FVector(Human->Pose.Head)-FVector(0,0,58)-FVector(Pose.Right)).Rotation();
    Pose.RightHandRotation=Pose.RightRotation; Pose.LeftRotation=Facing; Pose.LeftOpenness=0;
    ++Pose.Sequence;
    Fighter->SetActorLocation(FVector(Head.X,Head.Y,GS->Layout.Center.Z));
    Fighter->ServerPose(Pose);
    if (bQueryInFlight && Now>=QueryDeadline) {
        if (auto* Manager=UEnvQueryManager::GetCurrent(GetWorld())) Manager->AbortQuery(QueryId);
        bQueryInFlight=false; QueryId=INDEX_NONE;
        UE_LOG(LogMRFPSBot,Warning,TEXT("MRFPS_BOT_EQS_FAILED reason=timeout"));
    }
    if (!bQueryInFlight && Now>=NextQueryAt && RuntimeQuery) {
        NextQueryAt=Now+QueryInterval;
        QueryId=FEnvQueryRequest(RuntimeQuery,Fighter).Execute(EEnvQueryRunMode::AllMatching,this,&AMRFPSBotController::QueryFinished);
        bQueryInFlight=QueryId!=INDEX_NONE; QueryDeadline=Now+3;
        if (!bQueryInFlight && Now>=NextFailureLogAt) { UE_LOG(LogMRFPSBot,Warning,TEXT("MRFPS_BOT_EQS_FAILED reason=start")); NextFailureLogAt=Now+5; }
    }
    if (Fighter->Health<=0 || GS->Phase!=EMRFPSMatchPhase::Playing || GS->bCombatPaused || !Fighter->CanAct()) return;
    if (Fighter->MagazineRounds<=0) {
        if (ReloadAt<=0) ReloadAt=Now+1.2;
        if (Now>=ReloadAt) { Fighter->ServerDesktopReload(); ReloadAt=0; }
        return;
    }
    ReloadAt=0;
    if (Now>=NextShotAt && Human->CanAct() && ClearShot(GetWorld(),Fighter,Human)) {
        NextShotAt=Now+.35;
        const int32 Before=Fighter->AcceptedShots;
        Fighter->ServerFire();
        if (Fighter->AcceptedShots>Before && !bLoggedShot) { bLoggedShot=true; UE_LOG(LogMRFPSBot,Display,TEXT("MRFPS_BOT_FIRED")); }
    }
}
void AMRFPSBotController::QueryFinished(TSharedPtr<FEnvQueryResult> Result)
{
    bQueryInFlight=false; QueryId=INDEX_NONE;
    auto* Fighter=GetPawn<AMRFPSPawn>(); auto* Human=HumanPlayer.Get();
    const auto* GS=GetWorld()?GetWorld()->GetGameState<AMRFPSGameState>():nullptr;
    if (!Result.IsValid() || !Result->IsSuccessful() || !Fighter || !Human || !GS || !GS->bSoloBotSession) {
        UE_LOG(LogMRFPSBot,Warning,TEXT("MRFPS_BOT_EQS_FAILED reason=result")); return;
    }
    bQuerySucceeded=true;
    const int32 Count=Result->Items.Num();
    if (!bLoggedQuery) { bLoggedQuery=true; UE_LOG(LogMRFPSBot,Display,TEXT("MRFPS_BOT_EQS_READY candidates=%d"),Count); }
    const FVector Foot=FVector(Fighter->Pose.Head)-FVector(0,0,170);
    double Best=-DBL_MAX; bool Found=false; FVector Chosen;
    const int32 Offset=Count>0?int32(GetWorld()->GetTimeSeconds())%Count:0;
    for (int32 I=0;I<FMath::Min(Count,MaxCandidates);++I) {
        const FVector Point=Result->GetItemAsLocation((Offset+I*3)%Count);
        if (!GS->Layout.ContainsPlanar(Point,30) || FVector::DistSquared2D(Point,Foot)<FMath::Square(20.) ||
            FVector::DistSquared2D(Point,Human->Pose.Head)<FMath::Square(110.) || !PathClear(GetWorld(),Fighter,Foot,Point)) continue;
        const double Score=-FMath::Abs(FVector::Dist2D(Point,Human->Pose.Head)-260.)-.10*FVector::Dist2D(Point,Foot);
        if (!Found || Score>Best) { Found=true; Best=Score; Chosen=Point; }
    }
    bHasDestination=Found; if (Found) Destination=Chosen;
    else if (GetWorld()->GetTimeSeconds()>=NextFailureLogAt) { UE_LOG(LogMRFPSBot,Warning,TEXT("MRFPS_BOT_NO_CLEAR_MOVE")); NextFailureLogAt=GetWorld()->GetTimeSeconds()+5; }
}

namespace MRFPSBot
{
bool CanMove(UWorld* World,const AMRFPSPawn* Fighter,const FVector& Start,const FVector& End)
{ return PathClear(World,Fighter,Start,End); }
bool IsServerBot(const AMRFPSPawn* Fighter)
{
    const auto* PS=Fighter?Fighter->GetPlayerState<AMRFPSPlayerState>():nullptr;
    return Fighter && Fighter->HasAuthority() && PS && PS->IsABot() && Cast<AMRFPSBotController>(Fighter->GetController());
}
int32 CountHumanFighters(const AMRFPSGameMode* Mode,const AMRFPSPlayerState* Exclude)
{
    int32 Count=0;
    const auto* GS=Mode?Mode->GetGameState<AMRFPSGameState>():nullptr;
    if (GS) for (APlayerState* Base:GS->PlayerArray) if (const auto* PS=Cast<AMRFPSPlayerState>(Base);
        PS && PS!=Exclude && !PS->IsABot() && !PS->bInspector && !PS->bAdmin && PS->Slot>=0 && PS->Slot<2) ++Count;
    return Count;
}
bool IsPrepared(const AMRFPSGameMode* Mode)
{
    const auto* GS=Mode?Mode->GetGameState<AMRFPSGameState>():nullptr;
    if (!Mode || !Mode->HasAuthority() || !GS || !GS->bSoloBotSession || GS->SoloHumanSlot<0 || GS->SoloHumanSlot>=2 ||
        CountHumanFighters(Mode)!=1) return false;
    auto* Human=FighterForSlot(Mode->GetWorld(),GS->SoloHumanSlot); auto* Bot=FindBot(Mode->GetWorld());
    const auto* BotPawn=Bot?Bot->GetPawn<AMRFPSPawn>():nullptr;
    return RealHumanReady(Human) && Human->IsInsideArena() && (Human->Pose.bDesktop ||
        (GS->SoloHumanCalibration && Human->GeometryCalibrationGeneration==GS->SoloHumanCalibration)) &&
        Bot && IsServerBot(BotPawn) && BotPawn->GetSlot()==1-GS->SoloHumanSlot && BotPawn->IsInsideArena() && Bot->IsPreparedFor(Human);
}
bool ResetForRound(AMRFPSGameMode* Mode)
{
    if (!Mode || !Mode->HasAuthority()) return false;
    const auto* GS=Mode->GetGameState<AMRFPSGameState>();
    if (!GS || !GS->bSoloBotSession) return true;
    auto* Bot=FindBot(Mode->GetWorld()); return Bot && Bot->ResetForRound();
}
bool IsTrackingPause(const AMRFPSGameMode* Mode)
{
    const auto* GS=Mode?Mode->GetGameState<AMRFPSGameState>():nullptr;
    const auto* Bot=Mode?FindBot(Mode->GetWorld()):nullptr;
    return Mode && Mode->HasAuthority() && GS && GS->bSoloBotSession && GS->bCombatPaused && Bot && Bot->bPausedForTracking;
}
bool ClaimTrackingPause(AMRFPSGameMode* Mode)
{
    if (!IsTrackingPause(Mode)) return false;
    auto* Bot=FindBot(Mode->GetWorld());
    Bot->bPausedForTracking=false;
    UE_LOG(LogMRFPSBot,Display,TEXT("MRFPS_BOT_ADMIN_PAUSE_HELD"));
    return true;
}
void CancelAutoStart(AMRFPSGameMode* Mode)
{
    if (Mode && Mode->HasAuthority()) if (auto* Bot=FindBot(Mode->GetWorld())) Bot->bAutoStartPending=false;
}
void BeforeHumanAdmission(AMRFPSGameMode* Mode) { RetireBot(Mode,TEXT("human_join")); }
bool ResizeSolo(AMRFPSGameMode* Mode,double Width,double Length,FString& Error)
{
    Error=TEXT("BOT_RESIZE_NOT_READY");
    if (!Mode || !Mode->HasAuthority() || !Mode->GetWorld()) return false;
    auto* GS=Mode->GetGameState<AMRFPSGameState>();
    if (!GS || !GS->bSoloBotSession || GS->SoloHumanSlot<0 || GS->SoloHumanSlot>=2 || CountHumanFighters(Mode)!=1) return false;
    auto* Human=FighterForSlot(Mode->GetWorld(),GS->SoloHumanSlot);
    auto* Bot=FindBot(Mode->GetWorld()); auto* Fighter=Bot?Bot->GetPawn<AMRFPSPawn>():nullptr;
    if (!RealHumanReady(Human) || !Bot || !IsServerBot(Fighter) || Fighter->GetSlot()!=1-GS->SoloHumanSlot ||
        !Bot->IsPreparedFor(Human) || (!Human->Pose.bDesktop &&
        (!GS->SoloHumanCalibration || Human->GeometryCalibrationGeneration!=GS->SoloHumanCalibration))) return false;
    FMRFPSArenaLayout NextLayout=GS->Layout;
    if (!NextLayout.SetDimensions(Width,Length,Error)) return false;
    const FVector Y=FRotator(0,NextLayout.YawDegrees,0).RotateVector(FVector(0,1,0));
    const FVector HumanHead=Human->Pose.Head;
    NextLayout.Center=FVector(HumanHead.X,HumanHead.Y,GS->Layout.Center.Z)+Y*(GS->SoloHumanSlot==0?Length*.5:-Length*.5);
    NextLayout.Center.Z=GS->Layout.Center.Z;
    Error=TEXT("BOT_RESIZE_NOT_READY");
    if (!NextLayout.IsSane() || !NextLayout.ContainsHead(HumanHead)) return false;
    FVector Foot; bool Found=false;
    for (double Inset:{40.,120.,200.}) {
        for (double X:{0.,-80.,80.}) {
            const FVector Candidate=NextLayout.ToWorld(FVector(X,(Fighter->GetSlot()==0?-1.:1.)*(NextLayout.LengthCm*.5-Inset),0));
            if (!ResizeSpawnClear(Mode->GetWorld(),Fighter,Human,NextLayout,Candidate)) continue;
            Foot=Candidate; Found=true; break;
        }
        if (Found) break;
    }
    Error=TEXT("BOT_RESIZE_BLOCKED");
    if (!Found) return false;
    FMRFPSPose Pose=Fighter->Pose;
    const FVector Head=Foot+FVector(0,0,170);
    const FRotator Facing(0,(HumanHead-Head).Rotation().Yaw,0);
    Pose.Head=Head; Pose.HeadRotation=Facing; Pose.bHeadTracked=Pose.bLeftTracked=Pose.bRightTracked=true; Pose.bDesktop=false;
    Pose.Left=Head+Facing.RotateVector(FVector(25,-25,-40)); Pose.Right=Head+Facing.RotateVector(FVector(35,18,-30));
    Pose.RightRotation=(HumanHead-FVector(0,0,58)-FVector(Pose.Right)).Rotation(); Pose.RightHandRotation=Pose.RightRotation;
    Pose.LeftRotation=Facing; Pose.LeftOpenness=0; ++Pose.Sequence;
    if (!Pose.IsSane()) return false;
    // All validation above is read-only. Commit on the game thread only after
    // both the proposed layout and a collision-free placement are known.
    Bot->ResetNavigationForLayout(); GS->Layout=NextLayout; GS->bSoloGeometryReady=false;
    Fighter->SetActorLocation(Foot,false,nullptr,ETeleportType::TeleportPhysics);
    Fighter->LastPoseReceived=-100; Fighter->ServerPose(Pose);
    Mode->ApplyBotLayoutChange(); GS->ForceNetUpdate(); Error.Empty(); return true;
}
void Maintain(AMRFPSGameMode* Mode,float DeltaSeconds)
{
    if (!Mode || !Mode->HasAuthority() || !Mode->GetWorld()) return;
    auto* GS=Mode->GetGameState<AMRFPSGameState>(); if (!GS) return;
    const int32 Humans=CountHumanFighters(Mode); auto* Bot=FindBot(Mode->GetWorld());
    if (Humans!=1) { if (Bot || GS->bSoloBotSession) RetireBot(Mode,Humans?TEXT("human_join"):TEXT("no_human")); return; }
    AMRFPSPawn* Human=nullptr;
    for (int32 Slot=0;Slot<2;++Slot) if (auto* Candidate=FighterForSlot(Mode->GetWorld(),Slot))
        if (const auto* PS=Candidate->GetPlayerState<AMRFPSPlayerState>(); PS && !PS->IsABot() && PS->bAuthenticated) Human=Candidate;
    if (GS->bSoloBotSession) {
        if (!Bot || !Human || Human->GetSlot()!=GS->SoloHumanSlot || (Human->bCalibrated && !Human->Pose.bDesktop && Human->GeometryCalibrationGeneration!=GS->SoloHumanCalibration)) {
            RetireBot(Mode,TEXT("calibration_changed")); return;
        }
        if (Bot->bPausedForTracking && !GS->bCombatPaused) Bot->bPausedForTracking=false;
        if (GS->Phase!=EMRFPSMatchPhase::Waiting) Bot->bAutoStartPending=false;
        if (!RealHumanReady(Human)) {
            if (GS->bSoloGeometryReady) { GS->bSoloGeometryReady=false; GS->ForceNetUpdate(); }
            if (!GS->bCombatPaused && (GS->Phase==EMRFPSMatchPhase::Playing || GS->Phase==EMRFPSMatchPhase::BetweenRounds)) {
                Mode->SetBotTrackingPause(true); Bot->bPausedForTracking=true;
                UE_LOG(LogMRFPSBot,Warning,TEXT("MRFPS_BOT_TRACKING_PAUSED"));
            }
            return;
        }
        const auto* Geometry=Mode->GetWorld()->GetSubsystem<UMRFPSGeometryWorld>();
        const bool Measured=Geometry && Geometry->HasLiveMeasurement(Human->GetSlot());
        if (GS->bSoloGeometryReady!=Measured) { GS->bSoloGeometryReady=Measured; GS->ForceNetUpdate(); }
        if (Bot->bPausedForTracking && GS->bCombatPaused && IsPrepared(Mode)) {
            Mode->SetBotTrackingPause(false); Bot->bPausedForTracking=false;
            UE_LOG(LogMRFPSBot,Display,TEXT("MRFPS_BOT_TRACKING_RESUMED"));
        }
        if (Bot->bAutoStartPending && IsPrepared(Mode) && Mode->StartPreparedSoloMatch()) Bot->bAutoStartPending=false;
        return;
    }
    if (Bot) { RetireBot(Mode,TEXT("inconsistent_state")); return; }
    if (!RealHumanReady(Human)) return;
    for (auto It=RetryAfter.CreateIterator();It;++It) if (!It.Key().IsValid()) It.RemoveCurrent();
    const double Now=Mode->GetWorld()->GetTimeSeconds();
    if (const double* Retry=RetryAfter.Find(Mode->GetWorld()); Retry && Now<*Retry) return;
    RetryAfter.Add(Mode->GetWorld(),Now+5);
    FMRFPSArenaLayout Layout;
    if (!SoloLayout(Human,GS->Layout,Layout)) { UE_LOG(LogMRFPSBot,Warning,TEXT("MRFPS_BOT_LAYOUT_UNAVAILABLE")); return; }
    FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Bot=Mode->GetWorld()->SpawnActor<AMRFPSBotController>(Params);
    auto* BotState=Bot?Bot->GetPlayerState<AMRFPSPlayerState>():nullptr;
    auto* Fighter=Mode->GetWorld()->SpawnActor<AMRFPSPawn>(Params);
    if (!Bot || !BotState || !Fighter) {
        if (Fighter) Fighter->Destroy(); if (Bot) Bot->Destroy();
        UE_LOG(LogMRFPSBot,Error,TEXT("MRFPS_BOT_AI_UNAVAILABLE reason=spawn")); return;
    }
    BotState->SetIsABot(true); BotState->SetPlayerName(TEXT("BOT")); BotState->Slot=1-Human->GetSlot();
    BotState->bAuthenticated=false; BotState->AuthExpiresAt=0; BotState->bAdmin=BotState->bInspector=false;
    if (Fighter->Haptics) Fighter->Haptics->bAutoConnectTracking=false;
    Fighter->bCalibrated=true; Fighter->bStageFloorReferenced=true;
    // Assign the state before choosing the endpoint so the same spawn policy
    // is used initially and at every later round.
    Fighter->SetPlayerState(BotState); FVector Foot;
    const bool SpawnClear=FindRoundSpawn(Mode->GetWorld(),Fighter,Layout,Foot);
    if (!SpawnClear) {
        Fighter->Destroy(); Bot->Destroy();
        UE_LOG(LogMRFPSBot,Warning,TEXT("MRFPS_BOT_SPAWN_BLOCKED")); return;
    }
    Fighter->Pose.Head=Foot+FVector(0,0,170); Fighter->SetActorLocation(Foot);
    if (!Bot->InitializeBot(Fighter,Human)) { Fighter->Destroy(); Bot->Destroy(); return; }
    GS->Layout=Layout; GS->bSharedFloorConfirmed=false; GS->bSoloBotSession=true; GS->bSoloGeometryReady=false;
    GS->SoloHumanSlot=Human->GetSlot(); GS->SoloHumanCalibration=Human->Pose.bDesktop?0:Human->GeometryCalibrationGeneration;
    Mode->ApplyBotLayoutChange(); Mode->ResetForFighterChange(); GS->ForceNetUpdate();
    RetryAfter.Remove(Mode->GetWorld());
    UE_LOG(LogMRFPSBot,Display,TEXT("MRFPS_BOT_SPAWN slot=%d"),BotState->Slot);
}
}
