#pragma once

#include "CoreMinimal.h"

// A login panel is a flat, camera-attached overlay. Intersect the actual
// visible quad, not its extremely thin Chaos collision box or the stage.
namespace MRFPSLoginRay
{
enum class EResult : uint8 { Hit, Invalid, Parallel, Behind, TooFar, Outside };

inline EResult Intersect(const FTransform& Panel,const FVector2D& Size,const FVector2D& Pivot,
    const FVector& Start,const FVector& Direction,double Maximum,FVector& Impact,FVector& Normal,double& Distance)
{
    const FVector Scale=Panel.GetScale3D();
    if (Panel.ContainsNaN() || Start.ContainsNaN() || Direction.ContainsNaN() ||
        !FMath::IsFinite(Maximum) || Maximum<=0 || !FMath::IsFinite(Size.X) || !FMath::IsFinite(Size.Y) ||
        !FMath::IsFinite(Pivot.X) || !FMath::IsFinite(Pivot.Y) || Size.X<=0 || Size.Y<=0 ||
        FMath::Abs(Scale.X)<UE_SMALL_NUMBER || FMath::Abs(Scale.Y)<UE_SMALL_NUMBER || FMath::Abs(Scale.Z)<UE_SMALL_NUMBER)
        return EResult::Invalid;
    const FVector Unit=Direction.GetSafeNormal();
    if (Unit.IsNearlyZero()) return EResult::Invalid;
    const FVector LocalStart=Panel.InverseTransformPosition(Start);
    const FVector LocalDirection=Panel.InverseTransformVector(Unit);
    if (FMath::Abs(LocalDirection.X)<UE_SMALL_NUMBER) return EResult::Parallel;
    Distance=-LocalStart.X/LocalDirection.X;
    if (Distance<0) return EResult::Behind;
    if (Distance>Maximum) return EResult::TooFar;
    const FVector Point=LocalStart+LocalDirection*Distance;
    const double U=-Point.Y+Size.X*Pivot.X, V=-Point.Z+Size.Y*Pivot.Y;
    if (U<0 || U>=Size.X || V<0 || V>=Size.Y) return EResult::Outside;
    Impact=Start+Unit*Distance;
    Normal=Panel.GetUnitAxis(EAxis::X);
    if (FVector::DotProduct(Normal,Unit)>0) Normal=-Normal;
    return EResult::Hit;
}
}
