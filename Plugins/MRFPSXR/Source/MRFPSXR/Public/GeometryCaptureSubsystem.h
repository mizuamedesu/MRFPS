#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GeometryCaptureSubsystem.generated.h"

namespace MRFPSGeometry
{
    constexpr int32 GridWidth = 48;
    constexpr int32 GridHeight = 36;
    constexpr int32 MaxVertices = 2048;
    constexpr int32 MaxIndices = 12288;
    constexpr double FreshSeconds = 0.75;
}

/** A current observed depth surface, not a complete room map or a static Scene snapshot. */
struct MRFPSXR_API FMRFPSEnvironmentGeometryFrame
{
    // Tracking space, UE axes (+X forward/+Y right/+Z up), centimeters.
    TArray<FVector3f> Vertices;
    TArray<uint32> Indices;
    // Local monotonic time of successful OpenXR acquisition. NOT camera exposure time.
    double CaptureMonotonicTime = 0.0;
    uint32 Sequence = 0;
    // Changes on session/tracking-space/recenter invalidation; core must recalibrate.
    uint64 TrackingSpaceEpoch = 0;
    bool bTrackingValid = false;
};

/** Android Vulkan real-HMD Environment Depth capture. No simulated/static fallback. */
UCLASS()
class MRFPSXR_API UMRFPSGeometryCaptureSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual void Deinitialize() override;

    // Caller is responsible for authenticated, calibrated local-player policy.
    // Requests the normal OS Spatial Data permission if necessary; never grants it.
    UFUNCTION(BlueprintCallable, Category="MRFPS|Environment Geometry") bool RequestCapture();
    UFUNCTION(BlueprintCallable, Category="MRFPS|Environment Geometry") void StopCapture();
    UFUNCTION(BlueprintPure, Category="MRFPS|Environment Geometry") bool IsCapturing() const;
    UFUNCTION(BlueprintPure, Category="MRFPS|Environment Geometry") FString GetSafeStatus() const;

    // Game thread only. Moves the single newest fresh frame once; false empties OutFrame.
    bool DequeueLatestFrame(FMRFPSEnvironmentGeometryFrame& OutFrame);
    // Nonzero for a supported live session, even before a frame is ready or after
    // StopCapture. Changes immediately when the coordinate system is invalidated.
    // Zero means unsupported/no session. The caller must recalibrate on change.
    uint64 GetTrackingSpaceEpoch() const;

private:
    UFUNCTION() void OnSpatialPermission(const TArray<FString>& Permissions, const TArray<bool>& GrantResults);
    UPROPERTY(Transient) TObjectPtr<UObject> PermissionRequest;
    bool bWaitingForPermission = false;
    bool bPermissionDeclined = false;
};
