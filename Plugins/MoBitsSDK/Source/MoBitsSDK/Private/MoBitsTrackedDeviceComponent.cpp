#include "MoBitsTrackedDeviceComponent.h"
#include "MoBitsConnectionComponent.h"
#include "HAL/PlatformTime.h"

UMoBitsTrackedDeviceComponent::UMoBitsTrackedDeviceComponent()
{
    PrimaryComponentTick.bCanEverTick = true; PrimaryComponentTick.bTickEvenWhenPaused = true;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork; bAutoActivate = true;
}
void UMoBitsTrackedDeviceComponent::ResetTrackingOffset()
{ bInitialized = false; OriginRevision = SessionGeneration = 0; RotationOffset = FQuat::Identity; PositionOffset = FVector::ZeroVector; }
void UMoBitsTrackedDeviceComponent::Deactivate()
{ bIsTracked = false; bInitialized = false; Super::Deactivate(); }
void UMoBitsTrackedDeviceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaTime, TickType, TickFunction);
    if (LastConnection.Get() != Connection || LastDeviceId != DeviceId)
    {
        if (LastConnection.IsValid()) RemoveTickPrerequisiteComponent(LastConnection.Get());
        if (Connection) AddTickPrerequisiteComponent(Connection);
        LastConnection = Connection; LastDeviceId = DeviceId; ResetTrackingOffset();
    }
    FMoBitsDevicePose Pose; FMoBitsMotionFrame Frame;
    bIsTracked = IsActive() && Connection && Connection->TryGetDevicePose(DeviceId, Pose) && Connection->GetLatestFrame(Frame);
    if (!bIsTracked) { bInitialized = false; return; }
    const FTransform Origin = MotionOrigin ? MotionOrigin->GetComponentTransform() : FTransform::Identity;
    const FQuat SourceRotation = (Origin.GetRotation() * Pose.Rotation).GetNormalized();
    const FVector SourcePosition = Origin.TransformPosition(Pose.PositionCm);
    const int64 NewGeneration = Connection->GetSessionGeneration();
    if (OriginRevision != 0 && (OriginRevision != Frame.OriginRevision || SessionGeneration != NewGeneration) && bPreservePoseOnOriginChange)
    {
        RotationOffset = GetComponentQuat() * SourceRotation.Inverse();
        if (Pose.bHasPosition) PositionOffset = GetComponentLocation() - SourcePosition;
        bInitialized = false;
    }
    const double Now = FPlatformTime::Seconds();
    const float Weight = !bInitialized || SmoothingSeconds <= 0 ? 1.f : static_cast<float>(1 - FMath::Exp(-(Now - LastUpdate) / SmoothingSeconds));
    SetWorldRotation(FQuat::Slerp(GetComponentQuat(), RotationOffset * SourceRotation, Weight).GetNormalized());
    if (bApplyEstimatedPosition && Pose.bHasPosition)
        SetWorldLocation(FMath::Lerp(GetComponentLocation(), SourcePosition + PositionOffset, Weight));
    OriginRevision = Frame.OriginRevision; SessionGeneration = NewGeneration; LastUpdate = Now; bInitialized = true;
}
