#include "MoBitsConnectionComponent.h"
#include "MoBitsClient.h"
#include "MoBitsCore.h"
#include "Engine/World.h"
#include "HAL/PlatformApplicationMisc.h"

// Errors never contain credentials; state and errors help diagnose stalled recovery.
DEFINE_LOG_CATEGORY_STATIC(LogMoBits, Log, All);

UMoBitsConnectionComponent::UMoBitsConnectionComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bTickEvenWhenPaused = true;
    PrimaryComponentTick.TickGroup = TG_PrePhysics;
    bAutoActivate = true;
}
void UMoBitsConnectionComponent::BindClient() { if (!Client) Client = MakeShared<FMoBitsClient>(); }
void UMoBitsConnectionComponent::BeginPlay() { Super::BeginPlay(); BindClient(); }
bool UMoBitsConnectionComponent::Connect(FString& Error)
{
    Error.Empty();
    if (!IsActive() || !IsComponentTickEnabled() || !GetWorld() || !GetWorld()->IsGameWorld() || GetWorld()->IsPaused())
    { Error = TEXT("Connect during active, unpaused play with component ticking enabled."); return false; }
    BindClient();
    if (Client->GetState() == EMoBitsConnectionState::Connected) return true;
    return Client->ConnectLocal(Error);
}
bool UMoBitsConnectionComponent::ConnectSessionJson(const FString& SessionJson, FString& Error)
{
    Error.Empty();
    if (!IsActive() || !IsComponentTickEnabled() || !GetWorld() || !GetWorld()->IsGameWorld() || GetWorld()->IsPaused())
    { Error = TEXT("Connect during active, unpaused play with component ticking enabled."); return false; }
    BindClient(); return Client->Connect(SessionJson, Error);
}
bool UMoBitsConnectionComponent::ConnectFromClipboard(FString& Error)
{
    FString Json; FPlatformApplicationMisc::ClipboardPaste(Json);
    const bool bResult = ConnectSessionJson(Json, Error); Json.Empty(); return bResult;
}
bool UMoBitsConnectionComponent::IsReconnecting() const { return Client && Client->IsReconnecting(); }
int32 UMoBitsConnectionComponent::GetReconnectAttempts() const { return Client ? Client->GetReconnectAttempts() : 0; }
void UMoBitsConnectionComponent::SetAutoReconnect(bool bEnabled) { BindClient(); Client->bAutoReconnect = bEnabled; }
void UMoBitsConnectionComponent::Disconnect() { if (Client) Client->Disconnect(); }
void UMoBitsConnectionComponent::ForceClose() { if (Client) { Client->ForceClose(); Client.Reset(); } }
void UMoBitsConnectionComponent::Deactivate() { ForceClose(); Super::Deactivate(); }
void UMoBitsConnectionComponent::EndPlay(const EEndPlayReason::Type Reason) { ForceClose(); Super::EndPlay(Reason); }
void UMoBitsConnectionComponent::OnComponentDestroyed(bool bDestroyingHierarchy) { ForceClose(); Super::OnComponentDestroyed(bDestroyingHierarchy); }
EMoBitsConnectionState UMoBitsConnectionComponent::GetConnectionState() const
{ return Client ? Client->GetState() : EMoBitsConnectionState::Disconnected; }
int64 UMoBitsConnectionComponent::GetSessionGeneration() const { return Client ? static_cast<int64>(Client->GetGeneration()) : 0; }
bool UMoBitsConnectionComponent::IsHapticsArmed() const { return Client && Client->Armed(); }
int64 UMoBitsConnectionComponent::ArmHaptics() { return Client ? Client->Arm() : 0; }
int64 UMoBitsConnectionComponent::Play(EMoBitsBodyTarget Target, int32 Effect, float Level, int32 TtlMs)
{ return Client ? Client->Play(static_cast<int32>(Target), Effect, Level, TtlMs) : 0; }
int64 UMoBitsConnectionComponent::PlayEffect(EMoBitsBodyTarget Target, EMoBitsHapticEffect Effect, float Level, int32 TtlMs)
{ return Play(Target, GetHapticEffectId(Effect), Level, TtlMs); }
int32 UMoBitsConnectionComponent::GetMrFpsPresetId(EMoBitsMrFpsPreset Preset)
{ return MoBitsCore::MrFpsEffectId(static_cast<int32>(Preset)); }
int64 UMoBitsConnectionComponent::PlayMrFpsPreset(EMoBitsBodyTarget Forearm, EMoBitsMrFpsPreset Preset, float Level, int32 TtlMs)
{
    const int32 Effect = GetMrFpsPresetId(Preset);
    if (!MoBitsCore::MrFpsTarget(static_cast<int32>(Forearm)) || Effect == 0) return 0;
    return Play(Forearm, Effect, Level, TtlMs);
}
int64 UMoBitsConnectionComponent::PlayAutomaticShot(EMoBitsBodyTarget Forearm, float Level, int32 TtlMs)
{ return PlayMrFpsPreset(Forearm, EMoBitsMrFpsPreset::AutomaticShot, Level, TtlMs); }
int32 UMoBitsConnectionComponent::GetHapticEffectId(EMoBitsHapticEffect Effect) { return static_cast<int32>(Effect); }
FString UMoBitsConnectionComponent::GetHapticEffectName(int32 Effect)
{
    if (!MoBitsCore::ValidEffect(Effect)) return FString();
    const char* Name = MoBitsCore::EffectName(Effect);
    return Name ? FString(UTF8_TO_TCHAR(Name)) : FString::Printf(TEXT("Effect%02d"), Effect);
}
FText UMoBitsConnectionComponent::GetHapticEffectDisplayName(int32 Effect)
{
    switch (Effect)
    {
    case 1: return FText::FromString(TEXT("タップ"));
    case 2: return FText::FromString(TEXT("ナッジ"));
    case 3: return FText::FromString(TEXT("インパクト"));
    default: return MoBitsCore::ValidEffect(Effect) ? FText::FromString(FString::Printf(TEXT("効果%02d"), Effect)) : FText();
    }
}
int64 UMoBitsConnectionComponent::PlayGlobal(int32 Effect, float Level, int32 TtlMs)
{ return Client ? Client->PlayGlobal(Effect, Level, TtlMs) : 0; }
int64 UMoBitsConnectionComponent::StopAll() { return Client ? Client->Stop() : 0; }
int64 UMoBitsConnectionComponent::GetDevices() { return Client ? Client->Devices() : 0; }
bool UMoBitsConnectionComponent::GetLatestFrame(FMoBitsMotionFrame& Frame) const { return Client && Client->Frame(Frame); }
bool UMoBitsConnectionComponent::TryGetDevicePose(const FString& DeviceId, FMoBitsDevicePose& Pose) const
{ return Client && Client->DevicePose(DeviceId, Pose); }
void UMoBitsConnectionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaTime, TickType, TickFunction);
    if (!Client) return;
    if (!IsActive() || !GetWorld()) { ForceClose(); return; }
    if (GetWorld()->IsPaused()) Client->Suspend();
    else Client->Resume();
    const auto Current = Client;
    Current->Tick();
    TArray<FMoBitsClient::FEvent> Events; Current->Drain(Events);
    for (const auto& E : Events)
    {
        if (Client != Current || E.Generation != Current->GetGeneration()) break;
        // A Blueprint listener can disconnect during this same delivery batch.
        if ((E.Kind == FMoBitsClient::EEventKind::Motion || E.Kind == FMoBitsClient::EEventKind::Devices) &&
            Current->GetState() != EMoBitsConnectionState::Connected) continue;
        switch (E.Kind)
        {
        case FMoBitsClient::EEventKind::State:
            UE_LOG(LogMoBits, Log, TEXT("Connection state %d%s"), static_cast<int32>(E.State), IsReconnecting() ? TEXT(" (reconnecting)") : TEXT(""));
            OnStateChanged.Broadcast(E.State); break;
        case FMoBitsClient::EEventKind::Receipt: OnReceipt.Broadcast(E.Receipt); break;
        case FMoBitsClient::EEventKind::Devices: OnDevices.Broadcast(E.Devices); break;
        case FMoBitsClient::EEventKind::Motion: OnMotion.Broadcast(E.Frame); break;
        case FMoBitsClient::EEventKind::Error:
            UE_LOG(LogMoBits, Warning, TEXT("%s"), *E.Error);
            OnError.Broadcast(E.Error); break;
        }
    }
}
