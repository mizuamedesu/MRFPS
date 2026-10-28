#include "MRFPSGame.h"
#include "MRFPSAdminPanel.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSBuild,Log,All);
namespace
{
bool BuildSurface(UWorld* World,const FMRFPSArenaLayout& Layout,const FVector& Origin,const FVector& Direction,FVector& Surface,AActor*& Support)
{
    Support=nullptr;
    if(Origin.ContainsNaN()||Direction.ContainsNaN()||Origin.SizeSquared()>FMath::Square(25000.)||
        !FMath::IsNearlyEqual(Direction.SizeSquared(),1.,.02)) return false;
    FCollisionQueryParams Query;
    for(TActorIterator<AMRFPSPawn> It(World);It;++It) Query.AddIgnoredActor(*It);
    for(TActorIterator<AMRFPSBarrier> It(World);It;++It) if(It->bPlacementPreview) Query.AddIgnoredActor(*It);
    FHitResult Hit;
    if(World->LineTraceSingleByChannel(Hit,Origin,Origin+Direction*20000,ECC_GameTraceChannel1,Query) && Hit.ImpactNormal.Z>.5) {
        if(auto* Piece=Cast<AMRFPSBarrier>(Hit.GetActor());Piece && !Piece->bPlacementPreview) {
            Surface=Hit.ImpactPoint;Support=Piece;return true;
        }
    }
    if(Direction.Z>=-.001) return false;
    const double Distance=(Layout.Center.Z-Origin.Z)/Direction.Z;
    if(Distance<0||Distance>20000) return false;
    Surface=Origin+Direction*Distance;return true;
}
bool Unoccupied(UWorld* World,EMRFPSBuildKind Kind,const FTransform& Transform,AActor* Support)
{
    FCollisionQueryParams Query;
    if(Support) Query.AddIgnoredActor(Support);
    for(TActorIterator<AMRFPSBarrier> It(World);It;++It) {
        if(It->bPlacementPreview) { Query.AddIgnoredActor(*It);continue; }
        if(It->BuildKind==Kind && It->GetActorLocation().Equals(Transform.GetLocation(),1) &&
            FMath::Abs(FRotator::NormalizeAxis(It->GetActorRotation().Yaw-Transform.Rotator().Yaw))<1) return false;
    }
    return !World->OverlapBlockingTestByChannel(Transform.GetLocation(),Transform.GetRotation(),ECC_GameTraceChannel1,
        FCollisionShape::MakeBox(MRFPSBuild::Extent(Kind)-FVector(.5)),Query);
}
}

void AMRFPSBarrier::ApplyBuildKind()
{
    if(!MRFPSBuild::Valid(BuildKind)||!Collision||!Visual) return;
    const FVector E=MRFPSBuild::Extent(BuildKind);
    Collision->SetBoxExtent(E);
    Collision->SetCollisionEnabled(BuildKind==EMRFPSBuildKind::Stairs?ECollisionEnabled::NoCollision:ECollisionEnabled::QueryOnly);
    Visual->SetVisibility(BuildKind!=EMRFPSBuildKind::Stairs);
    if(BuildKind==EMRFPSBuildKind::Wall) {
        if(auto* Mesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/Meshes/SM_MRWall.SM_MRWall"))) { Visual->SetStaticMesh(Mesh);Visual->SetRelativeScale3D(FVector::OneVector); }
    } else {
        Visual->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
        Visual->SetRelativeScale3D(E/50);
        const TCHAR* Name=BuildKind==EMRFPSBuildKind::Crate?TEXT("Orange"):TEXT("Floor");
        if(auto* Material=LoadObject<UMaterialInterface>(nullptr,*FString::Printf(TEXT("/Game/Materials/M_%s.M_%s"),Name,Name))) Visual->SetMaterial(0,Material);
    }
    for(int32 I=0;I<BuildSteps.Num();++I) if(BuildSteps[I]) {
        auto* Step=BuildSteps[I].Get(); const bool Visible=BuildKind==EMRFPSBuildKind::Stairs;
        const float Height=(I+1)*12.5f;
        Step->SetVisibility(Visible);Step->SetCollisionEnabled(Visible?ECollisionEnabled::QueryOnly:ECollisionEnabled::NoCollision);
        Step->SetRelativeLocation(FVector(0,-75+(I+.5)*18.75,-50+Height*.5));
        Step->SetRelativeScale3D(FVector(1,.1875,Height/100));
        if(auto* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Trim.M_Trim"))) Step->SetMaterial(0,Material);
    }
}
void AMRFPSBarrier::SetPreviewColor(bool Valid)
{
    SetActorEnableCollision(false);
    auto* Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Preview.M_Preview"));
    if(!Base) return;
    const auto Tint=[this,Base,Valid](UStaticMeshComponent* Mesh) {
        for(int32 I=0;I<Mesh->GetNumMaterials();++I) {
            auto* Dynamic=Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(I));
            if(!Dynamic || Dynamic->Parent!=Base) { Dynamic=UMaterialInstanceDynamic::Create(Base,this);Mesh->SetMaterial(I,Dynamic); }
            Dynamic->SetVectorParameterValue(TEXT("Tint"),Valid?FLinearColor(.05,1,.55):FLinearColor(1,.08,.1));
        }
    };
    Tint(Visual);for(const auto& Step:BuildSteps) if(Step) Tint(Step.Get());
}

bool AMRFPSGameMode::PlaceAdminObject(AMRFPSPlayerController* Admin,EMRFPSBuildKind Kind,const FVector& Origin,const FVector& Direction,float Yaw,FString& Error)
{
    Error=TEXT("ADMIN_REQUIRED");const auto* GS=GetGameState<AMRFPSGameState>();
    if(!AdminAuthorized(Admin)||!GS) return false;
    FVector Surface;AActor* Support=nullptr;FTransform Transform;
    Error=TEXT("ADMIN_WALL_INVALID_RAY");
    if(!MRFPSBuild::Valid(Kind)||!BuildSurface(GetWorld(),GS->Layout,Origin,Direction,Surface,Support)) return false;
    Error=TEXT("ADMIN_WALL_OUTSIDE_LAYOUT");
    if(!MRFPSBuild::Snap(GS->Layout,Kind,Surface,Yaw,Transform)) return false;
    Error=TEXT("ADMIN_WALL_CAPACITY");int32 Count=0;
    for(TActorIterator<AMRFPSBarrier> It(GetWorld());It;++It) if(It->bAdminPlaced&&!It->bPlacementPreview) ++Count;
    if(Count>=MaxAdminWalls) return false;
    Error=TEXT("ADMIN_WALL_OCCUPIED");if(!Unoccupied(GetWorld(),Kind,Transform,Support)) return false;
    auto* Piece=GetWorld()->SpawnActorDeferred<AMRFPSBarrier>(AMRFPSBarrier::StaticClass(),Transform,Admin,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if(!Piece) { Error=TEXT("ADMIN_WALL_SPAWN_FAILED");return false; }
    Piece->bAdminPlaced=true;Piece->BuildKind=Kind;Piece->OwnerSlot=-1;
    UGameplayStatics::FinishSpawningActor(Piece,Transform);Piece->ApplyBuildKind();Piece->ForceNetUpdate();
    UE_LOG(LogMRFPSBuild,Display,TEXT("MRFPS_BUILD_PLACED kind=%d location=%s"),int32(Kind),*Transform.GetLocation().ToCompactString());
    Error.Empty();return true;
}
bool AMRFPSGameMode::RemoveAdminObject(AMRFPSPlayerController* Admin,const FVector& Origin,const FVector& Direction,FString& Error)
{
    Error=TEXT("ADMIN_REQUIRED");if(!AdminAuthorized(Admin)) return false;
    Error=TEXT("ADMIN_WALL_INVALID_RAY");
    if(Origin.ContainsNaN()||Direction.ContainsNaN()||Origin.SizeSquared()>FMath::Square(25000.)||!FMath::IsNearlyEqual(Direction.SizeSquared(),1.,.02)) return false;
    FCollisionQueryParams Query;for(TActorIterator<AMRFPSPawn> It(GetWorld());It;++It) Query.AddIgnoredActor(*It);
    FHitResult Hit;GetWorld()->LineTraceSingleByChannel(Hit,Origin,Origin+Direction*20000,ECC_GameTraceChannel1,Query);
    auto* Piece=Cast<AMRFPSBarrier>(Hit.GetActor());
    Error=TEXT("ADMIN_NO_OBJECT");if(!Piece||!Piece->bAdminPlaced||Piece->bPlacementPreview) return false;
    Piece->Destroy();Error.Empty();return true;
}

void AMRFPSPlayerController::ServerAdminPlaceObject_Implementation(EMRFPSBuildKind Kind,FVector_NetQuantize10 Origin,FVector_NetQuantizeNormal Direction,float Yaw,uint32 LayoutRevision,uint32 ControlRevision)
{
    auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();auto* GM=GetWorld()->GetAuthGameMode<AMRFPSGameMode>();
    if(!IsAdmin()||!GS||!GM) { ClientAdminResult(TEXT("ADMIN_REQUIRED"));return; }
    const double Now=GetWorld()->GetTimeSeconds();
    if(Now-LastAdminWallAt<.2) { ClientAdminResult(TEXT("ADMIN_RATE_LIMIT"));return; } LastAdminWallAt=Now;
    if(LayoutRevision!=GS->Layout.Revision||ControlRevision!=GS->ControlRevision) { ClientAdminResult(TEXT("ADMIN_STALE_STATE"));return; }
    FString Error;ClientAdminResult(GM->PlaceAdminObject(this,Kind,Origin,Direction,Yaw,Error)?FName(TEXT("ADMIN_WALL_PLACED")):FName(*Error));
}
void AMRFPSPlayerController::ServerAdminRemoveObject_Implementation(FVector_NetQuantize10 Origin,FVector_NetQuantizeNormal Direction,uint32 LayoutRevision,uint32 ControlRevision)
{
    auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();auto* GM=GetWorld()->GetAuthGameMode<AMRFPSGameMode>();
    if(!IsAdmin()||!GS||!GM) { ClientAdminResult(TEXT("ADMIN_REQUIRED"));return; }
    const double Now=GetWorld()->GetTimeSeconds();if(Now-LastAdminWallAt<.2) return;LastAdminWallAt=Now;
    if(LayoutRevision!=GS->Layout.Revision||ControlRevision!=GS->ControlRevision) { ClientAdminResult(TEXT("ADMIN_STALE_STATE"));return; }
    FString Error;ClientAdminResult(GM->RemoveAdminObject(this,Origin,Direction,Error)?FName(TEXT("ADMIN_OK")):FName(*Error));
}
void AMRFPSPlayerController::TickAdminPreview()
{
    if(!IsLocalController()||!bAdminWallPlacement||!IsAdmin()) { if(AdminPreview) { AdminPreview->Destroy();AdminPreview=nullptr; } return; }
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();FVector Origin,Direction,Surface;AActor* Support=nullptr;
    if(!GS||!DeprojectMousePositionToWorld(Origin,Direction)||!BuildSurface(GetWorld(),GS->Layout,Origin,Direction,Surface,Support)) {
        if(AdminPreview) AdminPreview->SetActorHiddenInGame(true);bAdminPreviewValid=false;return;
    }
    FTransform Transform;bAdminPreviewValid=MRFPSBuild::Snap(GS->Layout,AdminBuildKind,Surface,AdminWallYaw,Transform);
    if(!AdminPreview) {
        AdminPreview=GetWorld()->SpawnActorDeferred<AMRFPSBarrier>(AMRFPSBarrier::StaticClass(),Transform,this,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if(!AdminPreview) return;
        AdminPreview->bAdminPlaced=true;AdminPreview->bPlacementPreview=true;AdminPreview->SetReplicates(false);
        UGameplayStatics::FinishSpawningActor(AdminPreview,Transform);AdminPreview->SetActorEnableCollision(false);
    }
    if(AdminPreview->BuildKind!=AdminBuildKind) { AdminPreview->BuildKind=AdminBuildKind;AdminPreview->ApplyBuildKind(); }
    AdminPreview->SetActorHiddenInGame(false);AdminPreview->SetActorTransform(Transform);
    bAdminPreviewValid=bAdminPreviewValid&&Unoccupied(GetWorld(),AdminBuildKind,Transform,Support);
    AdminPreview->SetPreviewColor(bAdminPreviewValid);
    if(bConsumeAdminPlacementClick&&GetWorld()->GetTimeSeconds()>=NextAdminRepeatAt) PlaceSelectedBuild();
}
void AMRFPSPlayerController::PlaceSelectedBuild()
{
    if(!IsAdmin()||!bAdminWallPlacement) return;
    NextAdminRepeatAt=GetWorld()->GetTimeSeconds()+.22;
    FVector Origin,Direction;const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    if(GS&&DeprojectMousePositionToWorld(Origin,Direction)) ServerAdminPlaceObject(AdminBuildKind,Origin,Direction,AdminWallYaw,GS->Layout.Revision,GS->ControlRevision);
}
void AMRFPSPlayerController::RemoveSelectedBuild()
{
    FVector Origin,Direction;const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    if(IsAdmin()&&GS&&DeprojectMousePositionToWorld(Origin,Direction)) ServerAdminRemoveObject(Origin,Direction,GS->Layout.Revision,GS->ControlRevision);
}
FString AMRFPSPlayerController::AdminFeedback() const
{
    if(GetWorld()->GetTimeSeconds()-LastAdminFeedbackAt>2.5) return FString();
    if(AdminStatus==TEXT("ADMIN_WALL_PLACED")) return TEXT("設置しました");
    if(AdminStatus==TEXT("ADMIN_OK")) return TEXT("変更しました");
    if(AdminStatus==TEXT("ADMIN_WALL_OUTSIDE_LAYOUT")) return TEXT("範囲外です");
    if(AdminStatus==TEXT("ADMIN_WALL_OCCUPIED")) return TEXT("ここには置けません");
    if(AdminStatus==TEXT("ADMIN_WALL_CAPACITY")) return TEXT("設置数の上限です");
    if(AdminStatus==TEXT("ADMIN_PLAYERS_NOT_READY")) return TEXT("プレイヤーの準備待ち");
    if(AdminStatus==TEXT("LAYOUT_SHARED_BASIS_UNAVAILABLE")) return TEXT("先に原点を登録してください");
    if(AdminStatus==TEXT("LAYOUT_DIMENSIONS_INVALID")) return TEXT("サイズを確認してください");
    if(AdminStatus==TEXT("ADMIN_INVALID_STATE")) return TEXT("試合を一時停止してください");
    if(AdminStatus==TEXT("ADMIN_STALE_STATE")) return TEXT("もう一度操作してください");
    return FString();
}
