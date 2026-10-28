#include "MRFPSXRModule.h"
#include "GeometryRuntime.h"
#if !PLATFORM_MAC
#include "IOpenXRExtensionPlugin.h"
#include "IOpenXRHMDModule.h"
#include "OpenXRCore.h"
#include "Misc/ScopeLock.h"
#include "RenderingThread.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSXR, Log, All);

namespace
{
    TAtomic<bool> GPassthroughRunning(false);
}

class FMRFPSPassthroughExtension final : public IOpenXRExtensionPlugin
{
public:
    virtual FString GetDisplayName() override { return TEXT("MRFPS Quest Passthrough"); }

    virtual bool GetOptionalExtensions(TArray<const ANSICHAR*>& OutExtensions) override
    {
        OutExtensions.Add(XR_FB_PASSTHROUGH_EXTENSION_NAME);
        return true;
    }

    virtual const void* OnCreateInstance(IOpenXRHMDModule* InModule, const void* InNext) override
    {
        OpenXRModule = InModule;
        return InNext;
    }

    virtual void PostCreateInstance(XrInstance InInstance) override
    {
        Instance = InInstance;
        bExtensionEnabled = OpenXRModule && OpenXRModule->IsExtensionEnabled(ANSI_TO_TCHAR(XR_FB_PASSTHROUGH_EXTENSION_NAME));
        if (!bExtensionEnabled)
        {
            UE_LOG(LogMRFPSXR, Warning, TEXT("Runtime does not enable XR_FB_passthrough; mixed reality is unavailable"));
            return;
        }
        bFunctionsLoaded = Load("xrCreatePassthroughFB", CreatePassthrough)
            && Load("xrDestroyPassthroughFB", DestroyPassthrough)
            && Load("xrPassthroughStartFB", StartPassthrough)
            && Load("xrPassthroughPauseFB", PausePassthrough)
            && Load("xrCreatePassthroughLayerFB", CreateLayer)
            && Load("xrDestroyPassthroughLayerFB", DestroyLayer)
            && Load("xrPassthroughLayerResumeFB", ResumeLayer)
            && Load("xrPassthroughLayerPauseFB", PauseLayer)
            && Load("xrPassthroughLayerSetStyleFB", SetStyle);
    }

    virtual void PostGetSystem(XrInstance InInstance, XrSystemId InSystem) override
    {
        bSystemSupported = false;
        if (!bFunctionsLoaded) return;
        XrSystemPassthroughPropertiesFB PassthroughProperties{ XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB };
        XrSystemProperties Properties{ XR_TYPE_SYSTEM_PROPERTIES };
        Properties.next = &PassthroughProperties;
        const XrResult Result = OpenXRDynamicAPI::xrGetSystemProperties(InInstance, InSystem, &Properties);
        bSystemSupported = XR_SUCCEEDED(Result) && PassthroughProperties.supportsPassthrough == XR_TRUE;
        UE_LOG(LogMRFPSXR, Display, TEXT("XR_FB_passthrough system support: %s (result %d)"), bSystemSupported ? TEXT("yes") : TEXT("no"), int32(Result));
    }

    virtual void PostCreateSession(XrSession InSession) override
    {
        FScopeLock Lock(&Mutex);
        Session = InSession;
        bRuntimeError = bNeedsSessionRecreation = false;
        if (!bSystemSupported) return;
        XrPassthroughCreateInfoFB FeatureInfo{ XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
        // Create paused; xrPassthroughStartFB is deferred until xrBeginSession succeeds.
        XrResult Result = CreatePassthrough(Session, &FeatureInfo, &Passthrough);
        if (XR_FAILED(Result))
        {
            UE_LOG(LogMRFPSXR, Error, TEXT("xrCreatePassthroughFB failed: %d"), int32(Result));
            return;
        }
        XrPassthroughLayerCreateInfoFB LayerInfo{ XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
        LayerInfo.passthrough = Passthrough;
        LayerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
        Result = CreateLayer(Session, &LayerInfo, &Layer);
        if (XR_FAILED(Result))
        {
            UE_LOG(LogMRFPSXR, Error, TEXT("xrCreatePassthroughLayerFB failed: %d"), int32(Result));
            DestroyResources();
            return;
        }
        XrPassthroughStyleFB Style{ XR_TYPE_PASSTHROUGH_STYLE_FB };
        Style.textureOpacityFactor = 1.0f;
        Style.edgeColor = { 0, 0, 0, 0 };
        Result = SetStyle(Layer, &Style);
        if (XR_FAILED(Result))
        {
            UE_LOG(LogMRFPSXR, Error, TEXT("xrPassthroughLayerSetStyleFB failed: %d"), int32(Result));
            DestroyResources();
            return;
        }
        Composition = { XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
        Composition.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        Composition.space = XR_NULL_HANDLE;
        Composition.layerHandle = Layer;
        UE_LOG(LogMRFPSXR, Display, TEXT("Passthrough reconstruction layer created; waiting for a running session"));
    }

    virtual void OnEvent(XrSession InSession, const XrEventDataBaseHeader* Header) override
    {
        if (!Header) return;
        if (Header->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const auto* Event = reinterpret_cast<const XrEventDataSessionStateChanged*>(Header);
            FScopeLock Lock(&Mutex);
            if (Event->session != Session || InSession != Session || !Layer) return;
            if (Event->state == XR_SESSION_STATE_SYNCHRONIZED || Event->state == XR_SESSION_STATE_VISIBLE || Event->state == XR_SESSION_STATE_FOCUSED)
            {
                StartResources();
            }
            else if (Event->state == XR_SESSION_STATE_STOPPING)
            {
                GPassthroughRunning.Store(false);
                if (bLayerActive) PauseLayer(Layer);
                if (bFeatureActive) PausePassthrough(Passthrough);
                bLayerActive = bFeatureActive = false;
            }
            else if (Event->state == XR_SESSION_STATE_EXITING || Event->state == XR_SESSION_STATE_LOSS_PENDING || Event->state == XR_SESSION_STATE_IDLE)
            {
                GPassthroughRunning.Store(false);
            }
        }
        else if (Header->type == XR_TYPE_EVENT_DATA_PASSTHROUGH_STATE_CHANGED_FB)
        {
            const auto* Event = reinterpret_cast<const XrEventDataPassthroughStateChangedFB*>(Header);
            FScopeLock Lock(&Mutex);
            if (Event->flags & (XR_PASSTHROUGH_STATE_CHANGED_REINIT_REQUIRED_BIT_FB | XR_PASSTHROUGH_STATE_CHANGED_NON_RECOVERABLE_ERROR_BIT_FB | XR_PASSTHROUGH_STATE_CHANGED_RECOVERABLE_ERROR_BIT_FB))
            {
                bRuntimeError = true;
                bNeedsSessionRecreation |= (Event->flags & (XR_PASSTHROUGH_STATE_CHANGED_REINIT_REQUIRED_BIT_FB | XR_PASSTHROUGH_STATE_CHANGED_NON_RECOVERABLE_ERROR_BIT_FB)) != 0;
                GPassthroughRunning.Store(false);
                UE_LOG(LogMRFPSXR, Error, TEXT("Runtime passthrough state error flags: %llu; MR is unavailable until restored or session recreated"), static_cast<unsigned long long>(Event->flags));
            }
            if (Event->flags & XR_PASSTHROUGH_STATE_CHANGED_RESTORED_ERROR_BIT_FB)
            {
                bRuntimeError = false;
                if (!bNeedsSessionRecreation && bFeatureActive && bLayerActive) GPassthroughRunning.Store(true);
            }
        }
    }

    virtual const void* OnEndProjectionLayer_RHIThread(XrSession InSession, int32 InLayerIndex, const void* InNext, XrCompositionLayerFlags& OutFlags) override
    {
        if (GPassthroughRunning.Load()) OutFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        return InNext;
    }

    virtual void UpdateCompositionLayers_RHIThread(XrSession InSession, TArray<XrCompositionLayerBaseHeader*>& Headers) override
    {
        if (!GPassthroughRunning.Load()) return;
        FScopeLock Lock(&Mutex);
        if (InSession == Session && Layer && GPassthroughRunning.Load())
        {
            // Persistent member, not a stack pointer. Index zero is below all scene projections.
            Headers.Insert(reinterpret_cast<XrCompositionLayerBaseHeader*>(&Composition), 0);
        }
    }

    virtual void OnDestroySession(XrSession InSession) override
    {
        GPassthroughRunning.Store(false);
        // UE5.8 FOpenXRHMD::DestroySession flushes rendering before taking SessionHandleMutex
        // and invoking this callback. A nested flush here could deadlock that mutex.
        FScopeLock Lock(&Mutex);
        if (Session == InSession)
        {
            DestroyResources();
            Session = XR_NULL_HANDLE;
        }
    }

    void Shutdown()
    {
        GPassthroughRunning.Store(false);
        FlushRenderingCommands();
        FScopeLock Lock(&Mutex);
        DestroyResources();
        Session = XR_NULL_HANDLE;
    }

private:
    template<typename T> bool Load(const ANSICHAR* Name, T& Function)
    {
        static_assert(sizeof(T) == sizeof(PFN_xrVoidFunction), "OpenXR function pointer representation mismatch");
        Function = nullptr;
        const XrResult Result = OpenXRDynamicAPI::xrGetInstanceProcAddr(Instance, Name, reinterpret_cast<PFN_xrVoidFunction*>(&Function));
        if (XR_FAILED(Result) || !Function)
        {
            UE_LOG(LogMRFPSXR, Error, TEXT("Cannot load %hs: result %d"), Name, int32(Result));
            return false;
        }
        return true;
    }

    void StartResources()
    {
        if (!Passthrough || !Layer || GPassthroughRunning.Load() || bRuntimeError || bNeedsSessionRecreation) return;
        if (!bFeatureActive)
        {
            const XrResult Result = StartPassthrough(Passthrough);
            if (XR_FAILED(Result))
            {
                UE_LOG(LogMRFPSXR, Error, TEXT("xrPassthroughStartFB failed: %d"), int32(Result));
                return;
            }
            bFeatureActive = true;
        }
        if (!bLayerActive)
        {
            const XrResult Result = ResumeLayer(Layer);
            if (XR_FAILED(Result))
            {
                UE_LOG(LogMRFPSXR, Error, TEXT("xrPassthroughLayerResumeFB failed: %d"), int32(Result));
                return;
            }
            bLayerActive = true;
        }
        GPassthroughRunning.Store(true);
        UE_LOG(LogMRFPSXR, Display, TEXT("Native OpenXR passthrough is running"));
    }

    void DestroyResources()
    {
        GPassthroughRunning.Store(false);
        if (Layer && DestroyLayer)
        {
            DestroyLayer(Layer);
            Layer = XR_NULL_HANDLE;
        }
        if (Passthrough && DestroyPassthrough)
        {
            DestroyPassthrough(Passthrough);
            Passthrough = XR_NULL_HANDLE;
        }
        Composition = { XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
        bFeatureActive = bLayerActive = false;
    }

    FCriticalSection Mutex;
    IOpenXRHMDModule* OpenXRModule = nullptr;
    XrInstance Instance = XR_NULL_HANDLE;
    XrSession Session = XR_NULL_HANDLE;
    XrPassthroughFB Passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB Layer = XR_NULL_HANDLE;
    XrCompositionLayerPassthroughFB Composition{ XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
    bool bExtensionEnabled = false;
    bool bFunctionsLoaded = false;
    bool bSystemSupported = false;
    bool bFeatureActive = false;
    bool bLayerActive = false;
    bool bRuntimeError = false;
    bool bNeedsSessionRecreation = false;
    PFN_xrCreatePassthroughFB CreatePassthrough = nullptr;
    PFN_xrDestroyPassthroughFB DestroyPassthrough = nullptr;
    PFN_xrPassthroughStartFB StartPassthrough = nullptr;
    PFN_xrPassthroughPauseFB PausePassthrough = nullptr;
    PFN_xrCreatePassthroughLayerFB CreateLayer = nullptr;
    PFN_xrDestroyPassthroughLayerFB DestroyLayer = nullptr;
    PFN_xrPassthroughLayerResumeFB ResumeLayer = nullptr;
    PFN_xrPassthroughLayerPauseFB PauseLayer = nullptr;
    PFN_xrPassthroughLayerSetStyleFB SetStyle = nullptr;
};

FMRFPSXRModule::FMRFPSXRModule() = default;
FMRFPSXRModule::~FMRFPSXRModule() = default;

void FMRFPSXRModule::StartupModule()
{
    Extension = MakeUnique<FMRFPSPassthroughExtension>();
    Extension->RegisterOpenXRExtensionModularFeature();
    MRFPSGeometryRuntime::Startup();
}

void FMRFPSXRModule::ShutdownModule()
{
    MRFPSGeometryRuntime::Shutdown();
    if (Extension)
    {
        Extension->UnregisterOpenXRExtensionModularFeature();
        Extension->Shutdown();
        Extension.Reset();
    }
}

bool FMRFPSXRModule::IsPassthroughRunning() { return GPassthroughRunning.Load(); }

IMPLEMENT_MODULE(FMRFPSXRModule, MRFPSXR)
#else
// Desktop observation uses the same authenticated video/geometry receivers;
// Apple's desktop client does not load Quest's OpenXR runtime.
class FMRFPSPassthroughExtension {};
FMRFPSXRModule::FMRFPSXRModule()=default;
FMRFPSXRModule::~FMRFPSXRModule()=default;
void FMRFPSXRModule::StartupModule() { MRFPSGeometryRuntime::Startup(); }
void FMRFPSXRModule::ShutdownModule() { MRFPSGeometryRuntime::Shutdown(); }
bool FMRFPSXRModule::IsPassthroughRunning() { return false; }
IMPLEMENT_MODULE(FMRFPSXRModule,MRFPSXR)
#endif
