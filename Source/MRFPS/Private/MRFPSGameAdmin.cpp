#include "MRFPSGame.h"
#include "MRFPSAdminPanel.h"
#include "MRFPSMenu.h"
#include "MRFPSGeometryWorld.h"
#include "MRFPSHapticsComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"

namespace
{
bool LivePlayer(const AMRFPSPawn* Fighter)
{
    const auto* PS=Fighter?Fighter->GetPlayerState<AMRFPSPlayerState>():nullptr;
    return PS && PS->bAuthenticated && !PS->bInspector && !PS->bAdmin && PS->Slot>=0 && PS->Slot<2 &&
        PS->AuthExpiresAt>FDateTime::UtcNow().ToUnixTimestamp() && Fighter->bCalibrated &&
        Fighter->Pose.bHeadTracked && Fighter->Pose.IsSane() && Fighter->IsPoseFresh();
}
AMRFPSPawn* FighterAt(UWorld* World,int32 Slot)
{
    AMRFPSPawn* Found=nullptr;
    for (TActorIterator<AMRFPSPawn> It(World);It;++It) if (It->GetSlot()==Slot) {
        if (Found) return nullptr;
        Found=*It;
    }
    return Found;
}
bool BothReady(UWorld* World,bool& Nominal)
{
    const auto* GS=World->GetGameState<AMRFPSGameState>();
    auto* A=FighterAt(World,0); auto* B=FighterAt(World,1);
    Nominal=false;
    if (!GS || !LivePlayer(A) || !LivePlayer(B) || !A->IsInsideArena() || !B->IsInsideArena()) return false;
    Nominal=A->Pose.bDesktop && B->Pose.bDesktop;
    return Nominal || (!A->Pose.bDesktop && !B->Pose.bDesktop && GS->bSharedFloorConfirmed && GS->Layout.bCalibrated &&
        GS->SharedFloorRevision && GS->Layout.RegistrationRevision==GS->SharedFloorRevision);
}
}

void AMRFPSGameState::OnRepLayout()
{
    if (!GetWorld() || !Layout.IsSane()) return;
    if (auto* Geometry=GetWorld()->GetSubsystem<UMRFPSGeometryWorld>()) Geometry->ResetAll();
    for (FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
        if (auto* PC=Cast<AMRFPSPlayerController>(It->Get())) PC->ResetGeometryForLayout();
    for (TActorIterator<AMRFPSArena> It(GetWorld());It;++It) It->ApplyLayout(Layout);
}
void AMRFPSArena::ApplyLayout(const FMRFPSArenaLayout& Layout)
{
    if (!Layout.IsSane() || !Floor) return;
    SetActorLocationAndRotation(Layout.Center,FRotator(0,Layout.YawDegrees,0));
    Floor->SetRelativeScale3D(FVector(Layout.WidthCm/100,Layout.LengthCm/100,.03));
    for (int32 Slot=0;Slot<NominalStartMarkers.Num();++Slot) if (NominalStartMarkers[Slot])
        NominalStartMarkers[Slot]->SetRelativeLocation(FVector(0,Slot==0?-Layout.LengthCm*.5:Layout.LengthCm*.5,1));
    for (int32 I=0;I<GridLines.Num();++I) {
        if (!GridLines[I]) continue;
        const bool AlongY=I<21; const int32 N=I%21;
        const double Span=AlongY?Layout.WidthCm:Layout.LengthCm;
        const int32 Divisions=FMath::CeilToInt(Span/100);
        const bool Visible=N<=Divisions;
        GridLines[I]->SetVisibility(Visible);
        if (!Visible) continue;
        const bool Edge=N==0 || N==Divisions;
        const double Coordinate=N==Divisions?Span*.5:N*100-Span*.5;
        const double Thickness=Edge?.035:.008;
        GridLines[I]->SetRelativeScale3D(AlongY?FVector(Thickness,Layout.LengthCm/100,.008):FVector(Layout.WidthCm/100,Thickness,.008));
        GridLines[I]->SetRelativeLocation(AlongY?FVector(Coordinate,0,.5):FVector(0,Coordinate,.5));
    }
}

void AMRFPSPawn::ClientCombatStopped_Implementation()
{
    bFiring=false; bFingerTriggerDown=false; bDesktopShield=false; bRequireFireRelease=true;
    if (Haptics) Haptics->StopHaptics();
}
void AMRFPSPawn::OffsetCombatTimers(double Seconds)
{
    if (!HasAuthority() || !FMath::IsFinite(Seconds) || Seconds<=0) return;
    if (RespawnAt>0) RespawnAt+=Seconds;
    if (WallReadyAt>0) WallReadyAt+=Seconds;
    LastServerFire+=Seconds; LastShieldUse+=Seconds; ServerRise.Reset(); ServerLower.Reset(); ForceNetUpdate();
}
void AMRFPSPawn::ClientNominalDesktopStart_Implementation(FVector_NetQuantize10 Head,FRotator Rotation)
{
    // A nominal desktop start is synthetic game input. It never relocates a
    // measured XR pose or changes the tracking-to-common-world transform.
    if (IsXR()) return;
    DesktopHead=Head; DesktopRotation=Rotation;
}

bool AMRFPSPlayerController::IsAdmin() const
{
    const auto* PS=GetPlayerState<AMRFPSPlayerState>();
    return PS && PS->bAuthenticated && PS->bInspector && PS->bAdmin && PS->Slot==-1 &&
        PS->AuthExpiresAt>FDateTime::UtcNow().ToUnixTimestamp();
}
void AMRFPSPlayerController::ToggleAdminPanel()
{
    if (!IsLocalController() || !IsAdmin()) return;
    if (AdminPanel && AdminPanel->IsInViewport()) { AdminPanel->Close(); return; }
    if (SessionMenu && SessionMenu->IsInViewport()) SessionMenu->Close();
    SetAdminWallPlacement(false); bAdminPanelWasReleased=bInspectorMouseReleased;
    AdminPanel=UMRFPSAdminPanel::CreateFor(this);
}
void AMRFPSPlayerController::AdminPanelClosed()
{ if (IsLocalController() && IsInspector()) SetInspectorMouseReleased(bAdminPanelWasReleased); }
void AMRFPSPlayerController::SetAdminWallPlacement(bool Enabled)
{
    if (!IsLocalController()) return;
    const bool WasEnabled=bAdminWallPlacement;
    bAdminWallPlacement=Enabled && IsAdmin();
    if(!bAdminWallPlacement) {
        bConsumeAdminPlacementClick=false;
        if(AdminPreview) { AdminPreview->Destroy();AdminPreview=nullptr; }
        if(AdminCellPreview) { AdminCellPreview->DestroyComponent();AdminCellPreview=nullptr; }
    }
    if (WasEnabled && !bAdminWallPlacement) bMultiView=bAdminPlacementWasMultiView;
    if (bAdminWallPlacement) {
        if (!WasEnabled) bAdminPlacementWasMultiView=bMultiView;
        bMultiView=false; // No video tiles may obscure the 3D picking surface.
        if (AdminPanel && AdminPanel->IsInViewport()) AdminPanel->Close();
        FreeView(); SetInspectorMouseReleased(true);
        AdminStatus.Empty();
    }
}
void AMRFPSPlayerController::RequestAdminAction(EMRFPSAdminAction Action,FVector2D Dimensions)
{
    if (!IsLocalController() || !IsAdmin()) { AdminStatus=TEXT("ADMIN_REQUIRED"); return; }
    if (const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>())
        ServerAdminAction(Action,Dimensions,GS->Layout.Revision,GS->ControlRevision);
}
void AMRFPSPlayerController::ServerAdminAction_Implementation(EMRFPSAdminAction Action,FVector2D Dimensions,uint32 ExpectedLayoutRevision,uint32 ExpectedControlRevision)
{
    FString Error=TEXT("ADMIN_REQUIRED");
    auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    auto* GM=GetWorld()->GetAuthGameMode<AMRFPSGameMode>();
    const double Now=GetWorld()->GetTimeSeconds();
    if (!IsAdmin() || !GM || !GS) { ClientAdminResult(FName(*Error)); return; }
    if (Now-LastAdminActionAt<.2) { ClientAdminResult(TEXT("ADMIN_RATE_LIMIT")); return; }
    LastAdminActionAt=Now;
    if (ExpectedLayoutRevision!=GS->Layout.Revision || ExpectedControlRevision!=GS->ControlRevision) {
        ClientAdminResult(TEXT("ADMIN_STALE_STATE")); return;
    }
    if (uint8(Action)>uint8(EMRFPSAdminAction::ClearAdminWalls) || Dimensions.ContainsNaN()) {
        ClientAdminResult(TEXT("ADMIN_INVALID_ACTION")); return;
    }
    const bool Accepted=GM->HandleAdminAction(this,Action,Dimensions,Error);
    ClientAdminResult(Accepted?FName(TEXT("ADMIN_OK")):FName(*Error));
}
void AMRFPSPlayerController::ServerAdminPlaceWall_Implementation(FVector_NetQuantize10 Origin,FVector_NetQuantizeNormal Direction,float Yaw,uint32 ExpectedLayoutRevision,uint32 ExpectedControlRevision)
{
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>(); auto* GM=GetWorld()->GetAuthGameMode<AMRFPSGameMode>();
    const double Now=GetWorld()->GetTimeSeconds();
    if (!IsAdmin() || !GS || !GM) { ClientAdminResult(TEXT("ADMIN_REQUIRED")); return; }
    if (Now-LastAdminWallAt<.25) { ClientAdminResult(TEXT("ADMIN_RATE_LIMIT")); return; }
    LastAdminWallAt=Now;
    if (ExpectedLayoutRevision!=GS->Layout.Revision || ExpectedControlRevision!=GS->ControlRevision) { ClientAdminResult(TEXT("ADMIN_STALE_STATE")); return; }
    FString Error;
    ClientAdminResult(GM->PlaceAdminWall(this,Origin,Direction,Yaw,Error)?FName(TEXT("ADMIN_WALL_PLACED")):FName(*Error));
}
void AMRFPSPlayerController::ClientAdminResult_Implementation(FName SafeCode)
{ SetAdminFeedback(SafeCode); }
void AMRFPSPlayerController::SetAdminFeedback(FName Code)
{ AdminStatus=Code.ToString();LastAdminFeedbackAt=GetWorld()->GetTimeSeconds(); }

bool AMRFPSGameMode::AdminAuthorized(const AMRFPSPlayerController* Admin) const
{ return HasAuthority() && Admin && Admin->GetWorld()==GetWorld() && Admin->HasAuthority() && Admin->IsAdmin(); }
bool AMRFPSGameMode::RegisteredAnchorsValid() const
{
    const auto* GS=GetGameState<AMRFPSGameState>();
    if (!GS || !GS->bSharedFloorConfirmed || !GS->SharedFloorRevision) return false;
    for (int32 Slot=0;Slot<2;++Slot) {
        const auto* Fighter=RegisteredPlayers[Slot].Get();
        if (!LivePlayer(Fighter) || Fighter!=FighterAt(GetWorld(),Slot) || Fighter->Pose.bDesktop || !Fighter->bStageFloorReferenced ||
            Fighter->GeometryCalibrationGeneration!=RegisteredCalibration[Slot]) return false;
    }
    return true;
}
bool AMRFPSGameMode::CaptureSharedFloorRegistration(FString& SafeError)
{
    auto* GS=GetGameState<AMRFPSGameState>();
    SafeError=TEXT("LAYOUT_SHARED_BASIS_UNAVAILABLE");
    if (!GS || GS->SharedFloorRevision==MAX_uint32) return false;
    AMRFPSPawn* Fighters[2]={FighterAt(GetWorld(),0),FighterAt(GetWorld(),1)};
    for (auto* Fighter:Fighters) if (!LivePlayer(Fighter) || Fighter->Pose.bDesktop || !Fighter->bStageFloorReferenced || !Fighter->GeometryCalibrationGeneration) return false;
    // This is the operator's explicit confirmation of the existing manual
    // common-origin/+X procedure and Stage floor, not automatic spatial-anchor
    // registration. Stage floor is z=0 in that common basis; no head-Z average.
    for (int32 Slot=0;Slot<2;++Slot) { RegisteredPlayers[Slot]=Fighters[Slot]; RegisteredCalibration[Slot]=Fighters[Slot]->GeometryCalibrationGeneration; }
    ++GS->SharedFloorRevision; GS->bSharedFloorConfirmed=true;
    SafeError.Empty(); return true;
}
void AMRFPSGameMode::InvalidateSharedFloor()
{
    auto* GS=GetGameState<AMRFPSGameState>(); if (!GS) return;
    if (!GS->bSharedFloorConfirmed && !GS->Layout.bCalibrated) return;
    GS->bSharedFloorConfirmed=false; GS->Layout.bCalibrated=false; GS->Layout.RegistrationRevision=0; GS->MeasuredAnchorDistanceCm=0;
    if (GS->Layout.Revision<MAX_uint32) ++GS->Layout.Revision;
    for (int32 Slot=0;Slot<2;++Slot) { RegisteredPlayers[Slot].Reset(); RegisteredCalibration[Slot]=0; }
    if (GS->Phase==EMRFPSMatchPhase::Playing) SetCombatPaused(true);
    ApplyLayoutChange(); // retains its last centre, orientation, size and floor height
}
void AMRFPSGameMode::ApplyLayoutChange()
{
    auto* GS=GetGameState<AMRFPSGameState>(); if (!GS) return;
    GS->OnRepLayout(); GS->ForceNetUpdate();
    for (TActorIterator<AMRFPSBarrier> It(GetWorld());It;++It) {
        auto* Piece=*It;
        if (!Piece->bAdminPlaced || !Piece->bMapAnchor) { Piece->Destroy(); continue; }
        const FTransform Transform(FRotator(0,GS->Layout.YawDegrees+Piece->MapLocalYaw,0),
            GS->Layout.ToWorld(Piece->MapLocalPosition));
        const bool Fits=Piece->BuildKind==EMRFPSBuildKind::Wall
            ? MRFPSBuild::FitsWall(GS->Layout,Transform,Piece->WallShape)
            : MRFPSBuild::Fits(GS->Layout,Piece->BuildKind,Transform);
        if (!Fits) { Piece->Destroy(); continue; }
        Piece->SetActorTransform(Transform,false,nullptr,ETeleportType::TeleportPhysics);
        Piece->ForceNetUpdate();
    }
    PausedWallLifetimes.Reset();
}
void AMRFPSGameMode::SetCombatPaused(bool Paused)
{
    auto* GS=GetGameState<AMRFPSGameState>(); if (!GS || GS->bCombatPaused==Paused) return;
    const double Now=GetWorld()->GetTimeSeconds();
    if (Paused) {
        CombatPausedAt=Now; PausedWallLifetimes.Reset();
        for (TActorIterator<AMRFPSBarrier> It(GetWorld());It;++It) {
            const float Remaining=It->GetLifeSpan();
            if (Remaining>0) { PausedWallLifetimes.Add(*It,Remaining); It->SetLifeSpan(0); }
        }
        for (TActorIterator<AMRFPSPawn> It(GetWorld());It;++It) { It->bShieldActive=false; It->ClientCombatStopped(); }
    } else {
        const double Duration=FMath::Max(0.,Now-CombatPausedAt);
        for (TActorIterator<AMRFPSPawn> It(GetWorld());It;++It) { It->OffsetCombatTimers(Duration); It->ClientCombatStopped(); }
        if (GS->RestartAt>0) GS->RestartAt+=Duration;
        for (const auto& Pair:PausedWallLifetimes) if (auto* Wall=Pair.Key.Get()) Wall->SetLifeSpan(Pair.Value);
        PausedWallLifetimes.Reset();
    }
    GS->bCombatPaused=Paused; GS->ForceNetUpdate();
}
void AMRFPSGameMode::ResetToWaiting()
{
    SetCombatPaused(false);
    if (GameState) for (APlayerState* Base:GameState->PlayerArray) if (auto* PS=Cast<AMRFPSPlayerState>(Base)) { PS->Kills=0; PS->Deaths=0; PS->ForceNetUpdate(); }
    for (TActorIterator<AMRFPSPawn> It(GetWorld());It;++It) { It->ClientCombatStopped(); It->Respawn(); }
    for (TActorIterator<AMRFPSBarrier> It(GetWorld());It;++It) if (!It->bAdminPlaced) It->Destroy();
    PausedWallLifetimes.Reset();
    if (auto* GS=GetGameState<AMRFPSGameState>()) { GS->Phase=EMRFPSMatchPhase::Waiting; GS->WinnerSlot=-1; GS->RestartAt=0; GS->bNominalDesktopSession=false; GS->ForceNetUpdate(); }
}
bool AMRFPSGameMode::HandleAdminAction(AMRFPSPlayerController* Admin,EMRFPSAdminAction Action,FVector2D Dimensions,FString& SafeError)
{
    SafeError=TEXT("ADMIN_REQUIRED");
    auto* GS=GetGameState<AMRFPSGameState>();
    if (!AdminAuthorized(Admin) || !GS) return false;
    SafeError=TEXT("ADMIN_INVALID_STATE");
    if (GS->ControlRevision==MAX_uint32) return false;
    const bool Editable=GS->Phase!=EMRFPSMatchPhase::Playing || GS->bCombatPaused;
    switch (Action) {
    case EMRFPSAdminAction::Start: {
        bool Nominal=false;
        if (GS->Phase!=EMRFPSMatchPhase::Waiting) return false;
        SafeError=TEXT("ADMIN_PLAYERS_NOT_READY");
        if (!BothReady(GetWorld(),Nominal) || (!Nominal && !RegisteredAnchorsValid())) return false;
        GS->bNominalDesktopSession=Nominal; ResetMatch(); break;
    }
    case EMRFPSAdminAction::Pause:
        if (GS->Phase!=EMRFPSMatchPhase::Playing || GS->bCombatPaused) return false;
        SetCombatPaused(true); break;
    case EMRFPSAdminAction::Resume: {
        bool Nominal=false;
        if (GS->Phase!=EMRFPSMatchPhase::Playing || !GS->bCombatPaused) return false;
        SafeError=TEXT("ADMIN_PLAYERS_NOT_READY");
        if (!BothReady(GetWorld(),Nominal) || (!Nominal && !RegisteredAnchorsValid())) return false;
        GS->bNominalDesktopSession=Nominal; SetCombatPaused(false); break;
    }
    case EMRFPSAdminAction::Reset: ResetToWaiting(); break;
    case EMRFPSAdminAction::ConfirmSharedFloor:
        if (!Editable || GS->Layout.Revision==MAX_uint32 || !CaptureSharedFloorRegistration(SafeError)) return false;
        GS->Layout.bCalibrated=false; GS->Layout.RegistrationRevision=0;
        ++GS->Layout.Revision; ApplyLayoutChange(); break;
    case EMRFPSAdminAction::CalibrateFloor: {
        if (!Editable || !RegisteredAnchorsValid()) { SafeError=TEXT("LAYOUT_SHARED_BASIS_UNAVAILABLE"); return false; }
        auto* A=RegisteredPlayers[0].Get(); auto* B=RegisteredPlayers[1].Get();
        if (!GS->Layout.CalibrateFromAnchors(A->Pose.Head,B->Pose.Head,0,GS->SharedFloorRevision,SafeError)) return false;
        GS->MeasuredAnchorDistanceCm=FVector::Dist2D(A->Pose.Head,B->Pose.Head); ApplyLayoutChange(); break;
    }
    case EMRFPSAdminAction::SetDimensions:
        if (!Editable || !GS->Layout.SetDimensions(Dimensions.X,Dimensions.Y,SafeError)) return false;
        ArenaWidthCm=GS->Layout.WidthCm; ArenaLengthCm=GS->Layout.LengthCm; ApplyLayoutChange(); break;
    case EMRFPSAdminAction::ClearAdminWalls:
        for (TActorIterator<AMRFPSBarrier> It(GetWorld());It;++It) if (It->bAdminPlaced) It->Destroy();
        break;
    default: SafeError=TEXT("ADMIN_INVALID_ACTION"); return false;
    }
    ++GS->ControlRevision; GS->ForceNetUpdate(); SafeError.Empty(); return true;
}
bool AMRFPSGameMode::PlaceAdminWall(AMRFPSPlayerController* Admin,const FVector& Origin,const FVector& Direction,float Yaw,FString& SafeError)
{ return PlaceAdminObject(Admin,EMRFPSBuildKind::Wall,Origin,Direction,Yaw,SafeError); }
