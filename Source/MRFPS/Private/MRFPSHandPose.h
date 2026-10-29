#pragma once

#include "CoreMinimal.h"
#include "HeadMountedDisplayTypes.h"

namespace MRFPSHandPose
{
// Unreal's OpenXR hand-joint palm basis has +X along the hand toward the
// fingers and +Z out of the back of the hand. The pistol mesh also points
// along +X. In particular, do not use the index fingertip as the weapon axis:
// it turns toward the palm when the player curls their finger to shoot.
inline bool RightPalmWeaponPose(const FXRHandTrackingState& Hand,const FTransform& EngineToTracking,
    const FTransform& TrackingToArena,FVector& OutPosition,FRotator& OutRotation)
{
    const int32 Palm=static_cast<int32>(EHandKeypoint::Palm);
    if (!Hand.bValid || Hand.TrackingStatus!=ETrackingStatus::Tracked ||
        !Hand.HandKeyLocations.IsValidIndex(Palm) || !Hand.HandKeyRotations.IsValidIndex(Palm) ||
        Hand.HandKeyLocations[Palm].ContainsNaN() || Hand.HandKeyRotations[Palm].ContainsNaN()) return false;
    const FVector Position=TrackingToArena.TransformPosition(EngineToTracking.TransformPosition(Hand.HandKeyLocations[Palm]));
    const FQuat Rotation=TrackingToArena.TransformRotation(EngineToTracking.TransformRotation(Hand.HandKeyRotations[Palm]));
    if (Position.ContainsNaN() || Rotation.ContainsNaN() || Rotation.SizeSquared()<.25) return false;
    OutPosition=Position;
    OutRotation=Rotation.GetNormalized().Rotator();
    return !OutRotation.ContainsNaN();
}
}
