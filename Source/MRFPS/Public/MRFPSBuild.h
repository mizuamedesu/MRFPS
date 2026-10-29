#pragma once
#include "CoreMinimal.h"
#include "MRFPSArenaLayout.h"
#include "MRFPSBuild.generated.h"

UENUM()
enum class EMRFPSBuildKind : uint8 { Wall, Crate };

UENUM()
enum class EMRFPSWallEdit : uint8 { ToggleCell, Taller, Shorter };

// One replicated value describes both the visible wall and its collision.
// Hidden rows keep their bits when a wall is shortened and later raised again.
USTRUCT()
struct FMRFPSWallShape
{
    GENERATED_BODY()
    UPROPERTY() uint32 SolidCells=0x00ffffffu;
    UPROPERTY() uint8 HeightRows=4;
    bool IsSane() const { return HeightRows>=1 && HeightRows<=8 && (SolidCells&0xff000000u)==0; }
    bool IsSolid(int32 Column,int32 Row) const
    {
        return IsSane() && Column>=0 && Column<3 && Row>=0 && Row<HeightRows && (SolidCells&(1u<<(Row*3+Column)))!=0;
    }
};

namespace MRFPSBuild
{
    constexpr double GridCm=50;
    constexpr double MaxHeightCm=500;
    constexpr int32 WallColumns=3;
    constexpr int32 WallMaxRows=8;
    constexpr double WallWidthCm=160;
    constexpr double WallRowHeightCm=50;
    constexpr double WallHalfThicknessCm=8;
    MRFPS_API bool Valid(EMRFPSBuildKind Kind);
    MRFPS_API FVector Extent(EMRFPSBuildKind Kind);
    MRFPS_API FString Label(EMRFPSBuildKind Kind);
    MRFPS_API bool Snap(const FMRFPSArenaLayout& Layout,EMRFPSBuildKind Kind,const FVector& Surface,float Yaw,FTransform& Result);
    MRFPS_API bool Fits(const FMRFPSArenaLayout& Layout,EMRFPSBuildKind Kind,const FTransform& Transform);
    MRFPS_API bool FitsWall(const FMRFPSArenaLayout& Layout,const FTransform& Transform,const FMRFPSWallShape& Shape);
    MRFPS_API FVector CellCenter(int32 Column,int32 Row);
    MRFPS_API FVector CellExtent();
}
