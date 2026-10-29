#include "MRFPSGeometryWorld.h"
#include "MRFPSGame.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#if !UE_SERVER
#include "RHI.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#endif

struct FMRFPSGeometryWorldState
{
    struct FSlot
    {
        TWeakObjectPtr<AMRFPSPawn> Owner;
        FMRFPSGeometryChunk Header;
        FMRFPSGeometryMesh Mesh;
        double ExpiresAt=0;
        bool bActive=false;
        bool bRetired=false;
    };
    FSlot Slots[2];
};
namespace
{
double ServerSeconds(const UWorld* World)
{
    if (const auto* GS=World->GetGameState()) return GS->GetServerWorldTimeSeconds();
    return World->GetTimeSeconds();
}
}
bool UMRFPSGeometryWorld::DoesSupportWorldType(EWorldType::Type Type) const
{ return Type==EWorldType::Game || Type==EWorldType::PIE; }
void UMRFPSGeometryWorld::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection); State=MakeShared<FMRFPSGeometryWorldState>(); RenderActors.SetNum(2);
}
void UMRFPSGeometryWorld::Deinitialize()
{ ResetAll(); State.Reset(); Super::Deinitialize(); }
TStatId UMRFPSGeometryWorld::GetStatId() const
{ RETURN_QUICK_DECLARE_CYCLE_STAT(UMRFPSGeometryWorld,STATGROUP_Tickables); }
bool UMRFPSGeometryWorld::OwnerIdentityValid(int32 Slot) const
{
    if (!State || Slot<0 || Slot>=2) return false;
    const auto& S=State->Slots[Slot]; const auto* Pawn=S.Owner.Get();
    const auto* PS=Pawn?Pawn->GetPlayerState<AMRFPSPlayerState>():nullptr;
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    const bool Shared=GS && GS->bSharedFloorConfirmed && GS->Layout.bCalibrated && GS->SharedFloorRevision &&
        GS->Layout.RegistrationRevision==GS->SharedFloorRevision;
    const bool Solo=GS && Pawn && GS->bSoloBotSession && GS->SoloHumanSlot==Slot && GS->SoloHumanCalibration &&
        Pawn->bStageFloorReferenced && Pawn->GeometryCalibrationGeneration==GS->SoloHumanCalibration;
    return PS && Pawn->GetWorld()==GetWorld() && PS->bAuthenticated && !PS->bInspector && PS->Slot==Slot &&
        PS->AuthExpiresAt>FDateTime::UtcNow().ToUnixTimestamp() && PS->GeometryStreamId==S.Header.StreamId &&
        Pawn->bCalibrated && Pawn->GeometryCalibrationGeneration==S.Header.Calibration && GS && (Shared || Solo) &&
        GS->Layout.Revision==S.Header.LayoutRevision;
}
bool UMRFPSGeometryWorld::OwnerValid(int32 Slot) const
{
    if (!OwnerIdentityValid(Slot)) return false;
    const auto* Pawn=State->Slots[Slot].Owner.Get();
    if (!Pawn->Pose.IsSane() || !Pawn->IsInsideArena() || Pawn->Pose.bDesktop) return false;
    return GetWorld()->GetNetMode()==NM_Client || Pawn->IsPoseFresh();
}
void UMRFPSGeometryWorld::ClearSlot(int32 Slot,bool Retire)
{
    if (!State || Slot<0 || Slot>=2) return;
    auto& S=State->Slots[Slot];
    if (Retire && GetWorld()->GetNetMode()!=NM_Client) if (auto* Owner=S.Owner.Get()) {
        if (auto* PS=Owner->GetPlayerState<AMRFPSPlayerState>(); PS && PS->GeometryStreamId==S.Header.StreamId) {
            PS->GeometryStreamId=0; PS->ForceNetUpdate();
        }
    }
    S.bActive=false; S.bRetired=S.bRetired || Retire; S.Mesh=FMRFPSGeometryMesh();
    if (Retire) S.Owner.Reset();
    // Retain the closed server generation/sequence as a late-packet fence.
    if (RenderActors.IsValidIndex(Slot) && RenderActors[Slot]) { RenderActors[Slot]->Destroy(); RenderActors[Slot]=nullptr; }
}
void UMRFPSGeometryWorld::RemoveOwner(AMRFPSPawn* Owner)
{
    if (!State || !Owner) return;
    for (int32 Slot=0;Slot<2;++Slot) if (State->Slots[Slot].Owner.Get()==Owner) ClearSlot(Slot);
}
void UMRFPSGeometryWorld::ResetAll()
{
    for (int32 Slot=0;Slot<2;++Slot) ClearSlot(Slot);
    if (State) for (auto& Slot:State->Slots) Slot=FMRFPSGeometryWorldState::FSlot();
}
bool UMRFPSGeometryWorld::Commit(AMRFPSPawn* Owner,const FMRFPSGeometryChunk& Header,FMRFPSGeometryMesh Mesh)
{
    if (!IsInGameThread() || !State || !Owner || Header.Slot>=2 || !Header.StreamId || !Header.Calibration || !Header.Sequence || !Mesh.IsSane() ||
        Mesh.Vertices.Num()!=Header.VertexCount || Mesh.Indices.Num()!=Header.TriangleCount*3 ||
        Header.TotalBytes!=6*(int32(Header.VertexCount)+int32(Header.TriangleCount)) ||
        !MRFPSGeometryWire::CaptureFresh(Header.CaptureMs,ServerSeconds(GetWorld()),MRFPSGeometryWire::SnapshotLifetime)) return false;
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    if (!GS || Header.LayoutRevision!=GS->Layout.Revision) return false;
    for (const FVector& Point:Mesh.Vertices) if (!GS->Layout.ContainsGeometry(Point)) return false;
    auto& S=State->Slots[Header.Slot];
    if (Header.StreamId<S.Header.StreamId || (Header.StreamId==S.Header.StreamId &&
        (S.bRetired || S.Owner.Get()!=Owner || Header.Sequence<=S.Header.Sequence))) return false;
    const uint32 NowMs=uint32(uint64(ServerSeconds(GetWorld())*1000));
    const double Age=FMath::Max(0.0,double(int32(NowMs-Header.CaptureMs))*.001);
    if (Age>=MRFPSGeometryWire::SnapshotLifetime) return false;
    auto Old=MoveTemp(S); S.Owner=Owner; S.Header=Header; S.Header.Payload.Reset();
    if (!OwnerValid(Header.Slot)) { S=MoveTemp(Old); return false; }
    S.Mesh=MoveTemp(Mesh); S.ExpiresAt=FPlatformTime::Seconds()+MRFPSGeometryWire::SnapshotLifetime-Age; S.bActive=true; S.bRetired=false;
    UpdateVisual(Header.Slot); return true;
}
void UMRFPSGeometryWorld::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime); if (!State) return;
    if (GetWorld()->GetNetMode()==NM_Client) if (const auto* Viewer=Cast<AMRFPSPlayerController>(GetWorld()->GetFirstPlayerController())) {
        const auto* PS=Viewer->GetPlayerState<AMRFPSPlayerState>();
        if (!PS || !PS->bAuthenticated || PS->AuthExpiresAt<=FDateTime::UtcNow().ToUnixTimestamp() ||
            (!PS->bInspector && (PS->Slot<0 || PS->Slot>=2))) { ResetAll(); return; }
    }
    const double Now=FPlatformTime::Seconds();
    for (int32 Slot=0;Slot<2;++Slot) if (State->Slots[Slot].bActive &&
        (Now>=State->Slots[Slot].ExpiresAt || !OwnerValid(Slot))) {
        // A receiver may lose packets while the server still has fresh data.
        // Hide its expired mesh, retaining owner/sequence fences so a newer
        // snapshot in that same authoritative stream can recover. Only the
        // authority or changed ownership/calibration retires the generation.
        const bool Retire=GetWorld()->GetNetMode()!=NM_Client || !OwnerIdentityValid(Slot);
        ClearSlot(Slot,Retire);
    }
}
bool UMRFPSGeometryWorld::Trace(const FVector& Start,const FVector& End,FVector& HitPoint) const
{
    return GetWorld()->GetNetMode()!=NM_Client && TraceMeasuredSurface(Start,End,HitPoint);
}
bool UMRFPSGeometryWorld::HasLiveMeasurement(int32 Slot) const
{
    if (!State || Slot<0 || Slot>=2) return false;
    const auto& S=State->Slots[Slot];
    return S.bActive && FPlatformTime::Seconds()<S.ExpiresAt && OwnerValid(Slot) && !S.Mesh.Indices.IsEmpty();
}
bool UMRFPSGeometryWorld::OverlapsMeasuredBounds(const FBox& Bounds) const
{
    if (!GetWorld() || GetWorld()->GetNetMode()==NM_Client || !Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN() ||
        Bounds.Min.X>Bounds.Max.X || Bounds.Min.Y>Bounds.Max.Y || Bounds.Min.Z>Bounds.Max.Z || !State) return true;
    const double Now=FPlatformTime::Seconds();
    for (int32 Slot=0;Slot<2;++Slot) {
        const auto& S=State->Slots[Slot];
        if (!S.bActive || Now>=S.ExpiresAt || !OwnerValid(Slot)) continue;
        for (int32 I=0;I<S.Mesh.Indices.Num();I+=3) {
            if (!S.Mesh.Indices.IsValidIndex(I+2) || !S.Mesh.Vertices.IsValidIndex(S.Mesh.Indices[I]) ||
                !S.Mesh.Vertices.IsValidIndex(S.Mesh.Indices[I+1]) || !S.Mesh.Vertices.IsValidIndex(S.Mesh.Indices[I+2])) return true;
            FBox Triangle(ForceInit);
            Triangle+=S.Mesh.Vertices[S.Mesh.Indices[I]];
            Triangle+=S.Mesh.Vertices[S.Mesh.Indices[I+1]];
            Triangle+=S.Mesh.Vertices[S.Mesh.Indices[I+2]];
            if (Bounds.Intersect(Triangle)) return true;
        }
    }
    return false;
}
bool UMRFPSGeometryWorld::SampleAvatarGround(const FVector& Probe,float& GroundZ) const
{
    FVector Hit;
    if(Probe.ContainsNaN() || !TraceMeasuredSurface(Probe+FVector(0,0,70),Probe-FVector(0,0,70),Hit)) return false;
    GroundZ=Hit.Z;return true;
}
bool UMRFPSGeometryWorld::TraceMeasuredSurface(const FVector& Start,const FVector& End,FVector& HitPoint) const
{
    if(!State || Start.ContainsNaN() || End.ContainsNaN()) return false;
    bool Found=false; double Best=1; const double Now=FPlatformTime::Seconds();
    for (int32 Slot=0;Slot<2;++Slot) {
        const auto& S=State->Slots[Slot]; if (!S.bActive || Now>=S.ExpiresAt || !OwnerValid(Slot)) continue;
        double Fraction=1; FVector Point;
        if (S.Mesh.Trace(Start,End,Fraction,Point) && Fraction<=Best) { Best=Fraction; HitPoint=Point; Found=true; }
    }
    return Found;
}
FString UMRFPSGeometryWorld::GetSafeStatus() const
{
    int32 Surfaces=0;
    if (State) for (int32 Slot=0;Slot<2;++Slot) if (State->Slots[Slot].bActive &&
        FPlatformTime::Seconds()<State->Slots[Slot].ExpiresAt && OwnerValid(Slot)) Surfaces+=State->Slots[Slot].Mesh.Indices.Num()/3;
    return Surfaces ? FString::Printf(TEXT("Live measured surfaces: %d triangles"),Surfaces) : TEXT("No live measured geometry");
}
void UMRFPSGeometryWorld::UpdateVisual(int32 Slot)
{
#if !UE_SERVER
    if (!FApp::CanEverRender() || GUsingNullRHI || !GDynamicRHI || GetWorld()->GetNetMode()!=NM_Client) return;
    const auto* PC=Cast<AMRFPSPlayerController>(GetWorld()->GetFirstPlayerController());
    if (!PC || !PC->IsInspector()) return;
    AActor* Actor=RenderActors[Slot];
    if (!Actor) {
        FActorSpawnParameters Params; Params.ObjectFlags|=RF_Transient;
        Actor=GetWorld()->SpawnActor<AActor>(Params); if (!Actor) return;
        auto* Mesh=NewObject<UProceduralMeshComponent>(Actor); Actor->SetRootComponent(Mesh);
        Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision); Mesh->SetGenerateOverlapEvents(false);
        Mesh->SetCastShadow(false); Mesh->bUseAsyncCooking=false; Mesh->RegisterComponent();
        if (auto* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Wall.M_Wall"))) Mesh->SetMaterial(0,Material);
        RenderActors[Slot]=Actor;
    }
    if (auto* Mesh=Cast<UProceduralMeshComponent>(Actor->GetRootComponent())) {
        const auto& Data=State->Slots[Slot].Mesh;
        const TArray<FVector> Normals; const TArray<FVector2D> UV; const TArray<FColor> Colors; const TArray<FProcMeshTangent> Tangents;
        Mesh->CreateMeshSection(0,Data.Vertices,Data.Indices,Normals,UV,Colors,Tangents,false);
    }
#endif
}
