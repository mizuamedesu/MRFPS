#pragma once

#include "CoreMinimal.h"

#if PLATFORM_ANDROID && !UE_SERVER
#include "IVulkanDynamicRHI.h"

struct FMRFPSGeometryGpuParameters
{
    float TanFov[4]{};
    float DepthLimits[4]{};
    float Orientation[4]{};
    float Position[4]{};
    uint32 Dimensions[4]{};
};
static_assert(sizeof(FMRFPSGeometryGpuParameters) == 80, "GLSL push constant layout must match");

// These resources never wrap or modify the runtime-owned depth image. Queue
// access is valid only inside the existing OpenXR Vulkan submission callback.
class FMRFPSGeometryVulkan
{
public:
    FMRFPSGeometryVulkan();
    ~FMRFPSGeometryVulkan();
    bool Initialize(IVulkanDynamicRHI& RHI, TConstArrayView<VkImage> Images, FString& SafeError);
    bool Submit(uint32 ImageIndex, const FMRFPSGeometryGpuParameters& Parameters);
    enum class EPoll { Empty, Pending, Ready, Error };
    EPoll Poll(TArray<FVector4f>& Points);
    bool HasPending() const;
    static bool IsDeviceUsable();
    // Teardown only: at most two seconds for the owned fence, never device-wide
    // idle. False quarantines resources instead of freeing in-flight objects.
    bool Shutdown();
private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
#endif
