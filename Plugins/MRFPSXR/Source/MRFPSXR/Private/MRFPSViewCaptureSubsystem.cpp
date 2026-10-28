#include "MRFPSViewCaptureSubsystem.h"

#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MRFPSViewCaptureSubsystem)

#if (PLATFORM_WINDOWS || PLATFORM_MAC) && !UE_SERVER
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "RHI.h"
#include "Slate/SceneViewport.h"

namespace MRFPSDesktopViewCapture
{
constexpr int32 MaximumJpegBytes = 60 * 1024;
constexpr int32 MaximumSourceWidth = 1920;
constexpr int32 MaximumSourceHeight = 1080;
constexpr int32 MaximumOutputWidth = 640;
constexpr int32 MaximumOutputHeight = 360;
constexpr double MinimumCaptureInterval = 1.0 / 30.0;
constexpr double MaximumFrameAge = 2.0;

static bool IsRecent(double Time, double Now)
{
    const double Age = Now - Time;
    return Time > 0.0 && Age >= 0.0 && Age <= MaximumFrameAge;
}

static bool CanRender()
{
    return IsInGameThread() && !IsEngineExitRequested() && FApp::CanEverRender() &&
        !IsRunningDedicatedServer() && !GUsingNullRHI && GDynamicRHI && FViewport::IsGameRenderingEnabled();
}

static UGameViewportClient* OwnedViewportClient(const UGameInstance* Instance)
{
    if (!Instance || !CanRender()) return nullptr;
    auto* Client = Instance->GetGameViewportClient();
    // No GEngine->GameViewport, Slate screenshot, OS desktop capture, or fallback
    // to another PIE instance. Both UObject ownership and the viewport client match.
    if (!IsValid(Client) || Client->GetGameInstance() != Instance || !Client->GetWorld() ||
        !Client->GetWorld()->IsGameWorld() || !Client->Viewport || Client->Viewport->GetClient() != Client)
        return nullptr;
    return Client;
}
}
#endif

#if PLATFORM_ANDROID && !UE_SERVER && USE_ANDROID_JNI
#include "Android/AndroidJavaEnv.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSViewCapture, Log, All);

namespace MRFPSViewCapture
{
constexpr int32 MaximumJpegBytes = 60 * 1024;
constexpr double MaximumFrameAge = 2.0;

enum class EState : int32
{
    Stopped = 0, AwaitingConsent = 1, Active = 2, Denied = 3,
    Unavailable = 4, ProjectionStopped = 5, Busy = 6, DebugWaiting = 7
};

// The JNI producer owns no UObject and schedules no game-thread tasks. This is a
// single bounded mailbox, including while the game thread is paused by system UI.
struct FMailbox
{
    FCriticalSection Mutex;
    uint64 Session = 0;
    uint64 NextSession = 0;
    uint64 LastDebugSequence = 0;
    EState State = EState::Stopped;
    bool bRequested = false;
    bool bDebug = false;
    double ReceivedAt = 0.0;
    int32 Width = 0;
    int32 Height = 0;
    TArray<uint8> Jpeg;
    uint64 AcceptedFrames = 0;
    uint64 DequeuedFrames = 0;
    uint64 InvalidJpegs = 0;
    int32 LastFrameBytes = 0;
    double LastAcceptedAt = 0.0;
    double NextDiagnosticAt = 0.0;

    void ClearFrame()
    {
        Jpeg.Reset();
        Width = Height = 0;
        ReceivedAt = 0.0;
    }
};
static FMailbox Mailbox;

static bool IsFresh(const FMailbox& Box)
{
    const double Age = FPlatformTime::Seconds() - Box.ReceivedAt;
    return Box.ReceivedAt > 0.0 && Age >= 0.0 && Age <= MaximumFrameAge;
}

// Read the encoded JPEG's SOF dimensions, rather than trusting producer metadata.
// The consumer still performs normal decoder validation before displaying it.
static bool ValidateJpeg(const TArray<uint8>& Data, int32 Width, int32 Height, bool bDebug)
{
    const int32 LimitWidth = bDebug ? 1280 : 640;
    const int32 LimitHeight = bDebug ? 720 : 360;
    const int32 Size = Data.Num();
    if (Width <= 0 || Height <= 0 || Width > LimitWidth || Height > LimitHeight ||
        Size < 12 || Size > MaximumJpegBytes || Data[0] != 0xff || Data[1] != 0xd8 ||
        Data[Size - 2] != 0xff || Data[Size - 1] != 0xd9)
    {
        return false;
    }
    int32 Position = 2;
    while (Position + 1 < Size - 2)
    {
        if (Data[Position++] != 0xff) return false;
        while (Position < Size && Data[Position] == 0xff) ++Position;
        if (Position >= Size) return false;
        const uint8 Marker = Data[Position++];
        if (Marker == 0xda || Marker == 0xd9 || Marker == 0x00) return false;
        if (Marker == 0x01 || (Marker >= 0xd0 && Marker <= 0xd7)) continue;
        if (Position + 2 > Size) return false;
        const int32 Length = (int32(Data[Position]) << 8) | Data[Position + 1];
        if (Length < 2 || Length > Size - Position) return false;
        const bool bSof = Marker >= 0xc0 && Marker <= 0xcf &&
            Marker != 0xc4 && Marker != 0xc8 && Marker != 0xcc;
        if (bSof)
        {
            return Length >= 8 &&
                ((int32(Data[Position + 3]) << 8) | Data[Position + 4]) == Height &&
                ((int32(Data[Position + 5]) << 8) | Data[Position + 6]) == Width;
        }
        Position += Length;
    }
    return false;
}

static bool CallJava(const char* MethodName, uint64 Session, bool bWithSession = true)
{
    JNIEnv* Env = AndroidJavaEnv::GetJavaEnv();
    jobject Activity = AndroidJavaEnv::GetGameActivityThis();
    if (!Env || !Activity) return false;
    jclass Class = AndroidJavaEnv::FindJavaClass("com/mrfps/capture/MRFPSViewCapture");
    if (!Class || Env->ExceptionCheck())
    {
        Env->ExceptionClear();
        if (Class) Env->DeleteLocalRef(Class);
        return false;
    }
    jmethodID Method = Env->GetStaticMethodID(Class, MethodName,
        bWithSession ? "(Landroid/app/Activity;J)V" : "(Landroid/app/Activity;)V");
    if (Method && !Env->ExceptionCheck())
    {
        if (bWithSession) Env->CallStaticVoidMethod(Class, Method, Activity, static_cast<jlong>(Session));
        else Env->CallStaticVoidMethod(Class, Method, Activity);
    }
    const bool bSuccess = Method && !Env->ExceptionCheck();
    if (Env->ExceptionCheck()) Env->ExceptionClear();
    Env->DeleteLocalRef(Class);
    return bSuccess;
}
}

extern "C" JNIEXPORT void JNICALL
Java_com_mrfps_capture_MRFPSViewCapture_nativeOnState(JNIEnv*, jclass, jlong Session, jint State)
{
    using namespace MRFPSViewCapture;
    if (State < int32(EState::Stopped) || State > int32(EState::DebugWaiting)) return;
    FScopeLock Lock(&Mailbox.Mutex);
    if (Session <= 0 || uint64(Session) != Mailbox.Session) return;
    Mailbox.State = static_cast<EState>(State);
    UE_LOG(LogMRFPSViewCapture, Display, TEXT("MRFPS_CAPTURE_NATIVE_STATE session=%llu state=%d"),
        static_cast<unsigned long long>(Mailbox.Session), State);
    if (Mailbox.State != EState::AwaitingConsent && Mailbox.State != EState::Active &&
        Mailbox.State != EState::DebugWaiting)
    {
        Mailbox.bRequested = false;
        Mailbox.ClearFrame();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mrfps_capture_MRFPSViewCapture_nativeOnFrame(JNIEnv* Env, jclass, jlong Session,
    jbyteArray Bytes, jint Width, jint Height, jlong SourceUtcNanoseconds)
{
    using namespace MRFPSViewCapture;
    if (!Bytes || Session <= 0) return;
    const jsize Length = Env->GetArrayLength(Bytes);
    if (Length < 12 || Length > MaximumJpegBytes) return;
    bool bDebug = false;
    {
        FScopeLock Lock(&Mailbox.Mutex);
        if (uint64(Session) != Mailbox.Session || !Mailbox.bRequested) return;
        bDebug = Mailbox.bDebug;
    }
    if (bDebug)
    {
        // Debug files can survive process restarts. Sequence is producer UTC Unix
        // nanoseconds, not a counter; tolerate the measured headset clock skew.
        if (SourceUtcNanoseconds <= 0) return;
        const double SourceSeconds = double(SourceUtcNanoseconds) / 1000000000.0;
        const double NowSeconds = double(FDateTime::UtcNow().GetTicks() -
            FDateTime(1970, 1, 1).GetTicks()) / double(ETimespan::TicksPerSecond);
        const double Age = NowSeconds - SourceSeconds;
        if (Age > MaximumFrameAge || Age < -5.0) return;
    }
    else if (SourceUtcNanoseconds != 0)
    {
        return;
    }
    TArray<uint8> Frame;
    Frame.SetNumUninitialized(Length);
    Env->GetByteArrayRegion(Bytes, 0, Length, reinterpret_cast<jbyte*>(Frame.GetData()));
    if (Env->ExceptionCheck()) { Env->ExceptionClear(); return; }
    if (!ValidateJpeg(Frame, Width, Height, bDebug)) {
        FScopeLock Lock(&Mailbox.Mutex);
        if (uint64(Session) == Mailbox.Session) ++Mailbox.InvalidJpegs;
        return;
    }
    FScopeLock Lock(&Mailbox.Mutex);
    if (uint64(Session) != Mailbox.Session || !Mailbox.bRequested) return;
    if (bDebug && uint64(SourceUtcNanoseconds) <= Mailbox.LastDebugSequence) return;
    Mailbox.LastDebugSequence = bDebug ? uint64(SourceUtcNanoseconds) : 0;
    Mailbox.Jpeg = MoveTemp(Frame);
    Mailbox.Width = Width;
    Mailbox.Height = Height;
    Mailbox.ReceivedAt = FPlatformTime::Seconds();
    Mailbox.LastAcceptedAt = Mailbox.ReceivedAt;
    Mailbox.LastFrameBytes = Length;
    ++Mailbox.AcceptedFrames;
    Mailbox.State = bDebug ? EState::DebugWaiting : EState::Active;
}
#endif

void UMRFPSViewCaptureSubsystem::Deinitialize()
{
    StopCapture();
    Super::Deinitialize();
}

void UMRFPSViewCaptureSubsystem::RequestCapture()
{
    if (!IsInGameThread()) return;
#if PLATFORM_ANDROID && !UE_SERVER && USE_ANDROID_JNI
    using namespace MRFPSViewCapture;
    {
        FScopeLock Lock(&Mailbox.Mutex);
        if (CaptureSession != 0 && Mailbox.Session == CaptureSession && Mailbox.bRequested) return;
        CaptureSession = ++Mailbox.NextSession;
        Mailbox.Session = CaptureSession;
        bDebugSource = FParse::Param(FCommandLine::Get(), TEXT("MRFPSVideoFromAdb"));
        Mailbox.bDebug = bDebugSource;
        Mailbox.bRequested = true;
        Mailbox.LastDebugSequence = 0;
        Mailbox.AcceptedFrames = Mailbox.DequeuedFrames = Mailbox.InvalidJpegs = 0;
        Mailbox.LastFrameBytes = 0;
        Mailbox.LastAcceptedAt = Mailbox.NextDiagnosticAt = 0.0;
        Mailbox.ClearFrame();
        Mailbox.State = bDebugSource ? EState::DebugWaiting : EState::AwaitingConsent;
        NextDebugPollAt = 0.0;
    }
    // Debug source deliberately opens no projection/consent. It is opt-in only.
    if (!bDebugSource && !CallJava("request", CaptureSession))
    {
        FScopeLock Lock(&Mailbox.Mutex);
        if (Mailbox.Session == CaptureSession)
        {
            Mailbox.State = EState::Unavailable;
            Mailbox.bRequested = false;
        }
    }
#elif (PLATFORM_WINDOWS || PLATFORM_MAC) && !UE_SERVER
    using namespace MRFPSDesktopViewCapture;
    if (CaptureSession != 0) return;
    ClearDesktopFrame();
    bDesktopUnavailable = true;
    // The caller is the local player's explicit share control or the opt-in
    // auto-request flag. Never select desktop capture as an ADB/headset fallback.
    if (FParse::Param(FCommandLine::Get(), TEXT("MRFPSVideoFromAdb"))) return;
    auto* Client = OwnedViewportClient(GetGameInstance());
    if (!Client || NextDesktopSession == MAX_uint64) return;
    CaptureSession = ++NextDesktopSession;
    DesktopViewportOwner = Client;
    NextDesktopCaptureAt = 0.0;
    DesktopDrawHandle = UGameViewportClient::OnViewportRendered().AddUObject(this, &UMRFPSViewCaptureSubsystem::OnDesktopViewportRendered);
    bDesktopUnavailable = false;
#endif
}

void UMRFPSViewCaptureSubsystem::StopCapture()
{
    if (!IsInGameThread()) return;
#if PLATFORM_ANDROID && !UE_SERVER && USE_ANDROID_JNI
    using namespace MRFPSViewCapture;
    bool bOwned = false;
    {
        FScopeLock Lock(&Mailbox.Mutex);
        bOwned = CaptureSession != 0 && CaptureSession == Mailbox.Session;
        if (bOwned)
        {
            Mailbox.Session = 0; // Late JNI callbacks cannot resurrect this session.
            Mailbox.bRequested = false;
            Mailbox.State = EState::Stopped;
            Mailbox.ClearFrame();
        }
    }
    if (bOwned && !bDebugSource) CallJava("stop", CaptureSession, false);
#elif (PLATFORM_WINDOWS || PLATFORM_MAC) && !UE_SERVER
    UGameViewportClient::OnViewportRendered().Remove(DesktopDrawHandle);
    DesktopDrawHandle.Reset();
    DesktopViewportOwner.Reset();
    NextDesktopCaptureAt = 0.0;
    bDesktopUnavailable = false;
    ClearDesktopFrame();
#endif
    CaptureSession = 0;
    bDebugSource = false;
}

bool UMRFPSViewCaptureSubsystem::IsCapturing() const
{
#if PLATFORM_ANDROID && !UE_SERVER && USE_ANDROID_JNI
    using namespace MRFPSViewCapture;
    FScopeLock Lock(&Mailbox.Mutex);
    return CaptureSession != 0 && CaptureSession == Mailbox.Session && Mailbox.bRequested && IsFresh(Mailbox);
#elif (PLATFORM_WINDOWS || PLATFORM_MAC) && !UE_SERVER
    return IsDesktopFrameFresh();
#else
    return false;
#endif
}

bool UMRFPSViewCaptureSubsystem::IsCaptureRequestPending() const
{
#if PLATFORM_ANDROID && !UE_SERVER && USE_ANDROID_JNI
    JNIEnv* Env = AndroidJavaEnv::GetJavaEnv();
    if (Env) {
        jclass Class = AndroidJavaEnv::FindJavaClass("com/mrfps/capture/MRFPSViewCapture");
        if (Class && !Env->ExceptionCheck()) {
            jmethodID Method = Env->GetStaticMethodID(Class, "hasPendingConsent", "()Z");
            if (Method && !Env->ExceptionCheck()) {
                const bool Pending = Env->CallStaticBooleanMethod(Class, Method) == JNI_TRUE;
                if (!Env->ExceptionCheck()) { Env->DeleteLocalRef(Class); return Pending; }
            }
        }
        if (Env->ExceptionCheck()) Env->ExceptionClear();
        if (Class) Env->DeleteLocalRef(Class);
    }
    using namespace MRFPSViewCapture;
    FScopeLock Lock(&Mailbox.Mutex);
    return CaptureSession != 0 && CaptureSession == Mailbox.Session &&
        (Mailbox.State == EState::AwaitingConsent || Mailbox.State == EState::Busy);
#else
    return false;
#endif
}

FString UMRFPSViewCaptureSubsystem::GetSafeStatus() const
{
#if PLATFORM_ANDROID && !UE_SERVER && USE_ANDROID_JNI
    using namespace MRFPSViewCapture;
    FScopeLock Lock(&Mailbox.Mutex);
    if (CaptureSession == 0 || CaptureSession != Mailbox.Session) return TEXT("Headset view capture stopped.");
    if (Mailbox.bRequested && IsFresh(Mailbox))
        return Mailbox.bDebug ? TEXT("ADB debug headset view is fresh.") : TEXT("Headset view is being shared.");
    switch (Mailbox.State)
    {
    case EState::AwaitingConsent: return TEXT("Waiting for headset screen-sharing consent.");
    case EState::Active: return TEXT("Sharing interrupted / check headset and close other casting.");
    case EState::Denied: return TEXT("Screen-sharing consent was declined.");
    case EState::Unavailable: return TEXT("Headset screen capture is unavailable.");
    case EState::ProjectionStopped: return TEXT("Headset sharing ended / close other casting, then press X to share again.");
    case EState::Busy: return TEXT("A previous screen-sharing consent dialog is still open.");
    case EState::DebugWaiting: return TEXT("ADB debug mode; waiting for a fresh internal frame.");
    default: return TEXT("Headset view capture stopped.");
    }
#elif (PLATFORM_WINDOWS || PLATFORM_MAC) && !UE_SERVER
    if (IsDesktopFrameFresh()) return TEXT("Player view is being shared.");
    if (bDesktopUnavailable) return TEXT("Player view capture is unavailable.");
    return CaptureSession != 0 ? TEXT("Waiting for a fresh player view.") : TEXT("Player view sharing stopped.");
#else
    return TEXT("Player view capture is unavailable on this platform.");
#endif
}

bool UMRFPSViewCaptureSubsystem::DequeueLatestFrame(TArray<uint8>& Jpeg, int32& W, int32& H)
{
    Jpeg.Reset();
    W = H = 0;
    if (!IsInGameThread()) return false;
#if PLATFORM_ANDROID && !UE_SERVER && USE_ANDROID_JNI
    using namespace MRFPSViewCapture;
    const double Now = FPlatformTime::Seconds();
    if (bDebugSource && CaptureSession != 0 && Now >= NextDebugPollAt)
    {
        NextDebugPollAt = Now + 0.2; // Debug file I/O is bounded to 5 Hz as well.
        CallJava("pollDebug", CaptureSession);
    }
    FScopeLock Lock(&Mailbox.Mutex);
    if (Now >= Mailbox.NextDiagnosticAt && CaptureSession != 0) {
        Mailbox.NextDiagnosticAt = Now + 2.0;
        UE_LOG(LogMRFPSViewCapture, Display,
            TEXT("MRFPS_CAPTURE_NATIVE session=%llu requested=%d state=%d accepted=%llu dequeued=%llu invalid=%llu bytes=%d age=%.3f"),
            static_cast<unsigned long long>(Mailbox.Session), int32(Mailbox.bRequested), int32(Mailbox.State),
            static_cast<unsigned long long>(Mailbox.AcceptedFrames), static_cast<unsigned long long>(Mailbox.DequeuedFrames),
            static_cast<unsigned long long>(Mailbox.InvalidJpegs), Mailbox.LastFrameBytes,
            Mailbox.LastAcceptedAt > 0.0 ? Now - Mailbox.LastAcceptedAt : -1.0);
    }
    if (CaptureSession == 0 || CaptureSession != Mailbox.Session || !Mailbox.bRequested) return false;
    if (!IsFresh(Mailbox)) { Mailbox.ClearFrame(); return false; }
    if (Mailbox.Jpeg.IsEmpty()) return false;
    Jpeg = MoveTemp(Mailbox.Jpeg);
    ++Mailbox.DequeuedFrames;
    W = Mailbox.Width;
    H = Mailbox.Height;
    return true;
#elif (PLATFORM_WINDOWS || PLATFORM_MAC) && !UE_SERVER
    if (!IsDesktopFrameFresh()) { ClearDesktopFrame(); return false; }
    if (DesktopJpeg.IsEmpty()) return false;
    Jpeg = MoveTemp(DesktopJpeg);
    W = DesktopFrameWidth;
    H = DesktopFrameHeight;
    return true;
#else
    return false;
#endif
}

#if (PLATFORM_WINDOWS || PLATFORM_MAC) && !UE_SERVER
void UMRFPSViewCaptureSubsystem::ClearDesktopFrame()
{
    DesktopJpeg.Reset();
    DesktopFrameWidth = DesktopFrameHeight = 0;
    DesktopFrameAt = 0.0;
}

bool UMRFPSViewCaptureSubsystem::IsDesktopFrameFresh() const
{
    using namespace MRFPSDesktopViewCapture;
    auto* Client = OwnedViewportClient(GetGameInstance());
    return CaptureSession != 0 && Client && Client == DesktopViewportOwner.Get() &&
        IsRecent(DesktopFrameAt, FPlatformTime::Seconds());
}

void UMRFPSViewCaptureSubsystem::OnDesktopViewportRendered(FViewport* DrawnViewport)
{
    using namespace MRFPSDesktopViewCapture;
    if (!CaptureSession) return;
    auto* Client = OwnedViewportClient(GetGameInstance());
    if (!Client || Client != DesktopViewportOwner.Get()) { ClearDesktopFrame(); return; }
    // UE5.8 calls this public delegate on the game thread after its scene canvas
    // (including the game HUD) has flushed, before screenshots and EndRenderFrame.
    // Reading later from Tick can access a released direct-render backbuffer.
    // The delegate is global, but capture accepts this exact owned viewport only.
    if (DrawnViewport != Client->Viewport) return;
    const double Now = FPlatformTime::Seconds();
    if (Now < NextDesktopCaptureAt) return;
    NextDesktopCaptureAt = Now + MinimumCaptureInterval;
    ClearDesktopFrame();

    FSceneViewport* Viewport = Client->Viewport;
    const FIntPoint Size = Viewport->GetSizeXY();
    if (Size.X <= 0 || Size.Y <= 0 || Size.X > MaximumSourceWidth || Size.Y > MaximumSourceHeight) return;
    // A direct-render viewport has no GT render target; its RT target is valid
    // only between the queued Begin/EndRenderFrame surrounding this callback.
    if (static_cast<const ISlateViewport*>(Viewport)->UseSeparateRenderTarget() &&
        !Viewport->GetRenderTargetTexture().IsValid()) return;
    const uint64 Session = CaptureSession;
    const double DrawTime = Now;
    TArray<FColor> Pixels; // At most 1920*1080*4 bytes; no persistent raw-frame queue.
    FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
    ReadFlags.SetLinearToGamma(true);
    if (!Viewport->ReadPixels(Pixels, ReadFlags, FIntRect(0, 0, Size.X, Size.Y)) ||
        Pixels.Num() != Size.X * Size.Y || Viewport->GetSizeXY() != Size)
        return;

    // Preserve the whole game view's aspect ratio within 640x360, without upsampling.
    const double Scale = FMath::Min(1.0, FMath::Min(double(MaximumOutputWidth) / Size.X, double(MaximumOutputHeight) / Size.Y));
    const int32 Width = FMath::Max(1, FMath::FloorToInt(Size.X * Scale));
    const int32 Height = FMath::Max(1, FMath::FloorToInt(Size.Y * Scale));
    FImage Resized;
    FImageCore::ResizeImageAllocDest(FImageView(Pixels.GetData(), Size.X, Size.Y), Resized,
        Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB, FImageCore::EResizeImageFilter::Triangle);
    FImageCore::SetAlphaOpaque(Resized);
    TArray64<uint8> Encoded;
    for (const int32 Quality : {65, 50, 35, 20})
    {
        Encoded.Reset();
        if (!FImageUtils::CompressImage(Encoded, TEXT("jpg"), Resized, Quality)) return;
        if (Encoded.Num() >= 12 && Encoded.Num() <= MaximumJpegBytes) break;
    }
    // The deadline starts at the viewport draw; encoding time must not add a
    // second frame interval. The next render still samples only the latest view.
    if (Encoded.Num() < 12 || Encoded.Num() > MaximumJpegBytes ||
        Encoded[0] != 0xff || Encoded[1] != 0xd8 || Encoded[Encoded.Num()-2] != 0xff || Encoded.Last() != 0xd9 ||
        CaptureSession != Session || OwnedViewportClient(GetGameInstance()) != Client || Client->Viewport != Viewport ||
        !IsRecent(DrawTime, FPlatformTime::Seconds())) return;
    DesktopJpeg.Append(Encoded.GetData(), static_cast<int32>(Encoded.Num()));
    DesktopFrameWidth = Width;
    DesktopFrameHeight = Height;
    // A read/encode finishing late cannot make the old viewport draw fresh again.
    DesktopFrameAt = DrawTime;
}
#endif
