#pragma once
#include "CoreMinimal.h"
#include "MRFPSArenaLayout.h"
#include "MRFPSBuild.generated.h"

UENUM()
enum class EMRFPSBuildKind : uint8 { Wall, Floor, Stairs, Crate };

namespace MRFPSBuild
{
    constexpr double GridCm=50;
    constexpr double MaxHeightCm=500;
    MRFPS_API bool Valid(EMRFPSBuildKind Kind);
    MRFPS_API FVector Extent(EMRFPSBuildKind Kind);
    MRFPS_API FString Label(EMRFPSBuildKind Kind);
    MRFPS_API bool Snap(const FMRFPSArenaLayout& Layout,EMRFPSBuildKind Kind,const FVector& Surface,float Yaw,FTransform& Result);
    MRFPS_API bool Fits(const FMRFPSArenaLayout& Layout,EMRFPSBuildKind Kind,const FTransform& Transform);
}
