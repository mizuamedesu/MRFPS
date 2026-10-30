#include "MRFPSFlashGrenade.h"
#include "MRFPSGame.h"
#include "MRFPSGeometryWorld.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSFlashGrenade,Log,All);

AMRFPSFlashGrenade::AMRFPSFlashGrenade()
{
    PrimaryActorTick.bCanEverTick=true;
    PrimaryActorTick.TickInterval=1.f/30.f;
    bReplicates=true; bAlwaysRelevant=true;
    SetReplicateMovement(true); SetNetUpdateFrequency(30);
    Visual=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Grenade"));
    SetRootComponent(Visual); Visual->SetMobility(EComponentMobility::Movable);
    ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    Visual->SetStaticMesh(Sphere.Object); Visual->SetRelativeScale3D(FVector(.12));
    Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision); Visual->SetCastShadow(false);
}

void AMRFPSFlashGrenade::Arm(AMRFPSPawn* Thrower,const FVector& InitialVelocity)
{
    if (!HasAuthority() || !Thrower || InitialVelocity.ContainsNaN()) { Destroy(); return; }
    SetOwner(Thrower);
    Velocity=InitialVelocity.GetClampedToMaxSize(1600.f);
    FuseRemaining=1.8f; bArmed=true;
    ForceNetUpdate();
}

void AMRFPSFlashGrenade::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || !bArmed || !GetWorld()) return;
    const auto* State=GetWorld()->GetGameState<AMRFPSGameState>();
    if (!State || (State->Phase!=EMRFPSMatchPhase::Playing && State->Phase!=EMRFPSMatchPhase::Waiting)) { Destroy(); return; }
    if (State->bCombatPaused) return;
    const float Dt=FMath::Clamp(DeltaSeconds,0.f,.1f);
    FuseRemaining-=Dt;
    if (!bStopped && Dt>0) {
        const FVector Start=GetActorLocation();
        const FVector End=Start+Velocity*Dt+FVector(0,0,-490.f*Dt*Dt);
        Velocity.Z-=980.f*Dt;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(MRFPSGrenade),false,this);
        if (GetOwner()) Params.AddIgnoredActor(GetOwner());
        FHitResult Hit;
        const bool WorldHit=GetWorld()->SweepSingleByChannel(Hit,Start,End,FQuat::Identity,
            ECC_GameTraceChannel1,FCollisionShape::MakeSphere(6.f),Params);
        FVector GeometryHit;
        const auto* Geometry=GetWorld()->GetSubsystem<UMRFPSGeometryWorld>();
        const bool MeasuredHit=Geometry && Geometry->Trace(Start,End,GeometryHit) &&
            (!WorldHit || FVector::DistSquared(Start,GeometryHit)<=FVector::DistSquared(Start,Hit.Location));
        if (MeasuredHit) {
            SetActorLocation(GeometryHit); Velocity=FVector::ZeroVector; bStopped=true;
        } else if (WorldHit) {
            SetActorLocation(Hit.Location+Hit.Normal);
            if (Hit.bStartPenetrating || Hit.Normal.ContainsNaN()) { Velocity=FVector::ZeroVector; bStopped=true; }
            else Velocity=Velocity.MirrorByVector(Hit.Normal)*.35f;
        } else SetActorLocation(End);
        const float Floor=State->Layout.Center.Z+6.f;
        if (GetActorLocation().Z<Floor) {
            FVector P=GetActorLocation(); P.Z=Floor; SetActorLocation(P);
            Velocity.Z=FMath::Abs(Velocity.Z)*.32f; Velocity.X*=.7f; Velocity.Y*=.7f;
            if (Velocity.SizeSquared()<FMath::Square(25.f)) { Velocity=FVector::ZeroVector; bStopped=true; }
        }
    }
    if (FuseRemaining<=0) Detonate();
}

void AMRFPSFlashGrenade::Detonate()
{
    if (!HasAuthority() || !bArmed) return;
    bArmed=false;
    const FVector Origin=GetActorLocation();
    const auto* State=GetWorld()->GetGameState<AMRFPSGameState>();
    const auto* Geometry=GetWorld()->GetSubsystem<UMRFPSGeometryWorld>();
    if (State && State->Phase==EMRFPSMatchPhase::Playing) for (TActorIterator<AMRFPSPawn> It(GetWorld());It;++It) {
        AMRFPSPawn* Target=*It;
        if (!Target || !Target->CanAct() || !Target->Pose.bHeadTracked) continue;
        const FVector Head=Target->Pose.Head;
        const float Distance=FVector::Distance(Origin,Head);
        if (!FMath::IsFinite(Distance) || Distance>=700.f) continue;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(MRFPSFlashVisibility),false,this);
        Params.AddIgnoredActor(Target);
        FHitResult Hit;
        if (GetWorld()->LineTraceSingleByChannel(Hit,Origin,Head,ECC_GameTraceChannel1,Params)) continue;
        FVector Obstacle;
        if (Geometry && Geometry->Trace(Origin,Head,Obstacle)) continue;
        const FVector Toward=(Origin-Head).GetSafeNormal();
        const float Facing=FMath::Max(0.f,FVector::DotProduct(Target->Pose.HeadRotation.Vector(),Toward));
        const float Strength=FMath::Clamp((1.f-Distance/700.f)*(.35f+.65f*Facing),.1f,1.f);
        Target->ApplyFlash(Strength,1.f+3.f*Strength);
    }
    UE_LOG(LogMRFPSFlashGrenade,Display,TEXT("MRFPS_FLASH_DETONATED"));
    MulticastBurst(); SetLifeSpan(.25f); ForceNetUpdate();
}

void AMRFPSFlashGrenade::MulticastBurst_Implementation()
{
#if !UE_SERVER
    if (Visual) { Visual->SetRelativeScale3D(FVector(1.2f)); Visual->SetVisibility(true); }
#endif
}
