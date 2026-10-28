#pragma once
#include "CoreMinimal.h"
#include "MRFPSArenaLayout.generated.h"

namespace MRFPSArenaLimits
{
    constexpr double MaxCoordinateCm=16000;
    constexpr double MinDimensionCm=200;
    constexpr double MaxDimensionCm=2000;
}

/** Fixed configured rectangle in the existing shared world basis. No tracking scale. */
USTRUCT(BlueprintType)
struct MRFPS_API FMRFPSArenaLayout
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) float WidthCm=450;
    UPROPERTY(BlueprintReadOnly) float LengthCm=700;
    UPROPERTY(BlueprintReadOnly) FVector Center=FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly) float YawDegrees=0;
    UPROPERTY() uint32 Revision=1;
    UPROPERTY() uint32 RegistrationRevision=0;
    UPROPERTY(BlueprintReadOnly) bool bCalibrated=false;
    bool IsSane() const;
    FVector ToLocal(const FVector& World) const;
    FVector ToWorld(const FVector& Local) const;
    bool ContainsPlanar(const FVector& World,double MarginCm=0) const;
    bool ContainsHead(const FVector& World) const;
    bool ContainsGeometry(const FVector& World) const;
    FVector NominalStart(int32 Slot,double HeadHeightCm=170) const;
    FRotator StartRotation(int32 Slot) const;
    FVector ClampPlanar(const FVector& World,double MarginCm=5) const;
    /** Explicit admin operation only. XY midpoint and P0->P1 Y heading; size
     * stays fixed. Z comes from the registered floor plane, never head heights. */
    bool CalibrateFromAnchors(const FVector& A0,const FVector& A1,double RegisteredFloorZ,
        uint32 SharedRegistrationRevision,FString& SafeError);
    bool SetDimensions(double Width,double Length,FString& SafeError);
};
