#include "GeometryCaptureSubsystem.h"
#include "GeometryRuntime.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GeometryCaptureSubsystem)

#if PLATFORM_ANDROID && !UE_SERVER
#include "AndroidPermissionCallbackProxy.h"
#include "AndroidPermissionFunctionLibrary.h"
namespace { const FString SpatialPermission = TEXT("com.oculus.permission.USE_SCENE"); }
#endif

void UMRFPSGeometryCaptureSubsystem::Deinitialize()
{
    StopCapture();
    Super::Deinitialize();
}

void UMRFPSGeometryCaptureSubsystem::RequestCapture()
{
#if PLATFORM_ANDROID && !UE_SERVER
    if (!IsInGameThread() || bWaitingForPermission || !MRFPSGeometryRuntime::CanRequest()) return;
    if (UAndroidPermissionFunctionLibrary::CheckPermission(SpatialPermission))
    {
        bPermissionDeclined = false;
        MRFPSGeometryRuntime::SetRequested(true);
        return;
    }
    bWaitingForPermission = true;
    bPermissionDeclined = false;
    // Bind before opening UI; a concurrent legitimate grant may complete immediately.
    auto* Proxy = UAndroidPermissionCallbackProxy::GetInstance();
    PermissionRequest = Proxy;
    Proxy->OnPermissionsGrantedDynamicDelegate.AddUniqueDynamic(this, &UMRFPSGeometryCaptureSubsystem::OnSpatialPermission);
    UAndroidPermissionFunctionLibrary::AcquirePermissions({SpatialPermission});
#endif
}

void UMRFPSGeometryCaptureSubsystem::OnSpatialPermission(const TArray<FString>& Permissions, const TArray<bool>& GrantResults)
{
#if PLATFORM_ANDROID && !UE_SERVER
    if (!bWaitingForPermission || !Permissions.Contains(SpatialPermission)) return;
    if (auto* Proxy = Cast<UAndroidPermissionCallbackProxy>(PermissionRequest))
        Proxy->OnPermissionsGrantedDynamicDelegate.RemoveDynamic(this, &UMRFPSGeometryCaptureSubsystem::OnSpatialPermission);
    PermissionRequest = nullptr;
    bWaitingForPermission = false;
    const bool bGranted = UAndroidPermissionFunctionLibrary::CheckPermission(SpatialPermission);
    bPermissionDeclined = !bGranted;
    if (bGranted) MRFPSGeometryRuntime::SetRequested(true);
#endif
}

void UMRFPSGeometryCaptureSubsystem::StopCapture()
{
    if (!IsInGameThread()) return;
#if PLATFORM_ANDROID && !UE_SERVER
    if (auto* Proxy = Cast<UAndroidPermissionCallbackProxy>(PermissionRequest))
        Proxy->OnPermissionsGrantedDynamicDelegate.RemoveDynamic(this, &UMRFPSGeometryCaptureSubsystem::OnSpatialPermission);
#endif
    PermissionRequest = nullptr;
    bWaitingForPermission = bPermissionDeclined = false;
    MRFPSGeometryRuntime::SetRequested(false);
}

bool UMRFPSGeometryCaptureSubsystem::IsCapturing() const { return MRFPSGeometryRuntime::IsFresh(); }
uint64 UMRFPSGeometryCaptureSubsystem::GetTrackingSpaceEpoch() const { return MRFPSGeometryRuntime::TrackingSpaceEpoch(); }

FString UMRFPSGeometryCaptureSubsystem::GetSafeStatus() const
{
    if (bWaitingForPermission) return TEXT("Waiting for Spatial Data permission.");
    if (bPermissionDeclined) return TEXT("Spatial Data permission was declined.");
    return MRFPSGeometryRuntime::Status();
}

bool UMRFPSGeometryCaptureSubsystem::DequeueLatestFrame(FMRFPSEnvironmentGeometryFrame& OutFrame)
{
    OutFrame = FMRFPSEnvironmentGeometryFrame();
    return IsInGameThread() && MRFPSGeometryRuntime::Dequeue(OutFrame);
}
