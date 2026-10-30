#pragma once

#include "CoreMinimal.h"
#include "HeadMountedDisplayTypes.h"
#include "MRFPSLoginPressState.h"

namespace MRFPSLoginPointer
{
inline const TCHAR* SourceName(ESource Source)
{
    switch (Source) {
    case ESource::Aim: return TEXT("aim");
    case ESource::Grip: return TEXT("grip");
    case ESource::Hand: return TEXT("hand");
    default: return TEXT("none");
    }
}

inline bool ValidPose(const FXRMotionControllerState& State)
{
    return State.bValid && State.TrackingStatus==ETrackingStatus::Tracked &&
        !State.ControllerLocation.ContainsNaN() && !State.ControllerRotation.ContainsNaN() &&
        State.ControllerRotation.SizeSquared()>.25;
}

inline bool ValidHandPose(const FXRHandTrackingState& Hand)
{
    const int32 Palm=static_cast<int32>(EHandKeypoint::Palm);
    return Hand.bValid && Hand.TrackingStatus==ETrackingStatus::Tracked &&
        Hand.HandKeyLocations.IsValidIndex(Palm) && Hand.HandKeyRotations.IsValidIndex(Palm) &&
        !Hand.HandKeyLocations[Palm].ContainsNaN() && !Hand.HandKeyRotations[Palm].ContainsNaN() &&
        Hand.HandKeyRotations[Palm].SizeSquared()>.25;
}

inline ESource Select(bool Aim,bool Grip,bool Hand)
{
    return Aim?ESource::Aim:Grip?ESource::Grip:Hand?ESource::Hand:ESource::None;
}

// Grip is a holding pose, not an OpenXR aim pose. Its uncorrected +X axis is
// only a usable fallback; controller-specific pointing remains uncalibrated.
inline FTransform ControllerWorldPose(const FXRMotionControllerState& State,const FTransform& TrackingToWorld)
{
    return FTransform(TrackingToWorld.TransformRotation(State.ControllerRotation).GetNormalized(),
                      TrackingToWorld.TransformPosition(State.ControllerLocation));
}

// OpenXRHandTracking 5.8 returns the joint transforms in engine world space.
// Convert them through the engine tracking origin into this pawn's origin.
inline FTransform HandWorldPose(const FXRHandTrackingState& Hand,const FTransform& EngineToTracking,
                                const FTransform& TrackingToWorld)
{
    const int32 Palm=static_cast<int32>(EHandKeypoint::Palm);
    const FQuat Rotation=TrackingToWorld.TransformRotation(EngineToTracking.TransformRotation(Hand.HandKeyRotations[Palm]));
    return FTransform(Rotation.GetNormalized(),TrackingToWorld.TransformPosition(EngineToTracking.TransformPosition(Hand.HandKeyLocations[Palm])));
}

}
