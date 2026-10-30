#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MRFPSFlashGrenade.generated.h"

class AMRFPSPawn;
class UStaticMeshComponent;

/** The server owns the trajectory and flash effect; clients only render it. */
UCLASS()
class MRFPS_API AMRFPSFlashGrenade : public AActor
{
    GENERATED_BODY()
public:
    AMRFPSFlashGrenade();
    virtual void Tick(float DeltaSeconds) override;
    void Arm(AMRFPSPawn* Thrower,const FVector& InitialVelocity);

private:
    void Detonate();
    UFUNCTION(NetMulticast, Unreliable) void MulticastBurst();
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Visual;
    FVector Velocity=FVector::ZeroVector;
    float FuseRemaining=1.8f;
    bool bArmed=false;
    bool bStopped=false;
};
