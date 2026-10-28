#include "MRFPSHapticsComponent.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

UMRFPSHapticsComponent::UMRFPSHapticsComponent()
{
    SetIsReplicatedByDefault(false);
}

void UMRFPSHapticsComponent::BeginPlay()
{
    Super::BeginPlay();
    OnReceipt.AddDynamic(this, &UMRFPSHapticsComponent::RecordReceipt);
    OnError.AddDynamic(this, &UMRFPSHapticsComponent::RecordError);
    if (GetNetMode() == NM_DedicatedServer)
    {
        bAutoConnectTracking = false;
        Deactivate();
        SetComponentTickEnabled(false);
    }
}

bool UMRFPSHapticsComponent::IsOwnerLocal() const
{
    if (!GetWorld() || GetNetMode() == NM_DedicatedServer) return false;
    if (const APlayerController* Player = Cast<APlayerController>(GetOwner())) return Player->IsLocalController();
    if (const APawn* Pawn = Cast<APawn>(GetOwner())) return Pawn->IsLocallyControlled();
    return false;
}

void UMRFPSHapticsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    const bool bLocal = IsOwnerLocal();
    if (bLocal && bAutoConnectTracking && !bConnectRequested && !GetWorld()->IsPaused())
    {
        bConnectRequested = true;
        FString Error;
        Connect(Error); // SDK handles bounded tracking-only reconnection.
        if (!Error.IsEmpty()) LastResult = Error;
    }
    else if (!bLocal && bConnectRequested)
    {
        Disconnect();
        bConnectRequested = false;
    }
    Super::TickComponent(DeltaTime, TickType, TickFunction);
}

bool UMRFPSHapticsComponent::CanOutput(float Level) const
{
    return IsOwnerLocal() && FMath::IsFinite(Level) && Level > 0.f && Level <= 1.f && IsHapticsArmed();
}

int64 UMRFPSHapticsComponent::NotifyShotFired()
{
    if (!CanOutput(RecoilLevel)) return 0;
    return bAutomaticRecoil ? PlayAutomaticShot(RecoilForearm, RecoilLevel) :
        PlayMrFpsPreset(RecoilForearm, EMoBitsMrFpsPreset::PistolRecoil, RecoilLevel);
}

int64 UMRFPSHapticsComponent::NotifyDamage(float Damage, bool bLethal)
{
    if (!FMath::IsFinite(Damage) || Damage <= 0.f || !CanOutput(DamageLevel)) return 0;
    const EMoBitsMrFpsPreset Preset = bLethal ? EMoBitsMrFpsPreset::LethalHit :
        (Damage >= 40.f ? EMoBitsMrFpsPreset::HeavyHit : EMoBitsMrFpsPreset::Hit);
    return PlayMrFpsPreset(DamageForearm, Preset, DamageLevel);
}

int64 UMRFPSHapticsComponent::NotifyRespawn()
{
    return CanOutput(RespawnLevel) ? PlayMrFpsPreset(DamageForearm, EMoBitsMrFpsPreset::Revive, RespawnLevel) : 0;
}

int64 UMRFPSHapticsComponent::StopHaptics()
{
    return StopAll();
}

void UMRFPSHapticsComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    StopAll();
    // The SDK closes both transports at EndPlay. The app independently confirms
    // hardware OFF; closure itself is never presented as confirmed completion.
    Super::EndPlay(Reason);
}

void UMRFPSHapticsComponent::RecordReceipt(const FMoBitsReceipt& Receipt)
{
    LastResult = Receipt.Reason.IsEmpty() ? Receipt.Status : Receipt.Status + TEXT(": ") + Receipt.Reason;
}

void UMRFPSHapticsComponent::RecordError(const FString& Message)
{
    LastResult = Message;
}

FString UMRFPSHapticsComponent::GetStatus() const
{
    const TCHAR* State = TEXT("disconnected");
    switch (GetConnectionState())
    {
    case EMoBitsConnectionState::Connecting: State = TEXT("connecting"); break;
    case EMoBitsConnectionState::Connected: State = IsHapticsArmed() ? TEXT("armed") : TEXT("connected / unarmed"); break;
    case EMoBitsConnectionState::Disconnecting: State = TEXT("stopping"); break;
    case EMoBitsConnectionState::Faulted: State = IsReconnecting() ? TEXT("reconnecting / unarmed") : TEXT("faulted"); break;
    default: break;
    }
    return LastResult.IsEmpty() ? FString(State) : FString(State) + TEXT(" - ") + LastResult;
}
