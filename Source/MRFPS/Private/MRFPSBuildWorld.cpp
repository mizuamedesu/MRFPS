#include "MRFPSGame.h"
#include "MRFPSAdminPanel.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSBuild,Log,All);
namespace
{
bool ValidBuildRay(const FVector& Origin,const FVector& Direction)
{
    return !Origin.ContainsNaN() && !Direction.ContainsNaN() && Origin.SizeSquared()<=FMath::Square(25000.) &&
        FMath::IsNearlyEqual(Direction.SizeSquared(),1.,.02);
}
struct FWallPick
{
    AMRFPSBarrier* Wall=nullptr;
    int32 Column=0,Row=0;
    double Distance=0;
};

// Intersect the wall's editing plane, not only its solid collider. An open
// cell must remain selectable so that the admin can restore it later.
bool PickAdminWall(UWorld* World,const FVector& Origin,const FVector& Direction,FWallPick& Pick)
{
    if(!World || !ValidBuildRay(Origin,Direction)) return false;
    double Nearest=20000.;
    for(TActorIterator<AMRFPSBarrier> It(World);It;++It) {
        auto* Wall=*It;
        if(!Wall->bAdminPlaced || Wall->bPlacementPreview || Wall->BuildKind!=EMRFPSBuildKind::Wall || !Wall->WallShape.IsSane()) continue;
        const FTransform Transform=Wall->GetActorTransform();
        const FVector LocalOrigin=Transform.InverseTransformPosition(Origin);
        const FVector LocalDirection=Transform.InverseTransformVectorNoScale(Direction);
        if(FMath::Abs(LocalDirection.X)<.00001) continue;
        const double Distance=-LocalOrigin.X/LocalDirection.X;
        if(Distance<0 || Distance>=Nearest) continue;
        const FVector Hit=LocalOrigin+LocalDirection*Distance;
        const double Height=Wall->WallShape.HeightRows*MRFPSBuild::WallRowHeightCm;
        if(Hit.Y< -MRFPSBuild::WallWidthCm*.5 || Hit.Y>=MRFPSBuild::WallWidthCm*.5 || Hit.Z< -100 || Hit.Z>=-100+Height) continue;
        Pick.Wall=Wall;Pick.Distance=Nearest=Distance;
        Pick.Column=FMath::Clamp(int32((Hit.Y+MRFPSBuild::WallWidthCm*.5)/(MRFPSBuild::WallWidthCm/MRFPSBuild::WallColumns)),0,MRFPSBuild::WallColumns-1);
        Pick.Row=FMath::Clamp(int32((Hit.Z+100)/MRFPSBuild::WallRowHeightCm),0,int32(Wall->WallShape.HeightRows)-1);
    }
    if(!Pick.Wall) return false;
    FCollisionQueryParams Query;Query.AddIgnoredActor(Pick.Wall);
    for(TActorIterator<AMRFPSPawn> It(World);It;++It) Query.AddIgnoredActor(*It);
    for(TActorIterator<AMRFPSBarrier> It(World);It;++It) if(It->bPlacementPreview) Query.AddIgnoredActor(*It);
    FHitResult Obstruction;
    return !World->LineTraceSingleByChannel(Obstruction,Origin,Origin+Direction*FMath::Max(0.,Pick.Distance-.5),ECC_GameTraceChannel1,Query);
}

AMRFPSBarrier* PickAdminCrate(UWorld* World,const FVector& Origin,const FVector& Direction)
{
    if (!World || !ValidBuildRay(Origin,Direction)) return nullptr;
    FCollisionQueryParams Query;
    for (TActorIterator<AMRFPSPawn> It(World);It;++It) Query.AddIgnoredActor(*It);
    for (TActorIterator<AMRFPSBarrier> It(World);It;++It) if (It->bPlacementPreview) Query.AddIgnoredActor(*It);
    FHitResult Hit;
    if (!World->LineTraceSingleByChannel(Hit,Origin,Origin+Direction*20000,ECC_GameTraceChannel1,Query)) return nullptr;
    auto* Piece=Cast<AMRFPSBarrier>(Hit.GetActor());
    return Piece && Piece->bAdminPlaced && !Piece->bPlacementPreview && Piece->BuildKind==EMRFPSBuildKind::Crate?Piece:nullptr;
}

bool AddedCellsAreClear(UWorld* World,AMRFPSBarrier* Wall,const FMRFPSWallShape& Next)
{
    if(!World || !Wall || !Next.IsSane()) return false;
    FCollisionQueryParams Query;Query.AddIgnoredActor(Wall);
    for(TActorIterator<AMRFPSBarrier> It(World);It;++It) if(It->bPlacementPreview) Query.AddIgnoredActor(*It);
    const auto& Transform=Wall->GetActorTransform();
    const FVector Extent=MRFPSBuild::CellExtent()-FVector(.5);
    for(int32 Row=0;Row<Next.HeightRows;++Row) for(int32 Col=0;Col<MRFPSBuild::WallColumns;++Col) {
        if(!Next.IsSolid(Col,Row) || Wall->WallShape.IsSolid(Col,Row)) continue;
        const FVector Center=Transform.TransformPosition(MRFPSBuild::CellCenter(Col,Row));
        if(World->OverlapBlockingTestByChannel(Center,Transform.GetRotation(),ECC_GameTraceChannel1,FCollisionShape::MakeBox(Extent),Query)) return false;
    }
    return true;
}

bool BuildSurface(UWorld* World,const FMRFPSArenaLayout& Layout,const FVector& Origin,const FVector& Direction,FVector& Surface,AActor*& Support)
{
    Support=nullptr;
    if(!ValidBuildRay(Origin,Direction)) return false;
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
    if(!Collision||!Visual||!WallCells) return;
    const bool Editable=bAdminPlaced && BuildKind==EMRFPSBuildKind::Wall && WallShape.IsSane();
    const bool Crate=bAdminPlaced && BuildKind==EMRFPSBuildKind::Crate;
    Collision->SetBoxExtent(MRFPSBuild::Extent(Crate?EMRFPSBuildKind::Crate:EMRFPSBuildKind::Wall));
    Collision->SetCollisionEnabled(bAdminPlaced && !Crate?ECollisionEnabled::NoCollision:ECollisionEnabled::QueryOnly);
    Visual->SetVisibility(!bAdminPlaced || Crate);
    WallCells->ClearInstances();WallCells->SetVisibility(Editable);
    if(!bAdminPlaced) {
        if(auto* Mesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/Meshes/SM_MRWall.SM_MRWall"))) { Visual->SetStaticMesh(Mesh);Visual->SetRelativeScale3D(FVector::OneVector); }
    }
    if(Editable) {
        if(auto* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Wall.M_Wall"))) WallCells->SetMaterial(0,Material);
#if !UE_SERVER
        const FVector VisualExtent=MRFPSBuild::CellExtent()-FVector(0,.5,.5);
        for(int32 Row=0;Row<WallShape.HeightRows;++Row) for(int32 Col=0;Col<MRFPSBuild::WallColumns;++Col) if(WallShape.IsSolid(Col,Row))
            WallCells->AddInstance(FTransform(FQuat::Identity,MRFPSBuild::CellCenter(Col,Row),VisualExtent/50));
#endif
    }
    if (Crate) {
        if (auto* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"))) Visual->SetStaticMesh(Cube);
        Visual->SetRelativeLocation(FVector::ZeroVector);Visual->SetRelativeScale3D(FVector::OneVector);
        if (auto* Wood=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Wood.M_Wood"))) Visual->SetMaterial(0,Wood);
        if (auto* Dark=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_WoodDark.M_WoodDark"))) WallCells->SetMaterial(0,Dark);
#if !UE_SERVER
        // Framing strips on the sides and top read as a wooden crate even at
        // a distance; the authority uses one matching one-metre collision box.
        WallCells->SetVisibility(true);
        const auto Add=[this](FVector Position,FVector Extent) { WallCells->AddInstance(FTransform(FQuat::Identity,Position,Extent/50)); };
        for (double Side:{-1.,1.}) for (double Edge:{-40.,40.}) {
            Add(FVector(Side*50,Edge,0),FVector(2,5,50));
            Add(FVector(Side*50,0,Edge),FVector(2,50,5));
            Add(FVector(Edge,Side*50,0),FVector(5,2,50));
            Add(FVector(0,Side*50,Edge),FVector(50,2,5));
        }
        for (double Edge:{-40.,40.}) {
            Add(FVector(Edge,0,50),FVector(5,50,2));
            Add(FVector(0,Edge,50),FVector(50,5,2));
        }
#endif
    }
    // Merge horizontally adjacent cells into collision boxes. Both the server's
    // weapon trace and each client's scene use the same replicated cell mask.
    int32 Used=0;
    if(Editable) for(int32 Row=0;Row<WallShape.HeightRows;++Row) {
        for(int32 Col=0;Col<MRFPSBuild::WallColumns;) {
            if(!WallShape.IsSolid(Col,Row)) { ++Col;continue; }
            const int32 Start=Col;
            while(Col<MRFPSBuild::WallColumns && WallShape.IsSolid(Col,Row)) ++Col;
            const int32 Count=Col-Start;
            if(!WallCollisionParts.IsValidIndex(Used)) {
                auto* Part=NewObject<UBoxComponent>(this);
                Part->SetupAttachment(Collision);Part->SetCollisionResponseToAllChannels(ECR_Ignore);
                Part->SetCollisionResponseToChannel(ECC_GameTraceChannel1,ECR_Block);
                Part->SetGenerateOverlapEvents(false);AddInstanceComponent(Part);Part->RegisterComponent();
                WallCollisionParts.Add(Part);
            }
            auto* Part=WallCollisionParts[Used++].Get();
            const double Width=MRFPSBuild::WallWidthCm/MRFPSBuild::WallColumns;
            const FVector First=MRFPSBuild::CellCenter(Start,Row);
            Part->SetRelativeLocation(First+FVector(0,(Count-1)*Width*.5,0));
            Part->SetBoxExtent(FVector(MRFPSBuild::WallHalfThicknessCm,Count*Width*.5,MRFPSBuild::WallRowHeightCm*.5));
            Part->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        }
    }
    for(int32 Index=Used;Index<WallCollisionParts.Num();++Index) if(WallCollisionParts[Index])
        WallCollisionParts[Index]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
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
    if(WallCells) Tint(WallCells);
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
    Piece->bAdminPlaced=true;Piece->BuildKind=Kind;Piece->OwnerSlot=-1;Piece->bMapAnchor=true;
    Piece->MapLocalPosition=GS->Layout.ToLocal(Transform.GetLocation());
    Piece->MapLocalYaw=FRotator::NormalizeAxis(Transform.Rotator().Yaw-GS->Layout.YawDegrees);
    UGameplayStatics::FinishSpawningActor(Piece,Transform);Piece->ApplyBuildKind();Piece->ForceNetUpdate();
    UE_LOG(LogMRFPSBuild,Display,TEXT("MRFPS_BUILD_PLACED kind=%d location=%s"),int32(Kind),*Transform.GetLocation().ToCompactString());
    Error.Empty();return true;
}

bool AMRFPSGameMode::EnsureAutomationObject(AMRFPSPlayerController* Admin,EMRFPSBuildKind Kind,const FVector& Surface,const FVector& Origin,const FVector& Direction,float Yaw,FString& Error)
{
    Error=TEXT("ADMIN_REQUIRED"); const auto* GS=GetGameState<AMRFPSGameState>();
    if(!AdminAuthorized(Admin)||!GS) return false;
    FTransform Wanted;
    Error=TEXT("ADMIN_WALL_OUTSIDE_LAYOUT");
    if(Surface.ContainsNaN()||!FMath::IsFinite(Yaw)||!MRFPSBuild::Valid(Kind)||!MRFPSBuild::Snap(GS->Layout,Kind,Surface,Yaw,Wanted)) return false;
    for(TActorIterator<AMRFPSBarrier> It(GetWorld());It;++It) if(It->bAdminPlaced&&!It->bPlacementPreview && It->BuildKind==Kind &&
        It->GetActorLocation().Equals(Wanted.GetLocation(),1) && FMath::Abs(FRotator::NormalizeAxis(It->GetActorRotation().Yaw-Wanted.Rotator().Yaw))<1) {
        Error=TEXT("AUTOMATION_ALREADY"); return true;
    }
    FVector Actual; AActor* Support=nullptr; FTransform Hit;
    Error=TEXT("ADMIN_WALL_INVALID_RAY");
    if(!BuildSurface(GetWorld(),GS->Layout,Origin,Direction,Actual,Support) || !MRFPSBuild::Snap(GS->Layout,Kind,Actual,Yaw,Hit) ||
       !Hit.GetLocation().Equals(Wanted.GetLocation(),1) || FMath::Abs(FRotator::NormalizeAxis(Hit.Rotator().Yaw-Wanted.Rotator().Yaw))>=1) return false;
    return PlaceAdminObject(Admin,Kind,Origin,Direction,Yaw,Error);
}

bool AMRFPSGameMode::SetAutomationWall(AMRFPSPlayerController* Admin,AMRFPSBarrier* Target,const FMRFPSWallShape& Expected,const FMRFPSWallShape& Desired,FString& Error)
{
    Error=TEXT("ADMIN_REQUIRED"); const auto* GS=GetGameState<AMRFPSGameState>();
    if(!AdminAuthorized(Admin)||!GS) return false;
    Error=TEXT("ADMIN_NO_OBJECT");
    if(!IsValid(Target)||Target->GetWorld()!=GetWorld()||!Target->bAdminPlaced||Target->bPlacementPreview||Target->BuildKind!=EMRFPSBuildKind::Wall) return false;
    Error=TEXT("ADMIN_WALL_INVALID_SHAPE"); if(!Expected.IsSane()||!Desired.IsSane()) return false;
    if(Target->WallShape.HeightRows==Desired.HeightRows && Target->WallShape.SolidCells==Desired.SolidCells) { Error=TEXT("AUTOMATION_ALREADY"); return true; }
    Error=TEXT("ADMIN_STALE_STATE");
    if(Target->WallShape.HeightRows!=Expected.HeightRows||Target->WallShape.SolidCells!=Expected.SolidCells) return false;
    Error=TEXT("ADMIN_WALL_OUTSIDE_LAYOUT"); if(!MRFPSBuild::FitsWall(GS->Layout,Target->GetActorTransform(),Desired)) return false;
    Error=TEXT("ADMIN_WALL_OCCUPIED"); if(!AddedCellsAreClear(GetWorld(),Target,Desired)) return false;
    Target->WallShape=Desired; Target->ApplyBuildKind(); Target->ForceNetUpdate(); Error.Empty(); return true;
}

bool AMRFPSGameMode::RemoveAutomationObject(AMRFPSPlayerController* Admin,AMRFPSBarrier* Target,const FMRFPSWallShape& Expected,FString& Error)
{
    Error=TEXT("ADMIN_REQUIRED"); if(!AdminAuthorized(Admin)) return false;
    Error=TEXT("ADMIN_NO_OBJECT");
    if(!IsValid(Target)||Target->GetWorld()!=GetWorld()||!Target->bAdminPlaced||Target->bPlacementPreview) return false;
    Error=TEXT("ADMIN_STALE_STATE");
    if(Target->WallShape.HeightRows!=Expected.HeightRows||Target->WallShape.SolidCells!=Expected.SolidCells) return false;
    Target->Destroy(); Error.Empty(); return true;
}

void AMRFPSGameMode::SpawnDefaultStage()
{
    auto* GS=GetGameState<AMRFPSGameState>();
    if (!HasAuthority() || !GS || !GS->Layout.IsSane()) return;
    struct FInitialPiece { EMRFPSBuildKind Kind; FVector Local; float Yaw; };
    const FInitialPiece Pieces[]={
        {EMRFPSBuildKind::Wall,FVector(-75,-70,100),90},
        {EMRFPSBuildKind::Wall,FVector(75,70,100),90},
        {EMRFPSBuildKind::Crate,FVector(120,-175,50),0},
        {EMRFPSBuildKind::Crate,FVector(-120,175,50),0}
    };
    int32 Spawned=0;
    for (const auto& Initial:Pieces) {
        if (Spawned>=MaxAdminWalls) break;
        const FTransform Transform(FRotator(0,GS->Layout.YawDegrees+Initial.Yaw,0),GS->Layout.ToWorld(Initial.Local));
        if (!MRFPSBuild::Fits(GS->Layout,Initial.Kind,Transform)) continue;
        auto* Piece=GetWorld()->SpawnActorDeferred<AMRFPSBarrier>(AMRFPSBarrier::StaticClass(),Transform,nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!Piece) continue;
        Piece->bAdminPlaced=true;Piece->bDefaultStageObject=true;Piece->bMapAnchor=true;
        Piece->BuildKind=Initial.Kind;Piece->OwnerSlot=-1;
        Piece->MapLocalPosition=Initial.Local;Piece->MapLocalYaw=Initial.Yaw;
        UGameplayStatics::FinishSpawningActor(Piece,Transform);Piece->ApplyBuildKind();Piece->ForceNetUpdate();
        ++Spawned;
    }
}
bool AMRFPSGameMode::EditAdminWall(AMRFPSPlayerController* Admin,const FVector& Origin,const FVector& Direction,EMRFPSWallEdit Edit,FString& Error)
{
    Error=TEXT("ADMIN_REQUIRED");const auto* GS=GetGameState<AMRFPSGameState>();
    if(!AdminAuthorized(Admin)||!GS) return false;
    Error=TEXT("ADMIN_NO_OBJECT");FWallPick Pick;
    if(uint8(Edit)>uint8(EMRFPSWallEdit::Shorter) || !PickAdminWall(GetWorld(),Origin,Direction,Pick)) return false;
    FMRFPSWallShape Next=Pick.Wall->WallShape;
    if(Edit==EMRFPSWallEdit::ToggleCell) Next.SolidCells^=(1u<<(Pick.Row*MRFPSBuild::WallColumns+Pick.Column));
    else if(Edit==EMRFPSWallEdit::Taller && Next.HeightRows<MRFPSBuild::WallMaxRows) ++Next.HeightRows;
    else if(Edit==EMRFPSWallEdit::Shorter && Next.HeightRows>1) --Next.HeightRows;
    else { Error=TEXT("ADMIN_WALL_HEIGHT_LIMIT");return false; }
    Error=TEXT("ADMIN_WALL_OUTSIDE_LAYOUT");
    if(!MRFPSBuild::FitsWall(GS->Layout,Pick.Wall->GetActorTransform(),Next)) return false;
    Error=TEXT("ADMIN_WALL_OCCUPIED");
    if(!AddedCellsAreClear(GetWorld(),Pick.Wall,Next)) return false;
    Pick.Wall->WallShape=Next;Pick.Wall->ApplyBuildKind();Pick.Wall->ForceNetUpdate();
    Error.Empty();return true;
}
bool AMRFPSGameMode::RemoveAdminObject(AMRFPSPlayerController* Admin,const FVector& Origin,const FVector& Direction,FString& Error)
{
    Error=TEXT("ADMIN_REQUIRED");if(!AdminAuthorized(Admin)) return false;
    Error=TEXT("ADMIN_WALL_INVALID_RAY");
    if(!ValidBuildRay(Origin,Direction)) return false;
    FWallPick Pick;
    if(PickAdminWall(GetWorld(),Origin,Direction,Pick)) { Pick.Wall->Destroy();Error.Empty();return true; }
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
void AMRFPSPlayerController::ServerAdminEditWall_Implementation(FVector_NetQuantize10 Origin,FVector_NetQuantizeNormal Direction,EMRFPSWallEdit Edit,uint32 LayoutRevision,uint32 ControlRevision)
{
    auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();auto* GM=GetWorld()->GetAuthGameMode<AMRFPSGameMode>();
    if(!IsAdmin()||!GS||!GM) { ClientAdminResult(TEXT("ADMIN_REQUIRED"));return; }
    const double Now=GetWorld()->GetTimeSeconds();
    if(Now-LastAdminWallAt<.2) { ClientAdminResult(TEXT("ADMIN_RATE_LIMIT"));return; } LastAdminWallAt=Now;
    if(LayoutRevision!=GS->Layout.Revision||ControlRevision!=GS->ControlRevision) { ClientAdminResult(TEXT("ADMIN_STALE_STATE"));return; }
    FString Error;ClientAdminResult(GM->EditAdminWall(this,Origin,Direction,Edit,Error)?FName(TEXT("ADMIN_OK")):FName(*Error));
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
    if(!IsLocalController()||!bAdminWallPlacement||!IsAdmin()) {
        if(AdminPreview) { AdminPreview->Destroy();AdminPreview=nullptr; }
        if(AdminCellPreview) { AdminCellPreview->DestroyComponent();AdminCellPreview=nullptr; }
        SelectedWallHeightCm=0;return;
    }
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();FVector Origin,Direction,Surface;AActor* Support=nullptr;
    if(!GS||!DeprojectMousePositionToWorld(Origin,Direction)) {
        if(AdminPreview) AdminPreview->SetActorHiddenInGame(true);
        if(AdminCellPreview) AdminCellPreview->SetVisibility(false);
        bAdminPreviewValid=false;SelectedWallHeightCm=0;return;
    }
    FWallPick Pick;
    if(PickAdminWall(GetWorld(),Origin,Direction,Pick)) {
        if(AdminPreview) AdminPreview->SetActorHiddenInGame(true);
        SelectedWallHeightCm=Pick.Wall->WallShape.HeightRows*MRFPSBuild::WallRowHeightCm;
        if(!AdminCellPreview) {
            AdminCellPreview=NewObject<UStaticMeshComponent>(this);
            AdminCellPreview->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
            AdminCellPreview->SetCollisionEnabled(ECollisionEnabled::NoCollision);AdminCellPreview->SetCastShadow(false);
            AddInstanceComponent(AdminCellPreview);AdminCellPreview->RegisterComponent();
        }
        const FTransform Wall=Pick.Wall->GetActorTransform();
        const FVector Extent=MRFPSBuild::CellExtent()+FVector(.5,.2,.2);
        AdminCellPreview->SetWorldTransform(FTransform(Wall.GetRotation(),Wall.TransformPosition(MRFPSBuild::CellCenter(Pick.Column,Pick.Row)),Extent/50));
        if(auto* Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Preview.M_Preview"))) {
            auto* Dynamic=Cast<UMaterialInstanceDynamic>(AdminCellPreview->GetMaterial(0));
            if(!Dynamic || Dynamic->Parent!=Base) { Dynamic=UMaterialInstanceDynamic::Create(Base,this);AdminCellPreview->SetMaterial(0,Dynamic); }
            Dynamic->SetVectorParameterValue(TEXT("Tint"),Pick.Wall->WallShape.IsSolid(Pick.Column,Pick.Row)?FLinearColor(.05,.65,1):FLinearColor(.05,1,.55));
        }
        AdminCellPreview->SetVisibility(true);bAdminPreviewValid=true;return;
    }
    if (auto* Crate=PickAdminCrate(GetWorld(),Origin,Direction)) {
        if (AdminPreview) AdminPreview->SetActorHiddenInGame(true);
        SelectedWallHeightCm=-1;
        if (!AdminCellPreview) {
            AdminCellPreview=NewObject<UStaticMeshComponent>(this);
            AdminCellPreview->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
            AdminCellPreview->SetCollisionEnabled(ECollisionEnabled::NoCollision);AdminCellPreview->SetCastShadow(false);
            AddInstanceComponent(AdminCellPreview);AdminCellPreview->RegisterComponent();
        }
        const FTransform Box=Crate->GetActorTransform();
        AdminCellPreview->SetWorldTransform(FTransform(Box.GetRotation(),Box.GetLocation(),(MRFPSBuild::Extent(EMRFPSBuildKind::Crate)+FVector(1))/50));
        if (auto* Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Preview.M_Preview"))) {
            auto* Dynamic=Cast<UMaterialInstanceDynamic>(AdminCellPreview->GetMaterial(0));
            if (!Dynamic || Dynamic->Parent!=Base) { Dynamic=UMaterialInstanceDynamic::Create(Base,this);AdminCellPreview->SetMaterial(0,Dynamic); }
            Dynamic->SetVectorParameterValue(TEXT("Tint"),FLinearColor(.05,.65,1));
        }
        AdminCellPreview->SetVisibility(true);bAdminPreviewValid=false;return;
    }
    SelectedWallHeightCm=0;
    if(AdminCellPreview) AdminCellPreview->SetVisibility(false);
    if(!BuildSurface(GetWorld(),GS->Layout,Origin,Direction,Surface,Support)) {
        if(AdminPreview) AdminPreview->SetActorHiddenInGame(true);bAdminPreviewValid=false;return;
    }
    FTransform Transform;bAdminPreviewValid=MRFPSBuild::Snap(GS->Layout,EMRFPSBuildKind::Wall,Surface,AdminWallYaw,Transform);
    if(!AdminPreview) {
        AdminPreview=GetWorld()->SpawnActorDeferred<AMRFPSBarrier>(AMRFPSBarrier::StaticClass(),Transform,this,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if(!AdminPreview) return;
        AdminPreview->bAdminPlaced=true;AdminPreview->bPlacementPreview=true;AdminPreview->SetReplicates(false);
        UGameplayStatics::FinishSpawningActor(AdminPreview,Transform);AdminPreview->SetActorEnableCollision(false);
    }
    AdminPreview->SetActorHiddenInGame(false);AdminPreview->SetActorTransform(Transform);
    bAdminPreviewValid=bAdminPreviewValid&&Unoccupied(GetWorld(),EMRFPSBuildKind::Wall,Transform,Support);
    AdminPreview->SetPreviewColor(bAdminPreviewValid);
    if(bConsumeAdminPlacementClick&&GetWorld()->GetTimeSeconds()>=NextAdminRepeatAt) PlaceSelectedBuild();
}
void AMRFPSPlayerController::PlaceSelectedBuild()
{
    if(!IsAdmin()||!bAdminWallPlacement) return;
    NextAdminRepeatAt=GetWorld()->GetTimeSeconds()+.22;
    FVector Origin,Direction;const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    if(GS&&DeprojectMousePositionToWorld(Origin,Direction)) ServerAdminPlaceObject(EMRFPSBuildKind::Wall,Origin,Direction,AdminWallYaw,GS->Layout.Revision,GS->ControlRevision);
}
bool AMRFPSPlayerController::ToggleSelectedWallCell()
{
    FVector Origin,Direction;const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();FWallPick Pick;
    if(!IsAdmin()||!bAdminWallPlacement||!GS||!DeprojectMousePositionToWorld(Origin,Direction)||!PickAdminWall(GetWorld(),Origin,Direction,Pick)) return false;
    ServerAdminEditWall(Origin,Direction,EMRFPSWallEdit::ToggleCell,GS->Layout.Revision,GS->ControlRevision);return true;
}
void AMRFPSPlayerController::AdjustSelectedWallHeight(int32 Delta)
{
    FVector Origin,Direction;const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();FWallPick Pick;
    if(!IsAdmin()||!bAdminWallPlacement||!GS||!DeprojectMousePositionToWorld(Origin,Direction)||!PickAdminWall(GetWorld(),Origin,Direction,Pick)) return;
    ServerAdminEditWall(Origin,Direction,Delta>0?EMRFPSWallEdit::Taller:EMRFPSWallEdit::Shorter,GS->Layout.Revision,GS->ControlRevision);
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
    if(AdminStatus==TEXT("ADMIN_WALL_HEIGHT_LIMIT")) return TEXT("高さの上限・下限です");
    if(AdminStatus==TEXT("ADMIN_PLAYERS_NOT_READY")) return TEXT("プレイヤーの準備待ち");
    if(AdminStatus==TEXT("LAYOUT_SHARED_BASIS_UNAVAILABLE")) return TEXT("先に原点を登録してください");
    if(AdminStatus==TEXT("LAYOUT_DIMENSIONS_INVALID")) return TEXT("サイズを確認してください");
    if(AdminStatus==TEXT("ADMIN_INVALID_STATE")) return TEXT("試合を一時停止してください");
    if(AdminStatus==TEXT("ADMIN_STALE_STATE")) return TEXT("もう一度操作してください");
    return FString();
}
