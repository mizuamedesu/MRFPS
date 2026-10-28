#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MoBitsSkeletonComponent.generated.h"

class UMoBitsConnectionComponent;
class UPoseableMeshComponent;
class USceneComponent;

USTRUCT(BlueprintType)
struct MOBITSSDK_API FMoBitsBoneBinding
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits", meta=(ClampMin="1", ClampMax="20")) int32 MoBitsBoneId = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits") FName RigBone;
};

/** Reference-pose rotation retargeting for a Poseable Mesh; does not overwrite an Anim Blueprint. */
UCLASS(ClassGroup=(MoBits), meta=(BlueprintSpawnableComponent))
class MOBITSSDK_API UMoBitsSkeletonComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UMoBitsSkeletonComponent();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits") TObjectPtr<UMoBitsConnectionComponent> Connection;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits") TObjectPtr<UPoseableMeshComponent> TargetMesh;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits") TObjectPtr<USceneComponent> MotionOrigin;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits") TArray<FMoBitsBoneBinding> Bindings;
    /** Uses only the estimated Hips displacement; bone translations remain those of the rig. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MoBits") bool bApplyEstimatedRootPosition = false;
    UPROPERTY(Transient, BlueprintReadOnly, Category="MoBits") bool bIsTracked = false;
    UFUNCTION(BlueprintCallable, Category="MoBits") void Recalibrate();
protected:
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    virtual void Deactivate() override;
private:
    bool bHasReference = false;
    int64 OriginRevision = 0;
    TWeakObjectPtr<UMoBitsConnectionComponent> LastConnection;
    TWeakObjectPtr<UPoseableMeshComponent> LastMesh;
    TMap<int32, FQuat> SourceReference;
    TMap<int32, FQuat> RigReference;
    FQuat TrackingToMesh = FQuat::Identity;
    FVector SourceRootReference = FVector::ZeroVector;
    FVector MeshRootReference = FVector::ZeroVector;
};
