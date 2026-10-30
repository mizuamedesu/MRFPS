#include "MRFPSGame.h"
#include "MRFPSFlashGrenade.h"
#include "MRFPSLoginMenu.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"

void AMRFPSPawn::SuspendPhysicalFireForMenu()
{
    bFiring=false; bDesktopShield=false; CancelGrenade();
    if (auto* PC=Cast<AMRFPSPlayerController>(Controller)) {
        PC->AutomationReloadUntil=0; PC->ReloadReleased();
    }
}

void AMRFPSPawn::SetGrenadeHeld(bool bHeld)
{
    if (!IsLocallyControlled() || !IsXR() || !GetWorld()) return;
    if (bHeld) {
        const auto* PC=Cast<AMRFPSPlayerController>(Controller);
        const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
        if (bLocalGrenadeHeld || !GS || GS->bCombatPaused || Health<=0 || GS->GetServerWorldTimeSeconds()<GrenadeReadyAt ||
            (GS->Phase!=EMRFPSMatchPhase::Waiting && GS->Phase!=EMRFPSMatchPhase::Playing) ||
            !Pose.bHeadTracked || !Pose.bRightTracked || (PC && PC->DeviceLogin && PC->DeviceLogin->IsOpen())) return;
        bLocalGrenadeHeld=true; bFiring=false;
        if (bPhysicalFireDown) bRequireFireRelease=true;
        ServerEquipGrenade();
    } else if (bLocalGrenadeHeld) {
        bLocalGrenadeHeld=false;
        const double Age=FPlatformTime::Seconds()-PreviousRightHandAt;
        if (!Pose.bHeadTracked || !Pose.bRightTracked || !bHaveRightHandSample || Age<0 || Age>.15) {
            ServerCancelGrenade(); return;
        }
        ServerThrowGrenade(RightHandVelocity.GetClampedToMaxSize(1600.f));
    }
}

void AMRFPSPawn::CancelGrenade()
{
    const bool WasHeld=bLocalGrenadeHeld;
    bLocalGrenadeHeld=false;
    if (WasHeld && IsLocallyControlled()) ServerCancelGrenade();
}

void AMRFPSPawn::ServerEquipGrenade_Implementation()
{
    if (!HasAuthority() || !CanAct() || !Pose.bRightTracked || GetWorld()->GetTimeSeconds()<GrenadeReadyAt) return;
    bGrenadeEquipped=true; ForceNetUpdate();
}

void AMRFPSPawn::ServerCancelGrenade_Implementation()
{
    if (!HasAuthority()) return;
    bGrenadeEquipped=false; ForceNetUpdate();
}

void AMRFPSPawn::ServerThrowGrenade_Implementation(FVector_NetQuantize10 RequestedVelocity)
{
    if (!HasAuthority() || !bGrenadeEquipped || !GetWorld()) return;
    bGrenadeEquipped=false; ForceNetUpdate();
    const FVector Velocity=RequestedVelocity;
    if (!CanAct() || !Pose.bRightTracked || Velocity.ContainsNaN() || GetWorld()->GetTimeSeconds()<GrenadeReadyAt) return;
    FActorSpawnParameters Params; Params.Owner=this;
    Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (auto* Grenade=GetWorld()->SpawnActor<AMRFPSFlashGrenade>(FVector(Pose.Right),FRotator::ZeroRotator,Params)) {
        Grenade->Arm(this,Velocity.GetClampedToMaxSize(1600.f));
        GrenadeReadyAt=GetWorld()->GetTimeSeconds()+1.;
        UE_LOG(LogTemp,Display,TEXT("MRFPS_GRENADE_THROW slot=%d speed=%.1f"),GetSlot(),Velocity.Size());
    }
}

void AMRFPSPawn::ApplyFlash(float Strength,float Duration)
{
    if (!HasAuthority() || !GetWorld() || !FMath::IsFinite(Strength) || !FMath::IsFinite(Duration)) return;
    Strength=FMath::Clamp(Strength,0.f,1.f); Duration=FMath::Clamp(Duration,0.f,4.f);
    if (Strength<=0 || Duration<=0) return;
    BlindedUntil=FMath::Max(BlindedUntil,GetWorld()->GetTimeSeconds()+Duration);
    if (Cast<APlayerController>(Controller)) ClientFlash(Strength,Duration);
}

bool AMRFPSPawn::IsFlashed() const
{
    return HasAuthority() && GetWorld() && GetWorld()->GetTimeSeconds()<BlindedUntil;
}

void AMRFPSPawn::ClientFlash_Implementation(float Strength,float Duration)
{
#if !UE_SERVER
    if (!IsLocallyControlled() || !FMath::IsFinite(Strength) || !FMath::IsFinite(Duration)) return;
    const double Now=FPlatformTime::Seconds();
    const float Previous=FlashIntensity();
    const float Remaining=LocalFlashStartedAt<0 ? 0.f : FMath::Max(0.f,LocalFlashDuration-float(Now-LocalFlashStartedAt));
    LocalFlashStartedAt=Now;
    LocalFlashStrength=FMath::Max(Previous,FMath::Clamp(Strength,0.f,1.f));
    LocalFlashDuration=FMath::Max(Remaining,FMath::Clamp(Duration,0.f,4.f));
#endif
}

float AMRFPSPawn::FlashIntensity() const
{
#if !UE_SERVER
    if (!IsLocallyControlled() || LocalFlashStartedAt<0 || LocalFlashDuration<=0) return 0.f;
    const float Age=FMath::Max(0.f,float(FPlatformTime::Seconds()-LocalFlashStartedAt));
    if (Age>=LocalFlashDuration) return 0.f;
    const float Hold=FMath::Min(.35f,LocalFlashDuration*.25f);
    const float Fade=1.f-FMath::SmoothStep(Hold,LocalFlashDuration,Age);
    return FMath::Clamp(LocalFlashStrength*Fade,0.f,1.f);
#else
    return 0.f;
#endif
}
