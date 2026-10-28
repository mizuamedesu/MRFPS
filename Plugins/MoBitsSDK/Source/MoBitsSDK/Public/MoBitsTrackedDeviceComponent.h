#pragma once
#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "MoBitsTrackedDeviceComponent.generated.h"

class UMoBitsConnectionComponent;

/** Applies a fresh device pose to this scene component. Position is opt-in. */
UCLASS(ClassGroup=(MoBits), meta=(BlueprintSpawnableComponent))
class MOBITSSDK_API UMoBitsTrackedDeviceComponent : public USceneComponent
{
    GENERATED_BODY()
public:
    UMoBitsTrackedDeviceComponent();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits") TObjectPtr<UMoBitsConnectionComponent> Connection;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits") FString DeviceId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits") TObjectPtr<USceneComponent> MotionOrigin;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits") bool bApplyEstimatedPosition = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits") bool bPreservePoseOnOriginChange = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits", meta=(ClampMin="0")) float SmoothingSeconds = 0.03f;
    UPROPERTY(Transient, BlueprintReadOnly, Category="MoBits") bool bIsTracked = false;
    UFUNCTION(BlueprintCallable, Category="MoBits") void ResetTrackingOffset();
protected:
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    virtual void Deactivate() override;
private:
    bool bInitialized = false;
    int64 OriginRevision = 0;
    int64 SessionGeneration = 0;
    double LastUpdate = 0;
    FString LastDeviceId;
    TWeakObjectPtr<UMoBitsConnectionComponent> LastConnection;
    FQuat RotationOffset = FQuat::Identity;
    FVector PositionOffset = FVector::ZeroVector;
};
