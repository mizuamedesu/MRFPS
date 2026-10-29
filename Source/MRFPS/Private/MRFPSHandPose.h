#pragma once

#include "CoreMinimal.h"
#include "HeadMountedDisplayTypes.h"

namespace MRFPSHandPose
{
enum class EPositionSource : uint8 { None, Controller, OpticalHand };

// Positional tracking and glove flex are independent streams. Prefer the
// controller, but allow an optical palm whenever the controller is unavailable.
inline EPositionSource SelectPositionSource(bool ControllerTracked,const FXRHandTrackingState& Hand)
{
    if (ControllerTracked) return EPositionSource::Controller;
    return Hand.bValid && Hand.TrackingStatus==ETrackingStatus::Tracked &&
        Hand.HandKeyLocations.Num()>=26 && Hand.HandKeyRotations.Num()>=26
        ? EPositionSource::OpticalHand : EPositionSource::None;
}

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

// Correct the optical-palm mounting angle by 90 degrees. The pistol mesh and
// server's aiming ray use the same +X forward basis. Controller aim is separate.
inline FQuat WeaponFromHand(const FQuat& HandRotation)
{
    const FQuat Hand=HandRotation.GetNormalized();
    return FRotationMatrix::MakeFromXZ(Hand.GetAxisX(),-Hand.GetAxisY()).ToQuat().GetNormalized();
}

// A controller aim pose is already an aiming ray. Infer the hand orientation
// that holds this weapon, rather than rotating that ray a second time.
inline FQuat HandFromWeapon(const FQuat& WeaponRotation)
{
    const FQuat Offset=WeaponFromHand(FQuat::Identity);
    return (WeaponRotation.GetNormalized()*Offset.Inverse()).GetNormalized();
}
}
