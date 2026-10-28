#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MoBitsTypes.h"
#include "MoBitsConnectionComponent.generated.h"

class FMoBitsClient;

/** One local MoBits app session per component. Never automatically arms. */
UCLASS(ClassGroup=(MoBits), meta=(BlueprintSpawnableComponent))
class MOBITSSDK_API UMoBitsConnectionComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UMoBitsConnectionComponent();
    UPROPERTY(BlueprintAssignable, Category="MoBits") FMoBitsStateEvent OnStateChanged;
    UPROPERTY(BlueprintAssignable, Category="MoBits") FMoBitsReceiptEvent OnReceipt;
    UPROPERTY(BlueprintAssignable, Category="MoBits") FMoBitsDevicesEvent OnDevices;
    UPROPERTY(BlueprintAssignable, Category="MoBits") FMoBitsMotionEvent OnMotion;
    UPROPERTY(BlueprintAssignable, Category="MoBits") FMoBitsErrorEvent OnError;

    /** Retry transient failures while ticking. Recovery never arms or repeats EMS. */
    UFUNCTION(BlueprintPure, Category="MoBits|Connection") bool IsReconnecting() const;
    UFUNCTION(BlueprintPure, Category="MoBits|Connection") int32 GetReconnectAttempts() const;
    UFUNCTION(BlueprintCallable, Category="MoBits|Connection") void SetAutoReconnect(bool bEnabled);
    /** Discovers the current OS user's running app. Requires no copied credentials. Re-reads discovery after every fault. */
    UFUNCTION(BlueprintCallable, Category="MoBits|Connection") bool Connect(FString& Error);
    /** Advanced manual connection for older apps. Credentials are never UPROPERTY data. */
    UFUNCTION(BlueprintCallable, Category="MoBits|Connection") bool ConnectSessionJson(const FString& SessionJson, FString& Error);
    UFUNCTION(BlueprintCallable, Category="MoBits|Connection") bool ConnectFromClipboard(FString& Error);
    /** Sends STOP_ALL, waits for confirmation up to five seconds, then closes. */
    UFUNCTION(BlueprintCallable, Category="MoBits|Connection") void Disconnect();
    UFUNCTION(BlueprintPure, Category="MoBits|Connection") EMoBitsConnectionState GetConnectionState() const;
    UFUNCTION(BlueprintPure, Category="MoBits|Connection") int64 GetSessionGeneration() const;
    UFUNCTION(BlueprintPure, Category="MoBits|Haptics") bool IsHapticsArmed() const;
    /** Returns request ID, or 0 when local validation fails. Results arrive via OnReceipt. */
    UFUNCTION(BlueprintCallable, Category="MoBits|Haptics") int64 ArmHaptics();
    UFUNCTION(BlueprintCallable, Category="MoBits|Haptics", meta=(ClampMin="1", ClampMax="30"))
    int64 Play(EMoBitsBodyTarget Target, int32 Effect, float NormalizedLevel, int32 TtlMs = 200);
    /** Typed form of Play. Sends the same wire ID as Play(Target, static_cast<int32>(Effect), ...). */
    UFUNCTION(BlueprintCallable, Category="MoBits|Haptics", meta=(DisplayName="Play Effect"))
    int64 PlayEffect(EMoBitsBodyTarget Target, EMoBitsHapticEffect Effect, float NormalizedLevel, int32 TtlMs = 200);
    /** Plays an app-registered MR FPS preset on one forearm. Requires explicit ARM.
     * TTL is the START/buffering deadline, not stimulus duration. The stream path adds 400 ms
     * lead after preload. AutomaticShot appends; other presets replace the arm's EMS tail.
     */
    UFUNCTION(BlueprintCallable, Category="MoBits|MR FPS", meta=(DisplayName="Play MR FPS Preset"))
    int64 PlayMrFpsPreset(EMoBitsBodyTarget Forearm, EMoBitsMrFpsPreset Preset, float NormalizedLevel, int32 TtlMs = 500);
    /** Call once per actual shot, normally every 150 ms. Shares one buffered clock;
     * LRA on every shot, the registered EMS kick on shots 1, 3, 5... . No automatic fire.
     */
    UFUNCTION(BlueprintCallable, Category="MoBits|MR FPS", meta=(DisplayName="Play Automatic Shot"))
    int64 PlayAutomaticShot(EMoBitsBodyTarget Forearm, float NormalizedLevel, int32 TtlMs = 500);
    UFUNCTION(BlueprintPure, Category="MoBits|MR FPS") static int32 GetMrFpsPresetId(EMoBitsMrFpsPreset Preset);
    /** Wire ID 1..30 of a body effect, or 0 for None. */
    UFUNCTION(BlueprintPure, Category="MoBits|Haptics") static int32 GetHapticEffectId(EMoBitsHapticEffect Effect);
    /** "Tap", or "Effect04" for slots without a tactile name. Empty for unregistered IDs. */
    UFUNCTION(BlueprintPure, Category="MoBits|Haptics") static FString GetHapticEffectName(int32 Effect);
    /** "タップ", or "効果04" for slots without a tactile name. Empty for unregistered IDs. */
    UFUNCTION(BlueprintPure, Category="MoBits|Haptics") static FText GetHapticEffectDisplayName(int32 Effect);
    /** Global effect numbers 1..30 are independent of the body-targeted numbers. */
    UFUNCTION(BlueprintCallable, Category="MoBits|Haptics") int64 PlayGlobal(int32 Effect, float NormalizedLevel, int32 TtlMs = 200);
    UFUNCTION(BlueprintCallable, Category="MoBits|Haptics") int64 StopAll();
    UFUNCTION(BlueprintCallable, Category="MoBits|Connection") int64 GetDevices();
    UFUNCTION(BlueprintPure, Category="MoBits|Tracking") bool GetLatestFrame(FMoBitsMotionFrame& Frame) const;
    UFUNCTION(BlueprintPure, Category="MoBits|Tracking") bool TryGetDevicePose(const FString& DeviceId, FMoBitsDevicePose& Pose) const;

    virtual void Deactivate() override;
    virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
private:
    void BindClient();
    void ForceClose();
    TSharedPtr<FMoBitsClient> Client;
};
