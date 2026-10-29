#pragma once

#include "CoreMinimal.h"

// Desired desktop input lives only in the local controller. It never changes
// authority state or supplies an XR pose, and expires on monotonic wall time.
struct FMRFPSLocalInputLease
{
    FVector2D Move=FVector2D::ZeroVector;
    FVector2D LookDegreesPerSecond=FVector2D::ZeroVector;
    double Until=0;
    double LastAppliedAt=0;
    bool Fire=false;
    bool Shield=false;

    bool Set(bool InFire,bool InShield,FVector2D InMove,FVector2D InLook,double Duration,double Now)
    {
        if (!FMath::IsFinite(Now) || !FMath::IsFinite(Duration) || Now<0 || Duration<.05 || Duration>1.0 ||
            !FMath::IsFinite(InMove.X) || !FMath::IsFinite(InMove.Y) ||
            !FMath::IsFinite(InLook.X) || !FMath::IsFinite(InLook.Y) ||
            FMath::Abs(InMove.X)>1 || FMath::Abs(InMove.Y)>1 ||
            FMath::Abs(InLook.X)>180 || FMath::Abs(InLook.Y)>180 || !FMath::IsFinite(Now+Duration)) return false;
        Fire=InFire; Shield=InShield; Move=InMove; LookDegreesPerSecond=InLook; Until=Now+Duration; LastAppliedAt=Now;
        return true;
    }
    bool Active(double Now) const { return FMath::IsFinite(Now) && Until>0 && Now<Until; }
    double ConsumeStep(double Now,double DeltaSeconds)
    {
        if (!Active(Now) || !FMath::IsFinite(DeltaSeconds)) return 0;
        const double Step=FMath::Min(FMath::Clamp(DeltaSeconds,0.,.05),FMath::Max(0.,Now-LastAppliedAt));
        LastAppliedAt=FMath::Max(LastAppliedAt,Now);
        return Step;
    }
    void Clear() { Fire=false; Shield=false; Move=FVector2D::ZeroVector; LookDegreesPerSecond=FVector2D::ZeroVector; Until=0; LastAppliedAt=0; }
};
