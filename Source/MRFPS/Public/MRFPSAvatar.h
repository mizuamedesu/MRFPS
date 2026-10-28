#pragma once

#include "CoreMinimal.h"
#include "Components/PoseableMeshComponent.h"
#include "MRFPSAvatar.generated.h"

class UBlendSpace;
class UAnimSequence;
struct FMRFPSPose;
struct FMRFPSArenaLayout;
struct FMRFPSAvatarRig;

/** Manny locomotion followed by a no-stretch full-body solve. Only the head
 * and tracked hands are measurements; the pelvis and planted feet are inferred.
 * This is a visual rig and never changes tracking space or server hitboxes. */
UCLASS()
class MRFPS_API UMRFPSAvatarComponent : public UPoseableMeshComponent
{
    GENERATED_BODY()
public:
    UMRFPSAvatarComponent();
    virtual ~UMRFPSAvatarComponent() override;
    virtual void BeginPlay() override;
    bool InitializeAvatar();
    void UpdateTracking(const FMRFPSPose& Pose,const FMRFPSArenaLayout& Layout,uint32 Calibration,float DeltaSeconds,bool LocalView);
    bool IsAvatarReady() const;
    FVector SolvedWorldPosition(FName Bone) const;
    float GetTrackingErrorCm() const { return TrackingErrorCm; }
    float GetLocomotionSpeed() const { return LocomotionSpeed; }
    float GetBodyScale() const { return BodyScale; }
private:
    UPROPERTY(Transient) TObjectPtr<UBlendSpace> Locomotion;
    UPROPERTY(Transient) TObjectPtr<UAnimSequence> Idle;
    TSharedPtr<FMRFPSAvatarRig> Rig;
    float TrackingErrorCm=0;
    float LocomotionSpeed=0;
    float BodyScale=1;
};
