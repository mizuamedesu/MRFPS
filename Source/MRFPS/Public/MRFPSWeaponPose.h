#pragma once

#include "CoreMinimal.h"
#include "MRFPSLoginPressState.h"

// Grip location and weapon +X aim are shared by the gun mesh, menu ray and
// authoritative firing calculation. Unreal world units are centimetres.
struct FMRFPSWeaponPose
{
    MRFPSLoginPointer::ESource Source=MRFPSLoginPointer::ESource::None;
    FTransform GripWorld=FTransform::Identity;
};

namespace MRFPSWeaponPose
{
inline FTransform Muzzle(const FTransform& Grip)
{
    const FQuat Aim=Grip.GetRotation().GetNormalized();
    return FTransform(Aim,Grip.GetLocation()+Aim.GetForwardVector()*22.f);
}
inline FTransform Visual(const FTransform& Grip,bool CustomGun)
{
    const FQuat Aim=Grip.GetRotation().GetNormalized();
    return FTransform(Aim,Grip.GetLocation()+Aim.GetForwardVector()*(CustomGun?0.f:8.f));
}
}
