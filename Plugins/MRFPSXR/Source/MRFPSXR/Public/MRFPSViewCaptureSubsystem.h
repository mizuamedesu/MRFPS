#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "MRFPSViewCaptureSubsystem.generated.h"

class UGameViewportClient;
class FViewport;

/**
 * User-consented Android headset composite or explicitly requested Windows game-view capture.
 * No audio is captured. Windows reads only this game instance's viewport, never the desktop.
 * Owns at most one pending JPEG; consuming a frame never repeats the previous frame.
 * Request/stop/dequeue are game-thread operations. Android worker callbacks never touch UObjects.
 */
UCLASS()
class MRFPSXR_API UMRFPSViewCaptureSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;

    /** Opens Android's OS consent UI, or starts an explicitly requested Windows game-view session. */
    UFUNCTION(BlueprintCallable, Category="MRFPS|Headset View")
    void RequestCapture();

    /** Invalidates pending results and releases the selected source. */
    UFUNCTION(BlueprintCallable, Category="MRFPS|Headset View")
    void StopCapture();

    /** True only while the selected source has supplied a frame within the last two seconds. */
    UFUNCTION(BlueprintPure, Category="MRFPS|Headset View")
    bool IsCapturing() const;

    /** Game-thread local session/intent state; this does not assert live frames or OS cleanup. */
    bool HasCaptureSession() const { return CaptureSession != 0; }

    /** True while Android still has an unresolved OS consent request. Never grants consent. */
    bool IsCaptureRequestPending() const;

    /** True for a failed or blocked capture, without treating an unstarted request as failed. */
    bool HasCaptureError() const;

    /** Fixed diagnostic text; never exposes tokens, paths, OS intents, or exception contents. */
    UFUNCTION(BlueprintPure, Category="MRFPS|Headset View")
    FString GetSafeStatus() const;

    /** Moves out the newest fresh JPEG (at most 60 KiB). Returns false with empty outputs otherwise. */
    bool DequeueLatestFrame(TArray<uint8>& Jpeg, int32& W, int32& H);

private:
    uint64 CaptureSession = 0;
    double NextDebugPollAt = 0.0;
    bool bDebugSource = false;

#if PLATFORM_ANDROID && !UE_SERVER && USE_ANDROID_JNI
    bool HasAndroidCaptureFailure() const;
    mutable uint64 ObservedFailedSession = 0;
    mutable double NextFailureCheckAt = 0.0;
    bool bAndroidUnavailable = false;
#endif

#if (PLATFORM_WINDOWS || PLATFORM_MAC) && !UE_SERVER
    void OnDesktopViewportRendered(FViewport* DrawnViewport);
    void ClearDesktopFrame();
    bool IsDesktopFrameFresh() const;
    TWeakObjectPtr<UGameViewportClient> DesktopViewportOwner;
    FDelegateHandle DesktopDrawHandle;
    uint64 NextDesktopSession = 0;
    double DesktopFrameAt = 0.0;
    double NextDesktopCaptureAt = 0.0;
    int32 DesktopFrameWidth = 0;
    int32 DesktopFrameHeight = 0;
    bool bDesktopUnavailable = false;
    TArray<uint8> DesktopJpeg;
#endif
};
