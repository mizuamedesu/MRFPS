#pragma once

#include "CoreMinimal.h"
#include "HeadMountedDisplayTypes.h"

namespace MRFPSHandPose
{
// Unreal's OpenXR hand-joint palm basis has +X along the hand toward the
// fingers and +Z out of the back of the hand. Retain this as the hand pose;
// finger curls must not rotate the wrist or the attached weapon.
inline bool RightPalmPose(const FXRHandTrackingState& Hand,const FTransform& EngineToTracking,
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

// Palm +X runs toward the fingers and +Z out of the back of the hand. A
// right-handed pistol points across the palm (+Y), not out of its back; its
// downward grip (-Z) runs toward the wrist. This also fixes the model roll.
inline FQuat WeaponFromHand(const FQuat& HandRotation)
{
    const FQuat Hand=HandRotation.GetNormalized();
    return FRotationMatrix::MakeFromXZ(Hand.GetAxisY(),Hand.GetAxisX()).ToQuat().GetNormalized();
}

// A controller aim pose is already an aiming ray. Infer the hand orientation
// that holds this weapon, rather than rotating that ray a second time.
inline FQuat HandFromWeapon(const FQuat& WeaponRotation)
{
    const FQuat Offset=WeaponFromHand(FQuat::Identity);
    return (WeaponRotation.GetNormalized()*Offset.Inverse()).GetNormalized();
}
}
