#pragma once

#include "CoreMinimal.h"
#include "HeadMountedDisplayTypes.h"
#include "MRFPSLoginPointer.h"
#include "MRFPSWeaponPose.h"

namespace MRFPSHandPose
{
enum class EPositionSource : uint8 { None, Controller, OpticalHand };

// Prefer the controller; allow an optical palm if it is unavailable.
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

// This is the same right-hand selection used by local gameplay and the login
// gun. A Grip position with Aim rotation matches the firing pose; optical and
// tracked palms use the existing weapon mounting conversion.
inline bool ResolveRightWeapon(const FXRMotionControllerState& Aim,const FXRMotionControllerState& Grip,
    const FXRHandTrackingState& Hand,const FTransform& EngineToTracking,const FTransform& TrackingToWorld,
    FMRFPSWeaponPose& Out)
{
    Out=FMRFPSWeaponPose();
    const bool A=MRFPSLoginPointer::ValidPose(Aim),G=MRFPSLoginPointer::ValidPose(Grip);
    if (A || G) {
        const FVector Position=TrackingToWorld.TransformPosition(G?Grip.ControllerLocation:Aim.ControllerLocation);
        const FQuat Rotation=TrackingToWorld.TransformRotation(A?Aim.ControllerRotation:Grip.ControllerRotation);
        if (Position.ContainsNaN() || Rotation.ContainsNaN() || Rotation.SizeSquared()<.25) return false;
        Out.Source=A?(G?MRFPSLoginPointer::ESource::AimGrip:MRFPSLoginPointer::ESource::Aim):MRFPSLoginPointer::ESource::Grip;
        Out.GripWorld=FTransform(Rotation.GetNormalized(),Position); return true;
    }
    FVector Position; FRotator Palm;
    if (SelectPositionSource(false,Hand)==EPositionSource::OpticalHand &&
        RightPalmPose(Hand,EngineToTracking,TrackingToWorld,Position,Palm)) {
        Out.Source=MRFPSLoginPointer::ESource::Hand;
        Out.GripWorld=FTransform(WeaponFromHand(Palm.Quaternion()),Position); return true;
    }
    return false;
}
}
