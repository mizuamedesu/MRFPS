#include "GeometryRuntime.h"
#include "GeometryDepthMath.h"
#include "GeometrySurface.h"

#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

#if PLATFORM_ANDROID && !UE_SERVER
#include "GeometryVulkan.h"
#include "IOpenXRExtensionPlugin.h"
#include "IOpenXRHMDModule.h"
#include "MRFPSXRModule.h"
#include "OpenXRCore.h"
#include "HAL/IConsoleManager.h"
#include "RenderingThread.h"
#include "Engine/Engine.h"
#include "IXRTrackingSystem.h"
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr_platform.h>

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSGeometry, Log, All);

namespace
{
TAutoConsoleVariable<float> CaptureRate(TEXT("mrfps.Geometry.CaptureHz"), 2.0f,
    TEXT("Real environment-depth acquisition rate, clamped to 0.5..4 Hz."), ECVF_Default);

struct FGeometryMailbox
{
    FCriticalSection Mutex;
    bool bSupported = false;
    bool bRequested = false;
    uint64 Request = 0;
    uint64 Epoch = 1;
    double LastFrameAt = 0.0;
    FString State = TEXT("Environment depth is unavailable.");
    TOptional<FMRFPSEnvironmentGeometryFrame> Latest;
    void Clear() { Latest.Reset(); LastFrameAt = 0.0; }
};
FGeometryMailbox Mailbox;

bool FreshAt(double Time)
{
    return MRFPSGeometryMath::IsFresh(Time, FPlatformTime::Seconds());
}

void SetState(const TCHAR* State, bool bClear = true)
{
    FScopeLock Lock(&Mailbox.Mutex);
    Mailbox.State = State;
    if (bClear) Mailbox.Clear();
}

bool FinitePose(const XrPosef& Pose)
{
    const XrQuaternionf& Q = Pose.orientation;
    const XrVector3f& P = Pose.position;
    const float LengthSquared = Q.x*Q.x + Q.y*Q.y + Q.z*Q.z + Q.w*Q.w;
    return FMath::IsFinite(P.x) && FMath::IsFinite(P.y) && FMath::IsFinite(P.z) &&
        FMath::IsFinite(LengthSquared) && FMath::Abs(LengthSquared - 1.0f) < 0.01f;
}

// Resolve the same native reference-space type UE actually uses. The supplied
// engine XrSpace is used synchronously only while UpdateDeviceLocations holds it
// alive. It is NEVER retained as a usable handle on the submission thread.
XrReferenceSpaceType EngineReferenceType()
{
    if (!IsInGameThread() || !GEngine || !GEngine->XRSystem || !GEngine->XRSystem->GetIOpenXRHMD())
        return XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
    switch (GEngine->XRSystem->GetTrackingOrigin())
    {
    case EHMDTrackingOrigin::Local: return XR_REFERENCE_SPACE_TYPE_LOCAL;
    case EHMDTrackingOrigin::LocalFloor: return XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
    case EHMDTrackingOrigin::Stage: return XR_REFERENCE_SPACE_TYPE_STAGE;
    default: return XR_REFERENCE_SPACE_TYPE_MAX_ENUM; // Unsupported custom origins fail closed.
    }
}

class FMRFPSGeometryExtension final : public IOpenXRExtensionPlugin
{
public:
    virtual FString GetDisplayName() override { return TEXT("MRFPS real environment geometry"); }
    virtual bool GetOptionalExtensions(TArray<const ANSICHAR*>& Extensions) override
    {
        Extensions.Add(XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME);
        return true;
    }
    virtual const void* OnCreateInstance(IOpenXRHMDModule* Module, const void* Next) override
    {
        OpenXRModule = Module;
        return Next;
    }
    virtual void PostCreateInstance(XrInstance Value) override
    {
        Instance = Value;
        bEnabled = OpenXRModule && OpenXRModule->IsExtensionEnabled(ANSI_TO_TCHAR(XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME));
        if (!bEnabled) { SetState(TEXT("Runtime does not support real environment depth.")); return; }
        bLoaded = Load("xrCreateEnvironmentDepthProviderMETA", CreateProvider) &&
            Load("xrDestroyEnvironmentDepthProviderMETA", DestroyProvider) &&
            Load("xrStartEnvironmentDepthProviderMETA", StartProvider) &&
            Load("xrStopEnvironmentDepthProviderMETA", StopProvider) &&
            Load("xrCreateEnvironmentDepthSwapchainMETA", CreateSwapchain) &&
            Load("xrDestroyEnvironmentDepthSwapchainMETA", DestroySwapchain) &&
            Load("xrEnumerateEnvironmentDepthSwapchainImagesMETA", EnumerateImages) &&
            Load("xrGetEnvironmentDepthSwapchainStateMETA", GetSwapchainState) &&
            Load("xrAcquireEnvironmentDepthImageMETA", AcquireImage) &&
            Load("xrSetEnvironmentDepthHandRemovalMETA", SetHandRemoval);
        UE_LOG(LogMRFPSGeometry, Display, TEXT("XR_META_environment_depth functions: %s"), bLoaded ? TEXT("ready") : TEXT("unavailable"));
    }
    virtual void PostGetSystem(XrInstance Value, XrSystemId System) override
    {
        if (!bEnabled || !bLoaded) return;
        XrSystemEnvironmentDepthPropertiesMETA Depth{XR_TYPE_SYSTEM_ENVIRONMENT_DEPTH_PROPERTIES_META};
        XrSystemProperties Properties{XR_TYPE_SYSTEM_PROPERTIES}; Properties.next = &Depth;
        bSystemSupported = OpenXRDynamicAPI::xrGetSystemProperties(Value, System, &Properties) == XR_SUCCESS && Depth.supportsEnvironmentDepth == XR_TRUE;
        bHandRemovalSupported = Depth.supportsHandRemoval == XR_TRUE;
    }
    virtual void PostCreateSession(XrSession Value) override
    {
        FScopeLock Lock(&Mutex);
        Session = Value;
        bFaulted = bWasTracked = bSessionActive = false;
        bPresenceAllowsCapture = true;
        bSpaceRefresh = true; bReferenceChangePending = false; ReferenceChangeAt = OwnedSpaceValidFrom = 0;
        EngineSpaceId = XR_NULL_HANDLE;
        NextAcquireAt = 0.0;
        const bool bVulkan = GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::Vulkan;
        if (bLoaded && bSystemSupported && bVulkan)
        {
            XrReferenceSpaceCreateInfo Info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            Info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; Info.poseInReferenceSpace.orientation.w = 1.0f;
            if (OpenXRDynamicAPI::xrCreateReferenceSpace(Session, &Info, &ViewSpace) != XR_SUCCESS) ViewSpace = XR_NULL_HANDLE;
        }
        FScopeLock Shared(&Mailbox.Mutex);
        Mailbox.bSupported = bLoaded && bSystemSupported && bVulkan && FMRFPSGeometryVulkan::IsDeviceUsable() && ViewSpace != XR_NULL_HANDLE;
        Mailbox.bRequested = false; ++Mailbox.Request;
        Mailbox.State = Mailbox.bSupported ? TEXT("Environment geometry capture stopped.") : TEXT("Real Vulkan environment depth is unavailable.");
        ++Mailbox.Epoch; Mailbox.Clear();
    }
    virtual void UpdateDeviceLocations(XrSession Value, XrTime DisplayTime, XrSpace Space) override
    {
        const XrReferenceSpaceType Type = EngineReferenceType();
        FScopeLock Lock(&Mutex);
        if (Value != Session || !ViewSpace || !Space || DisplayTime <= 0) return;
        if (Space != EngineSpaceId || Type != ReferenceType)
        {
            EngineSpaceId = Space; // Comparison-only identity, never dereferenced later.
            ReferenceType = Type;
            bSpaceRefresh = true;
            DestroyTrackingSpaces();
            BumpEpoch();
        }
        if (!bSpaceRefresh && TrackingSpace && !MatchesEngineSpace(Space, DisplayTime))
        {
            // Also covers opaque-handle reuse and base offsets changed without
            // a reference-space event. No identity/type-only assumption is made.
            bSpaceRefresh = true; DestroyTrackingSpaces(); BumpEpoch();
        }
        if (!bSpaceRefresh || (ReferenceChangeAt > 0 && DisplayTime < ReferenceChangeAt)) return;
        if (bReferenceChangePending)
        {
            // Invalidate again at the effective change time: a calibration made
            // between the pending event and that time cannot survive the jump.
            bReferenceChangePending = false; ReferenceChangeAt = 0; BumpEpoch();
        }
        if (Type == XR_REFERENCE_SPACE_TYPE_MAX_ENUM)
        { SetState(TEXT("This tracking origin is unsupported for environment geometry.")); return; }
        if (!BaseSpace)
        {
            XrReferenceSpaceCreateInfo BaseInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            BaseInfo.referenceSpaceType = Type; BaseInfo.poseInReferenceSpace.orientation.w = 1.0f;
            if (OpenXRDynamicAPI::xrCreateReferenceSpace(Session, &BaseInfo, &BaseSpace) != XR_SUCCESS)
            { BaseSpace = XR_NULL_HANDLE; SetState(TEXT("Waiting for a supported tracking origin.")); return; }
        }
        // Both spaces have the SAME native reference type. The relative offset
        // includes UE's current base/recenter transform without sampling a head
        // pose or reading a private FOpenXRHMD tracking-space object.
        XrSpaceLocation Offset{XR_TYPE_SPACE_LOCATION};
        constexpr auto Valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (OpenXRDynamicAPI::xrLocateSpace(Space, BaseSpace, DisplayTime, &Offset) != XR_SUCCESS ||
            (Offset.locationFlags & Valid) != Valid || !FinitePose(Offset.pose))
        { SetState(TEXT("Waiting for a valid tracking-space transform.")); return; }
        XrReferenceSpaceCreateInfo CloneInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        CloneInfo.referenceSpaceType = Type; CloneInfo.poseInReferenceSpace = Offset.pose;
        if (OpenXRDynamicAPI::xrCreateReferenceSpace(Session, &CloneInfo, &TrackingSpace) != XR_SUCCESS)
        { TrackingSpace = XR_NULL_HANDLE; SetState(TEXT("Cannot retain the current tracking origin.")); return; }
        if (!MatchesEngineSpace(Space, DisplayTime))
        {
            OpenXRDynamicAPI::xrDestroySpace(TrackingSpace); TrackingSpace = XR_NULL_HANDLE;
            SetState(TEXT("Tracking-space alignment could not be verified.")); return;
        }
        OwnedSpaceValidFrom = DisplayTime;
        ReferenceChangeAt = 0; bSpaceRefresh = false;
    }
    virtual void OnEvent(XrSession Value, const XrEventDataBaseHeader* Event) override
    {
        if (!Event) return;
        FScopeLock Lock(&Mutex);
        if (Value != Session) return;
        if (Event->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
        {
            const auto* Change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(Event);
            if (Change->session != Session) return;
            ReferenceChangeAt = FMath::Max(ReferenceChangeAt, Change->changeTime);
            bSpaceRefresh = bReferenceChangePending = true;
            DestroyTrackingSpaces();
            BumpEpoch();
            SetState(TEXT("Tracking origin changed; geometry requires recalibration."));
        }
        else if (Event->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const auto* State = reinterpret_cast<const XrEventDataSessionStateChanged*>(Event);
            if (State->session != Session) return;
            bSessionActive = State->state == XR_SESSION_STATE_SYNCHRONIZED || State->state == XR_SESSION_STATE_VISIBLE || State->state == XR_SESSION_STATE_FOCUSED;
            if (State->state == XR_SESSION_STATE_STOPPING || State->state == XR_SESSION_STATE_LOSS_PENDING ||
                State->state == XR_SESSION_STATE_EXITING || State->state == XR_SESSION_STATE_IDLE)
            { bWasTracked = false; BumpEpoch(); SetState(TEXT("Headset session is not running.")); }
        }
        else if (Event->type == XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT)
        {
            const auto* Presence = reinterpret_cast<const XrEventDataUserPresenceChangedEXT*>(Event);
            if (Presence->session != Session) return;
            bPresenceAllowsCapture = Presence->isUserPresent == XR_TRUE;
            if (!bPresenceAllowsCapture)
            { bWasTracked = false; BumpEpoch(); SetState(TEXT("Headset is not being worn.")); }
        }
    }

    virtual void PostBeginFrame_RHIThread(XrTime DisplayTime) override
    {
        // UE5.8's Vulkan OpenXR bridge calls this INSIDE the public
        // RHIRunOnQueue(Graphics) callback, through OnBeginRendering_RHIThread.
        // Its submission serialization covers xrAcquire and our vkQueueSubmit.
        // Do not move this work to a generic render callback or nest another
        // RHIRunOnQueue: that could deadlock or run after xrEndFrame.
        FScopeLock Lock(&Mutex);
        if (!Session) return;
        bool bRequested = false; uint64 Request = 0; uint64 Epoch = 0;
        {
            FScopeLock Shared(&Mailbox.Mutex);
            bRequested = Mailbox.bRequested; Request = Mailbox.Request; Epoch = Mailbox.Epoch;
        }
        if (SeenRequest != Request)
        {
            SeenRequest = Request; bFaulted = false;
            if (Provider) DestroyResources();
        }
        XrSpaceLocation Head{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags Required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        const bool bTracked = bSessionActive && bPresenceAllowsCapture && !bSpaceRefresh &&
            DisplayTime >= OwnedSpaceValidFrom && ViewSpace && TrackingSpace &&
            OpenXRDynamicAPI::xrLocateSpace(ViewSpace, TrackingSpace, DisplayTime, &Head) == XR_SUCCESS &&
            (Head.locationFlags & Required) == Required && FinitePose(Head.pose);
        if (!bTracked && bWasTracked) { BumpEpoch(); Epoch = CurrentEpoch(); }
        bWasTracked = bTracked;
        TArray<FVector4f> Points;
        const auto Poll = GPU.Poll(Points);
        if (Poll == FMRFPSGeometryVulkan::EPoll::Error)
        { bFaulted = true; SetState(TEXT("Environment depth GPU readback failed.")); return; }
        if (Poll == FMRFPSGeometryVulkan::EPoll::Ready && bRequested && bTracked && PendingRequest == Request &&
            PendingFrame.TrackingSpaceEpoch == Epoch && FreshAt(PendingFrame.CaptureMonotonicTime))
        {
            FMRFPSEnvironmentGeometryFrame Frame = MoveTemp(PendingFrame);
            if (MRFPSGeometrySurface::ProjectionMatches(Points, PendingView, Width, Height) &&
                MRFPSGeometrySurface::Build(Points, Frame))
            {
                FScopeLock Shared(&Mailbox.Mutex);
                if (Mailbox.bRequested && Mailbox.Request == Request && Mailbox.Epoch == Epoch)
                {
                    Mailbox.LastFrameAt = Frame.CaptureMonotonicTime;
                    Mailbox.Latest.Emplace(MoveTemp(Frame));
                    Mailbox.State = TEXT("Real environment geometry is fresh.");
                }
            }
            else SetState(TEXT("Waiting for usable observed depth surfaces."));
        }
        if (!bRequested)
        {
            if (bStarted) { StopProvider(Provider); bStarted = false; }
            return;
        }
        if (!bTracked) { SetState(TEXT("Waiting for real positional headset tracking.")); return; }
        if (!FMRFPSXRModule::IsPassthroughRunning()) { SetState(TEXT("Waiting for native passthrough.")); return; }
        if (bFaulted) return;
        if (!Provider && !CreateResources()) { bFaulted = true; return; }
        if (!bStarted)
        {
            if (StartProvider(Provider) != XR_SUCCESS) { bFaulted = true; SetState(TEXT("Cannot start real environment depth.")); return; }
            bStarted = true;
        }
        const double Now = FPlatformTime::Seconds();
        if (GPU.HasPending() || Now < NextAcquireAt) return;
        float Hz = CaptureRate.GetValueOnAnyThread(); if (!FMath::IsFinite(Hz)) Hz = 2.0f;
        NextAcquireAt = Now + 1.0 / FMath::Clamp(Hz, 0.5f, 4.0f);
        XrEnvironmentDepthImageAcquireInfoMETA Info{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META};
        Info.space = TrackingSpace; Info.displayTime = DisplayTime;
        XrEnvironmentDepthImageMETA Image{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META};
        Image.views[0].type = Image.views[1].type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META;
        const XrResult Result = AcquireImage(Provider, &Info, &Image);
        if (Result == XR_ENVIRONMENT_DEPTH_NOT_AVAILABLE_META)
        { SetState(TEXT("Waiting for a real environment depth image.")); return; }
        // NOT_AVAILABLE is a positive success code with UNCHANGED output. Only
        // XR_SUCCESS supplies a real image; XR_SUCCEEDED alone is insufficient.
        if (Result != XR_SUCCESS || Image.swapchainIndex >= uint32(Images.Num()))
        { bFaulted = true; SetState(TEXT("Real environment depth acquisition failed.")); return; }
        const auto& View = Image.views[0];
        if (!FinitePose(View.pose) || !MRFPSGeometryMath::ValidPlanes(Image.nearZ, Image.farZ) || Sequence == MAX_uint32)
        { SetState(TEXT("Environment depth metadata is invalid.")); return; }
        FMRFPSGeometryGpuParameters Parameters{};
        Parameters.TanFov[0] = FMath::Tan(View.fov.angleLeft); Parameters.TanFov[1] = FMath::Tan(View.fov.angleRight);
        Parameters.TanFov[2] = FMath::Tan(View.fov.angleUp); Parameters.TanFov[3] = FMath::Tan(View.fov.angleDown);
        for (float Value : Parameters.TanFov) if (!FMath::IsFinite(Value)) { SetState(TEXT("Environment depth frustum is invalid.")); return; }
        if (Parameters.TanFov[0] >= Parameters.TanFov[1] || Parameters.TanFov[3] >= Parameters.TanFov[2])
        { SetState(TEXT("Environment depth frustum is invalid.")); return; }
        Parameters.DepthLimits[0] = Image.nearZ; Parameters.DepthLimits[1] = Image.farZ;
        Parameters.DepthLimits[2] = MRFPSGeometryMath::MinimumDepthMeters;
        Parameters.DepthLimits[3] = MRFPSGeometryMath::MaximumDepthMeters;
        Parameters.Orientation[0] = View.pose.orientation.x; Parameters.Orientation[1] = View.pose.orientation.y;
        Parameters.Orientation[2] = View.pose.orientation.z; Parameters.Orientation[3] = View.pose.orientation.w;
        Parameters.Position[0] = View.pose.position.x; Parameters.Position[1] = View.pose.position.y; Parameters.Position[2] = View.pose.position.z;
        Parameters.Dimensions[0] = Width; Parameters.Dimensions[1] = Height;
        Parameters.Dimensions[2] = MRFPSGeometry::GridWidth; Parameters.Dimensions[3] = MRFPSGeometry::GridHeight;
        PendingView = {Parameters.TanFov[0], Parameters.TanFov[1], Parameters.TanFov[2], Parameters.TanFov[3],
            Image.nearZ, Image.farZ, View.pose.orientation.x, View.pose.orientation.y, View.pose.orientation.z,
            View.pose.orientation.w, View.pose.position.x, View.pose.position.y, View.pose.position.z};
        if (!MRFPSGeometryMath::ValidView(PendingView)) { SetState(TEXT("Environment depth metadata is invalid.")); return; }
        PendingFrame = FMRFPSEnvironmentGeometryFrame();
        PendingFrame.CaptureMonotonicTime = FPlatformTime::Seconds();
        PendingFrame.Sequence = ++Sequence; PendingFrame.TrackingSpaceEpoch = Epoch; PendingFrame.bTrackingValid = true;
        PendingRequest = Request;
        if (!GPU.Submit(Image.swapchainIndex, Parameters))
        { bFaulted = true; SetState(TEXT("Environment depth compute submission failed.")); }
    }

    virtual void OnDestroySession(XrSession Value) override
    {
        // UE has flushed rendering before invoking this under SessionHandleMutex.
        // A nested FlushRenderingCommands here could deadlock that mutex.
        FScopeLock Lock(&Mutex);
        if (Value != Session) return;
        DestroyResources();
        DestroyTrackingSpaces();
        if (ViewSpace) OpenXRDynamicAPI::xrDestroySpace(ViewSpace);
        ViewSpace = EngineSpaceId = XR_NULL_HANDLE; Session = XR_NULL_HANDLE;
        bSessionActive = bWasTracked = false;
        FScopeLock Shared(&Mailbox.Mutex);
        Mailbox.bSupported = false; Mailbox.bRequested = false; ++Mailbox.Request; ++Mailbox.Epoch; Mailbox.Clear();
        Mailbox.State = TEXT("Headset session ended.");
    }

    void Shutdown()
    {
        FlushRenderingCommands();
        FScopeLock Lock(&Mutex);
        DestroyResources();
        DestroyTrackingSpaces();
        if (ViewSpace && Session) OpenXRDynamicAPI::xrDestroySpace(ViewSpace);
        ViewSpace = EngineSpaceId = XR_NULL_HANDLE; Session = XR_NULL_HANDLE;
        FScopeLock Shared(&Mailbox.Mutex);
        Mailbox.bSupported = Mailbox.bRequested = false; ++Mailbox.Request; ++Mailbox.Epoch; Mailbox.Clear();
    }

private:
    template<typename T> bool Load(const ANSICHAR* Name, T& Function)
    {
        Function = nullptr;
        return OpenXRDynamicAPI::xrGetInstanceProcAddr(Instance, Name, reinterpret_cast<PFN_xrVoidFunction*>(&Function)) == XR_SUCCESS && Function;
    }
    uint64 CurrentEpoch() { FScopeLock Shared(&Mailbox.Mutex); return Mailbox.Epoch; }
    void BumpEpoch() { FScopeLock Shared(&Mailbox.Mutex); ++Mailbox.Epoch; Mailbox.Clear(); }

    bool MatchesEngineSpace(XrSpace ActualEngineSpace, XrTime Time)
    {
        // Called ONLY synchronously from UpdateDeviceLocations while UE retains
        // ActualEngineSpace. The submission path never uses that borrowed handle.
        XrSpaceLocation Delta{XR_TYPE_SPACE_LOCATION};
        constexpr auto Valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (OpenXRDynamicAPI::xrLocateSpace(TrackingSpace, ActualEngineSpace, Time, &Delta) != XR_SUCCESS ||
            (Delta.locationFlags & Valid) != Valid) return false;
        const auto& Q=Delta.pose.orientation; const auto& P=Delta.pose.position;
        return MRFPSGeometryMath::IsIdentityOffset(Q.x,Q.y,Q.z,Q.w,P.x,P.y,P.z);
    }

    void DestroyTrackingSpaces()
    {
        // Acquired image positions are already baked into our own push constants;
        // no queued Vulkan command refers to either XrSpace. Mutex excludes acquire.
        if (TrackingSpace) OpenXRDynamicAPI::xrDestroySpace(TrackingSpace);
        if (BaseSpace) OpenXRDynamicAPI::xrDestroySpace(BaseSpace);
        TrackingSpace = BaseSpace = XR_NULL_HANDLE;
        OwnedSpaceValidFrom = 0;
    }

    bool CreateResources()
    {
        XrEnvironmentDepthProviderCreateInfoMETA ProviderInfo{XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META};
        if (CreateProvider(Session, &ProviderInfo, &Provider) != XR_SUCCESS)
        { Provider = XR_NULL_HANDLE; SetState(TEXT("Environment depth denied or unsupported; Spatial Data permission is required.")); return false; }
        if (bHandRemovalSupported)
        {
            XrEnvironmentDepthHandRemovalSetInfoMETA Hands{XR_TYPE_ENVIRONMENT_DEPTH_HAND_REMOVAL_SET_INFO_META}; Hands.enabled = XR_FALSE;
            if (SetHandRemoval(Provider, &Hands) != XR_SUCCESS)
            { DestroyResources(); SetState(TEXT("Cannot preserve observed hand depths.")); return false; }
        }
        XrEnvironmentDepthSwapchainCreateInfoMETA SwapchainInfo{XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META};
        if (CreateSwapchain(Provider, &SwapchainInfo, &Swapchain) != XR_SUCCESS)
        { DestroyResources(); SetState(TEXT("Cannot create real depth swapchain.")); return false; }
        XrEnvironmentDepthSwapchainStateMETA State{XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META};
        uint32 Count = 0;
        if (GetSwapchainState(Swapchain, &State) != XR_SUCCESS || State.width == 0 || State.height == 0 ||
            State.width > 4096 || State.height > 4096 || EnumerateImages(Swapchain, 0, &Count, nullptr) != XR_SUCCESS || Count == 0 || Count > 16)
        { DestroyResources(); SetState(TEXT("Depth swapchain exceeds supported resource bounds.")); return false; }
        Width = State.width; Height = State.height;
        Images.SetNum(Count);
        for (auto& Image : Images) { Image = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR}; }
        if (EnumerateImages(Swapchain, Count, &Count, reinterpret_cast<XrSwapchainImageBaseHeader*>(Images.GetData())) != XR_SUCCESS || Count != uint32(Images.Num()))
        { DestroyResources(); SetState(TEXT("Cannot enumerate real depth images.")); return false; }
        TArray<VkImage> Handles; Handles.Reserve(Images.Num());
        for (const auto& Image : Images) Handles.Add(Image.image);
        FString SafeError;
        if (!GPU.Initialize(*GetIVulkanDynamicRHI(), Handles, SafeError))
        { DestroyResources(); SetState(TEXT("Cannot initialize bounded Vulkan depth compute.")); return false; }
        UE_LOG(LogMRFPSGeometry, Display, TEXT("Real D16 environment depth ready: %ux%u, %d images; grid %dx%d"),
            Width, Height, Images.Num(), MRFPSGeometry::GridWidth, MRFPSGeometry::GridHeight);
        return true;
    }
    void DestroyResources()
    {
        const bool bCompleted = GPU.Shutdown();
        if (!bCompleted)
        {
            // Their GPU use is unproven after the bounded wait. Do not explicitly
            // destroy runtime images/providers either. xrDestroySession/device
            // teardown owns its child objects; the depth path stays disabled.
            Images.Reset(); PendingFrame = {};
            Provider = XR_NULL_HANDLE; Swapchain = XR_NULL_HANDLE; bStarted = false; bFaulted = true;
            FScopeLock Shared(&Mailbox.Mutex);
            Mailbox.bSupported = Mailbox.bRequested = false; ++Mailbox.Request; ++Mailbox.Epoch; Mailbox.Clear();
            Mailbox.State = TEXT("Environment geometry stopped after a GPU failure.");
            return;
        }
        if (bStarted && Provider && StopProvider) StopProvider(Provider);
        bStarted = false;
        Images.Reset(); PendingFrame = FMRFPSEnvironmentGeometryFrame();
        if (Swapchain && DestroySwapchain) DestroySwapchain(Swapchain);
        Swapchain = XR_NULL_HANDLE;
        if (Provider && DestroyProvider) DestroyProvider(Provider);
        Provider = XR_NULL_HANDLE;
    }

    FCriticalSection Mutex;
    IOpenXRHMDModule* OpenXRModule = nullptr;
    XrInstance Instance = XR_NULL_HANDLE;
    XrSession Session = XR_NULL_HANDLE;
    XrSpace ViewSpace = XR_NULL_HANDLE;
    XrSpace BaseSpace = XR_NULL_HANDLE;
    XrSpace TrackingSpace = XR_NULL_HANDLE;
    XrSpace EngineSpaceId = XR_NULL_HANDLE; // Comparison-only identity, NOT an owned/usable space.
    XrReferenceSpaceType ReferenceType = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
    XrTime ReferenceChangeAt = 0;
    XrTime OwnedSpaceValidFrom = 0;
    XrEnvironmentDepthProviderMETA Provider = XR_NULL_HANDLE;
    XrEnvironmentDepthSwapchainMETA Swapchain = XR_NULL_HANDLE;
    TArray<XrSwapchainImageVulkanKHR> Images;
    FMRFPSGeometryVulkan GPU;
    FMRFPSEnvironmentGeometryFrame PendingFrame;
    MRFPSGeometryMath::FView PendingView;
    uint64 PendingRequest = 0;
    uint64 SeenRequest = 0;
    uint32 Sequence = 0;
    uint32 Width = 0;
    uint32 Height = 0;
    double NextAcquireAt = 0.0;
    bool bEnabled = false;
    bool bLoaded = false;
    bool bSystemSupported = false;
    bool bHandRemovalSupported = false;
    bool bStarted = false;
    bool bFaulted = false;
    bool bWasTracked = false;
    bool bSpaceRefresh = true;
    bool bReferenceChangePending = false;
    bool bSessionActive = false;
    bool bPresenceAllowsCapture = true;
    PFN_xrCreateEnvironmentDepthProviderMETA CreateProvider = nullptr;
    PFN_xrDestroyEnvironmentDepthProviderMETA DestroyProvider = nullptr;
    PFN_xrStartEnvironmentDepthProviderMETA StartProvider = nullptr;
    PFN_xrStopEnvironmentDepthProviderMETA StopProvider = nullptr;
    PFN_xrCreateEnvironmentDepthSwapchainMETA CreateSwapchain = nullptr;
    PFN_xrDestroyEnvironmentDepthSwapchainMETA DestroySwapchain = nullptr;
    PFN_xrEnumerateEnvironmentDepthSwapchainImagesMETA EnumerateImages = nullptr;
    PFN_xrGetEnvironmentDepthSwapchainStateMETA GetSwapchainState = nullptr;
    PFN_xrAcquireEnvironmentDepthImageMETA AcquireImage = nullptr;
    PFN_xrSetEnvironmentDepthHandRemovalMETA SetHandRemoval = nullptr;
};
TUniquePtr<FMRFPSGeometryExtension> Extension;
}

namespace MRFPSGeometryRuntime
{
void Startup()
{
    Extension = MakeUnique<FMRFPSGeometryExtension>();
    Extension->RegisterOpenXRExtensionModularFeature();
}
void Shutdown()
{
    if (!Extension) return;
    Extension->UnregisterOpenXRExtensionModularFeature();
    Extension->Shutdown(); Extension.Reset();
}
bool CanRequest() { FScopeLock Lock(&Mailbox.Mutex); return Mailbox.bSupported; }
void SetRequested(bool bRequested)
{
    FScopeLock Lock(&Mailbox.Mutex);
    if (bRequested && !Mailbox.bSupported) return;
    if (Mailbox.bRequested == bRequested) return;
    Mailbox.bRequested = bRequested; ++Mailbox.Request; Mailbox.Clear();
    Mailbox.State = bRequested ? TEXT("Waiting for real environment depth.") : TEXT("Environment geometry capture stopped.");
}
bool IsFresh() { FScopeLock Lock(&Mailbox.Mutex); return Mailbox.bRequested && FreshAt(Mailbox.LastFrameAt); }
uint64 TrackingSpaceEpoch() { FScopeLock Lock(&Mailbox.Mutex); return Mailbox.bSupported ? Mailbox.Epoch : 0; }
FString Status()
{
    FScopeLock Lock(&Mailbox.Mutex);
    if (Mailbox.bRequested && Mailbox.LastFrameAt > 0 && !FreshAt(Mailbox.LastFrameAt)) return TEXT("Waiting for fresh environment geometry.");
    return Mailbox.State;
}
bool Dequeue(FMRFPSEnvironmentGeometryFrame& Frame)
{
    FScopeLock Lock(&Mailbox.Mutex);
    Frame = {};
    if (!Mailbox.bRequested) { Mailbox.Latest.Reset(); return false; }
    return MRFPSGeometrySurface::Take(Mailbox.Latest, Mailbox.Epoch, FPlatformTime::Seconds(), Frame);
}
}
#else
namespace MRFPSGeometryRuntime
{
void Startup() {}
void Shutdown() {}
bool CanRequest() { return false; }
void SetRequested(bool) {}
bool IsFresh() { return false; }
uint64 TrackingSpaceEpoch() { return 0; }
FString Status() { return TEXT("Real environment geometry capture requires an Android Vulkan headset."); }
bool Dequeue(FMRFPSEnvironmentGeometryFrame& Frame) { Frame = {}; return false; }
}
#endif
