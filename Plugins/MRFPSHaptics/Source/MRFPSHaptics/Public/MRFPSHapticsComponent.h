#pragma once

#include "CoreMinimal.h"
#include "MoBitsConnectionComponent.h"
#include "MRFPSHapticsComponent.generated.h"

/** Attach to the local PlayerController or pawn. Game events never arm EMS. */
UCLASS(ClassGroup=(MRFPS), meta=(BlueprintSpawnableComponent))
class MRFPSHAPTICS_API UMRFPSHapticsComponent : public UMoBitsConnectionComponent
{
    GENERATED_BODY()
public:
    UMRFPSHapticsComponent();

    /** Connect tracking when this component's player becomes local. Never arms. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MRFPS|MoBits")
    bool bAutoConnectTracking = true;

    /** Uses SDK preset 13 once per real shot; shooting interval must be >= 150 ms. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MRFPS|MoBits")
    bool bAutomaticRecoil = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MRFPS|MoBits")
    EMoBitsBodyTarget RecoilForearm = EMoBitsBodyTarget::RightForearm;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MRFPS|MoBits")
    EMoBitsBodyTarget DamageForearm = EMoBitsBodyTarget::LeftForearm;

    /** Relative to the wearer-calibrated limit registered in the MoBits app. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MRFPS|MoBits", meta=(ClampMin="0", ClampMax="1"))
    float RecoilLevel = 0.25f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MRFPS|MoBits", meta=(ClampMin="0", ClampMax="1"))
    float DamageLevel = 0.25f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MRFPS|MoBits", meta=(ClampMin="0", ClampMax="1"))
    float RespawnLevel = 0.20f;

    /** Call only when an owner-local shot has actually fired, once per shot. */
    UFUNCTION(BlueprintCallable, Category="MRFPS|MoBits") int64 NotifyShotFired();
    /** Call from the owning player's confirmed damage event; never on all clients. */
    UFUNCTION(BlueprintCallable, Category="MRFPS|MoBits") int64 NotifyDamage(float Damage, bool bLethal = false);
    UFUNCTION(BlueprintCallable, Category="MRFPS|MoBits") int64 NotifyRespawn();
    /** Immediate local disarm and SDK STOP_ALL; receipt reports confirmation. */
    UFUNCTION(BlueprintCallable, Category="MRFPS|MoBits") int64 StopHaptics();
    UFUNCTION(BlueprintPure, Category="MRFPS|MoBits") FString GetStatus() const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    bool IsOwnerLocal() const;
    bool CanOutput(float Level) const;
    bool bConnectRequested = false;
    FString LastResult;
    UFUNCTION() void RecordReceipt(const FMoBitsReceipt& Receipt);
    UFUNCTION() void RecordError(const FString& Message);
};
