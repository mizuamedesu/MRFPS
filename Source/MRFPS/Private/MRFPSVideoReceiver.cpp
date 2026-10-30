#include "MRFPSVideoReceiver.h"

#if UE_SERVER

#include "Engine/GameInstance.h"

// A dedicated server only relays the bounded, authorized JPEG packets. Keep
// codecs, worker buffers, renderer headers and video tests out of its TU.
struct FMRFPSVideoReceiverState {};

UMRFPSVideoReceiver::UMRFPSVideoReceiver() : FTickableGameObject(ETickableTickType::Never) {}
UMRFPSVideoReceiver::~UMRFPSVideoReceiver() = default;
void UMRFPSVideoReceiver::Initialize(FSubsystemCollectionBase& Collection) { Super::Initialize(Collection); }
void UMRFPSVideoReceiver::Deinitialize() { StopReceiver(); Super::Deinitialize(); }
void UMRFPSVideoReceiver::BeginDestroy() { StopReceiver(); Super::BeginDestroy(); }
bool UMRFPSVideoReceiver::SubmitFrame(int32, uint32, uint32, int32, int32, TArray<uint8>) { return false; }
UTexture2D* UMRFPSVideoReceiver::GetFreshTexture(int32) const { return nullptr; }
UTexture2D* UMRFPSVideoReceiver::GetFreshTextureForStream(int32, uint32) const { return nullptr; }
FString UMRFPSVideoReceiver::GetSafeStatus(int32) const { return TEXT("Video unavailable (dedicated server)"); }
FString UMRFPSVideoReceiver::GetSafeStatusForStream(int32, uint32) const { return TEXT("Video unavailable (dedicated server)"); }
void UMRFPSVideoReceiver::StopStream(int32, uint32) {}
void UMRFPSVideoReceiver::ResetSlot(int32) {}
void UMRFPSVideoReceiver::ResetAll() {}
void UMRFPSVideoReceiver::StopReceiver() {}
void UMRFPSVideoReceiver::Tick(float) {}
bool UMRFPSVideoReceiver::IsTickable() const { return false; }
TStatId UMRFPSVideoReceiver::GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(UMRFPSVideoReceiver, STATGROUP_Tickables); }
UWorld* UMRFPSVideoReceiver::GetTickableGameObjectWorld() const { return GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr; }

#else // !UE_SERVER: the client implementation below is unchanged.

#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "MRFPSVideoWorkBudget.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Async/Async.h"

#if !UE_SERVER
#include "ImageCore.h"
#include "ImageUtils.h"
#include "IImageWrapperModule.h"
#include "RHI.h"
#include "TextureResource.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSVideoReceiver, Log, All);

namespace MRFPSVideo
{
constexpr int32 SlotCount = 2;
constexpr int32 CanvasWidth = 640;
constexpr int32 CanvasHeight = 360;
constexpr int32 CanvasBytes = CanvasWidth * CanvasHeight * 4;
constexpr int32 MaxJpegBytes = 60 * 1024;
constexpr int32 MaxWidth = 1280;
constexpr int32 MaxHeight = 720;
constexpr double FreshSeconds = 1.0;
// Scheduling headroom for 30 fps sources. Decode and GPU progress determine
// actual throughput; this interval is not an achieved-FPS claim.
constexpr double DecodeInterval = 1.0 / 60.0;

// At most two permits cover decode, completion and render upload in the whole
// process, with at most one per receiver slot. A stalled game or render thread
// therefore cannot grow an unbounded queue or reorder uploads for a slot.
static_assert(SlotCount == MRFPSVideoWorkBudget::SlotCount, "video budget slot mismatch");
using FProcessBudget = MRFPSVideoWorkBudget::FProcessBudget;
using FReceiverBudget = MRFPSVideoWorkBudget::FReceiverBudget;
TSharedRef<FProcessBudget, ESPMode::ThreadSafe> ProcessBudget()
{
    static const auto Budget = MakeShared<FProcessBudget, ESPMode::ThreadSafe>();
    return Budget;
}
struct FWorkPermit
{
    FWorkPermit(const TSharedRef<FProcessBudget, ESPMode::ThreadSafe>& InProcess,
        const TSharedRef<FReceiverBudget, ESPMode::ThreadSafe>& InReceiver, int32 InSlot)
        : Process(InProcess), Receiver(InReceiver), Slot(InSlot) {}
    ~FWorkPermit() { MRFPSVideoWorkBudget::Release(*Process, *Receiver, Slot); }
    TSharedRef<FProcessBudget, ESPMode::ThreadSafe> Process;
    TSharedRef<FReceiverBudget, ESPMode::ThreadSafe> Receiver;
    int32 Slot;
};
using FPermit = TSharedPtr<FWorkPermit, ESPMode::ThreadSafe>;
FPermit AcquirePermit(const TSharedRef<FProcessBudget, ESPMode::ThreadSafe>& Process,
    const TSharedRef<FReceiverBudget, ESPMode::ThreadSafe>& Receiver, int32 Slot, double Now)
{
    if (!MRFPSVideoWorkBudget::AcquireFair(*Process, *Receiver, Slot, Now)) return nullptr;
    return MakeShared<FWorkPermit, ESPMode::ThreadSafe>(Process, Receiver, Slot);
}

bool ValidSlot(int32 Slot) { return Slot >= 0 && Slot < SlotCount; }
bool Fresh(double ArrivedAt, double Now)
{
    return FMath::IsFinite(ArrivedAt) && FMath::IsFinite(Now) && Now >= ArrivedAt && Now - ArrivedAt < FreshSeconds;
}

// Header-only validation before an image codec can allocate pixels. Only the
// complete 8-bit baseline/progressive grayscale/RGB JPEG profile is accepted.
// Marker lengths include their own two length bytes. Entropy validation is
// subsequently performed by the real ImageWrapper JPEG decoder.
bool ValidJpegHeader(TConstArrayView<uint8> Bytes, int32 DeclaredWidth, int32 DeclaredHeight)
{
    const int32 Size = Bytes.Num();
    if (Size < 4 || Size > MaxJpegBytes || DeclaredWidth <= 0 || DeclaredHeight <= 0 ||
        DeclaredWidth > MaxWidth || DeclaredHeight > MaxHeight ||
        Bytes[0] != 0xff || Bytes[1] != 0xd8 || Bytes[Size - 2] != 0xff || Bytes[Size - 1] != 0xd9) return false;
    bool bFoundFrame = false;
    uint8 Components = 0;
    int32 Cursor = 2;
    while (Cursor < Size - 2)
    {
        if (Bytes[Cursor++] != 0xff) return false;
        while (Cursor < Size && Bytes[Cursor] == 0xff) ++Cursor;
        if (Cursor >= Size) return false;
        const uint8 Marker = Bytes[Cursor++];
        if (Marker == 0 || Marker == 0xd8 || Marker == 0xd9 || Marker == 1 || (Marker >= 0xd0 && Marker <= 0xd7)) return false;
        if (Cursor > Size - 2) return false;
        const int32 Length = (static_cast<int32>(Bytes[Cursor]) << 8) | Bytes[Cursor + 1];
        if (Length < 2 || Length > Size - Cursor) return false;
        const bool bFrameMarker = Marker >= 0xc0 && Marker <= 0xcf && Marker != 0xc4 && Marker != 0xc8 && Marker != 0xcc;
        if (bFrameMarker)
        {
            if (bFoundFrame || (Marker != 0xc0 && Marker != 0xc2) || Length < 8) return false;
            const int32 Height = (static_cast<int32>(Bytes[Cursor + 3]) << 8) | Bytes[Cursor + 4];
            const int32 Width = (static_cast<int32>(Bytes[Cursor + 5]) << 8) | Bytes[Cursor + 6];
            Components = Bytes[Cursor + 7];
            if (Bytes[Cursor + 2] != 8 || (Components != 1 && Components != 3) || Length != 8 + 3 * Components ||
                Width != DeclaredWidth || Height != DeclaredHeight) return false;
            bFoundFrame = true;
        }
        if (Marker == 0xda)
        {
            if (!bFoundFrame || Length < 6) return false;
            const uint8 ScanComponents = Bytes[Cursor + 2];
            return ScanComponents > 0 && ScanComponents <= Components && Length == 6 + 2 * ScanComponents &&
                   Cursor + Length < Size - 2;
        }
        Cursor += Length;
    }
    return false;
}

struct FFrameMeta
{
    int32 Slot = 0;
    uint32 Stream = 0;
    uint32 Sequence = 0;
    uint64 Epoch = 0;
    int32 Width = 0;
    int32 Height = 0;
    double ArrivedAt = 0;
};
struct FEncodedFrame { FFrameMeta Meta; TArray<uint8> Jpeg; };
enum class EIssue : uint8 { None, Header, Order, Decode, Resource };
struct FSlot
{
    uint32 Stream = 0;
    uint32 LastSequence = 0;
    uint64 Epoch = 1;
    bool bClosed = false;
    double LastDispatch = -1.e30;
    EIssue Issue = EIssue::None;
    TOptional<FEncodedFrame> Pending;
    TOptional<FFrameMeta> Displayed;
    uint32 ReceivedFrames = 0;
    uint32 DecodedFrames = 0;
    uint32 UploadedFrames = 0;
    uint32 RejectedFrames = 0;
    uint32 DecodeFailures = 0;
    uint32 ResourceDrops = 0;
    double LastDiagnosticAt = -1.e30;
    uint32 DiagnosticDecoded = 0;
    uint32 DiagnosticUploaded = 0;
};
int32 FindReadySlot(FSlot (&Slots)[SlotCount], int32 NextSlot, double Now, unsigned BusyMask = 0)
{
    for (int32 Offset = 0; Offset < SlotCount; ++Offset)
    {
        const int32 Slot = (NextSlot + Offset) % SlotCount;
        FSlot& Target = Slots[Slot];
        if (!Target.Pending.IsSet()) continue;
        if (!Fresh(Target.Pending->Meta.ArrivedAt, Now)) { Target.Pending.Reset(); continue; }
        if (BusyMask & (1u << Slot)) continue;
        if (Now - Target.LastDispatch >= DecodeInterval) return Slot;
    }
    return INDEX_NONE;
}
bool CanAccept(const FSlot& Slot, uint32 Stream, uint32 Sequence)
{
    if (!Stream || !Sequence || Stream < Slot.Stream) return false;
    return Stream > Slot.Stream || (!Slot.bClosed && Sequence > Slot.LastSequence);
}
void ClearSlot(FSlot& Slot, bool bForgetGeneration)
{
    ++Slot.Epoch;
    Slot.Pending.Reset(); Slot.Displayed.Reset(); Slot.LastDispatch = -1.e30; Slot.Issue = EIssue::None;
    Slot.bClosed = Slot.Stream != 0;
    if (bForgetGeneration) { Slot.Stream = 0; Slot.LastSequence = 0; Slot.bClosed = false; }
}
bool StopSlot(FSlot& Slot, uint32 StreamId)
{
    if (!StreamId || StreamId < Slot.Stream) return false;
    ClearSlot(Slot, false);
    Slot.Stream = StreamId; Slot.LastSequence = 0; Slot.bClosed = true;
    Slot.LastDiagnosticAt = -1.e30;
    return true;
}
void Accept(FSlot& Slot, FEncodedFrame&& Frame)
{
    if (Frame.Meta.Stream != Slot.Stream)
    {
        ++Slot.Epoch;
        Slot.Pending.Reset(); Slot.Displayed.Reset(); Slot.LastDispatch = -1.e30;
        Slot.Stream = Frame.Meta.Stream; Slot.bClosed = false;
    }
    Slot.LastSequence = Frame.Meta.Sequence;
    Frame.Meta.Epoch = Slot.Epoch;
    Slot.Pending.Emplace(MoveTemp(Frame));
    Slot.Issue = EIssue::None;
}
bool Matches(const FSlot& Slot, const FFrameMeta& Meta, double Now)
{
    return !Slot.bClosed && Meta.Stream == Slot.Stream && Meta.Epoch == Slot.Epoch && Meta.Sequence > 0 &&
        Meta.Sequence <= Slot.LastSequence && Fresh(Meta.ArrivedAt, Now);
}
bool CanUpload(const FSlot& Slot, const FFrameMeta& Meta, double Now)
{
    // New pending input must not starve a slower decoder. A fresh completion
    // may advance the display while the latest pending frame stays queued.
    return Matches(Slot, Meta, Now) && (!Slot.Displayed.IsSet() || Meta.Sequence > Slot.Displayed->Sequence);
}
bool DisplayedForStream(const FSlot& Slot, uint32 ExpectedStreamId)
{
    return ExpectedStreamId != 0 && Slot.Displayed.IsSet() && Slot.Displayed->Stream == ExpectedStreamId;
}
const TCHAR* DiagnosticCode(const FSlot& Slot, double Now)
{
    if (Slot.bClosed) return TEXT("STOPPED");
    if (Slot.Displayed.IsSet() && Matches(Slot, Slot.Displayed.GetValue(), Now)) return TEXT("LIVE");
    if (Slot.Issue == EIssue::Header) return TEXT("JPEG_REJECTED");
    if (Slot.Issue == EIssue::Decode) return TEXT("DECODE_FAILED");
    if (Slot.Issue == EIssue::Resource) return TEXT("WAIT_RHI");
    if (Slot.Issue == EIssue::Order) return TEXT("ORDER_REJECTED");
    return Slot.Displayed.IsSet() ? TEXT("STALE") : TEXT("WAIT_FRAME");
}

struct FDecoded
{
    FFrameMeta Meta;
    TArray<uint8> Pixels;
    FPermit Permit;
    bool bSucceeded = false;
};
struct FUploadAck { FFrameMeta Meta; FPermit Permit; };
struct FMailbox
{
    FCriticalSection Mutex;
    bool bAlive = true;
    TOptional<FDecoded> Decoded[SlotCount];
    TOptional<FUploadAck> Uploaded[SlotCount];
};

#if !UE_SERVER
bool CanRender() { return FApp::CanEverRender() && !GUsingNullRHI && GDynamicRHI != nullptr && !IsRunningDedicatedServer(); }

bool ComposeCanvas(const FImage& Image, TArray<uint8>& Pixels)
{
    if (Image.SizeX <= 0 || Image.SizeY <= 0 || Image.SizeX > MaxWidth || Image.SizeY > MaxHeight || Image.NumSlices != 1 ||
        (Image.Format != ERawImageFormat::BGRA8 && Image.Format != ERawImageFormat::G8)) return false;
    const bool bGray = Image.Format == ERawImageFormat::G8;
    if (Image.RawData.Num() != static_cast<int64>(Image.SizeX) * Image.SizeY * (bGray ? 1 : 4)) return false;

    // The Quest capture path already produces this canvas size. Avoid a full
    // per-pixel scaling pass while preserving the existing opaque-alpha rule.
    if (!bGray && Image.SizeX == CanvasWidth && Image.SizeY == CanvasHeight)
    {
        Pixels.SetNumUninitialized(CanvasBytes);
        FMemory::Memcpy(Pixels.GetData(), Image.RawData.GetData(), CanvasBytes);
        for (int32 I = 3; I < CanvasBytes; I += 4) Pixels[I] = 255;
        return true;
    }
    // Fixed canvas prevents texture/GC churn on repeated dimension or stream
    // changes. Integer sampling avoids an intermediate full-size float image.
    Pixels.SetNumZeroed(CanvasBytes);
    for (int32 I = 3; I < CanvasBytes; I += 4) Pixels[I] = 255;
    const bool bWide = static_cast<int64>(Image.SizeX) * CanvasHeight >= static_cast<int64>(Image.SizeY) * CanvasWidth;
    const int32 W = bWide ? CanvasWidth : FMath::Max(1, Image.SizeX * CanvasHeight / Image.SizeY);
    const int32 H = bWide ? FMath::Max(1, Image.SizeY * CanvasWidth / Image.SizeX) : CanvasHeight;
    const int32 X0 = (CanvasWidth - W) / 2, Y0 = (CanvasHeight - H) / 2;
    for (int32 Y = 0; Y < H; ++Y)
    {
        const int32 SY = Y * Image.SizeY / H;
        for (int32 X = 0; X < W; ++X)
        {
            const int32 SX = X * Image.SizeX / W;
            const uint8* Source = Image.RawData.GetData() + (SY * Image.SizeX + SX) * (bGray ? 1 : 4);
            uint8* Dest = Pixels.GetData() + ((Y + Y0) * CanvasWidth + X + X0) * 4;
            Dest[0] = Source[0]; Dest[1] = bGray ? Source[0] : Source[1]; Dest[2] = bGray ? Source[0] : Source[2];
        }
    }
    return true;
}
bool Decode(const FEncodedFrame& Frame, TArray<uint8>& Pixels)
{
    FImage Image;
    if (!FImageUtils::DecompressImage(Frame.Jpeg.GetData(), Frame.Jpeg.Num(), Image) ||
        Image.SizeX != Frame.Meta.Width || Image.SizeY != Frame.Meta.Height) return false;
    return ComposeCanvas(Image, Pixels);
}
struct FUploadBuffer
{
    TArray<uint8> Pixels;
    FUpdateTextureRegion2D Region{0, 0, 0, 0, CanvasWidth, CanvasHeight};
};
#else
bool CanRender() { return false; }
#endif
}

struct FMRFPSVideoReceiverState
{
    MRFPSVideo::FSlot Slots[MRFPSVideo::SlotCount];
    TSharedPtr<MRFPSVideo::FMailbox, ESPMode::ThreadSafe> Mailbox = MakeShared<MRFPSVideo::FMailbox, ESPMode::ThreadSafe>();
    TSharedRef<MRFPSVideo::FReceiverBudget, ESPMode::ThreadSafe> Budget = MakeShared<MRFPSVideo::FReceiverBudget, ESPMode::ThreadSafe>();
    TFuture<void> Workers[MRFPSVideo::SlotCount];
    int32 NextSlot = 0;
};

UMRFPSVideoReceiver::UMRFPSVideoReceiver() : FTickableGameObject(ETickableTickType::Never) {}
UMRFPSVideoReceiver::~UMRFPSVideoReceiver() = default;

void UMRFPSVideoReceiver::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    State = MakeShared<FMRFPSVideoReceiverState>();
#if !UE_SERVER
    if (MRFPSVideo::CanRender() && FModuleManager::Get().LoadModule(TEXT("ImageCore")) &&
        FModuleManager::LoadModulePtr<IImageWrapperModule>(TEXT("ImageWrapper")))
    {
        TArray<uint8> Black; Black.SetNumZeroed(MRFPSVideo::CanvasBytes);
        for (int32 I = 3; I < Black.Num(); I += 4) Black[I] = 255;
        Textures.SetNum(MRFPSVideo::SlotCount);
        bEnabled = true;
        for (auto& Texture : Textures)
        {
            // Supplying initial bytes calls UpdateResource exactly once inside
            // CreateTransient. One mip, no streaming, no per-frame recreation.
            Texture = UTexture2D::CreateTransient(MRFPSVideo::CanvasWidth, MRFPSVideo::CanvasHeight,
                PF_B8G8R8A8, NAME_None, TConstArrayView64<uint8>(Black.GetData(), Black.Num()));
            if (!Texture) { bEnabled = false; break; }
            Texture->NeverStream = true;
            Texture->SRGB = true;
        }
    }
#endif
    if (bEnabled) SetTickableTickType(ETickableTickType::Conditional);
    UE_LOG(LogMRFPSVideoReceiver, Display, TEXT("MRFPS_VIDEO_RECEIVER_INIT state=%s"), bEnabled ? TEXT("ENABLED") : TEXT("DISABLED"));
}

bool UMRFPSVideoReceiver::SubmitFrame(int32 Slot, uint32 StreamId, uint32 Sequence, int32 Width, int32 Height, TArray<uint8> Jpeg)
{
    using namespace MRFPSVideo;
    if (!IsInGameThread() || !bEnabled || !State || !CanRender() || !ValidSlot(Slot)) return false;
    FSlot& Target = State->Slots[Slot];
    if (!CanAccept(Target, StreamId, Sequence)) { ++Target.RejectedFrames; Target.Issue = EIssue::Order; return false; }
    if (!ValidJpegHeader(Jpeg, Width, Height)) { ++Target.RejectedFrames; Target.Issue = EIssue::Header; return false; }
    FEncodedFrame Frame;
    Frame.Meta = {Slot, StreamId, Sequence, 0, Width, Height, FPlatformTime::Seconds()};
    Frame.Jpeg = MoveTemp(Jpeg);
    Accept(Target, MoveTemp(Frame));
    ++Target.ReceivedFrames;
    return true;
}

void UMRFPSVideoReceiver::Tick(float DeltaTime)
{
    using namespace MRFPSVideo;
    if (!State || !bEnabled) return;
    if (!CanRender()) { ResetAll(); return; }
#if !UE_SERVER
    const double Now = FPlatformTime::Seconds();
    for (int32 Slot = 0; Slot < SlotCount; ++Slot)
    {
        FSlot& Target = State->Slots[Slot];
        if ((Target.Stream || Target.RejectedFrames) && Now - Target.LastDiagnosticAt >= 1)
        {
            const double Window = Target.LastDiagnosticAt > 0 ? Now - Target.LastDiagnosticAt : 0;
            const double DecodeFps = Window > 0 ? (Target.DecodedFrames - Target.DiagnosticDecoded) / Window : 0;
            const double UploadFps = Window > 0 ? (Target.UploadedFrames - Target.DiagnosticUploaded) / Window : 0;
            Target.LastDiagnosticAt = Now;
            Target.DiagnosticDecoded = Target.DecodedFrames; Target.DiagnosticUploaded = Target.UploadedFrames;
            UE_LOG(LogMRFPSVideoReceiver, Display, TEXT("MRFPS_VIDEO_RECEIVER slot=%d stream=%u received=%u decoded=%u uploaded=%u rejected=%u decodefail=%u rhidrop=%u pending=%d displayed=%u state=%s decodefps=%.1f uploadfps=%.1f window=%.2f"),
                Slot, Target.Stream, Target.ReceivedFrames, Target.DecodedFrames, Target.UploadedFrames, Target.RejectedFrames,
                Target.DecodeFailures, Target.ResourceDrops,
                Target.Pending.IsSet() ? 1 : 0, Target.Displayed.IsSet() ? Target.Displayed->Sequence : 0, DiagnosticCode(Target, Now), DecodeFps, UploadFps, Window);
        }
    }
    TOptional<FDecoded> Decoded[SlotCount];
    TOptional<FUploadAck> Uploaded[SlotCount];
    {
        FScopeLock Lock(&State->Mailbox->Mutex);
        for (int32 I = 0; I < SlotCount; ++I)
        {
            Decoded[I] = MoveTemp(State->Mailbox->Decoded[I]); State->Mailbox->Decoded[I].Reset();
            Uploaded[I] = MoveTemp(State->Mailbox->Uploaded[I]); State->Mailbox->Uploaded[I].Reset();
        }
    }
    for (int32 I = 0; I < SlotCount; ++I) if (Uploaded[I].IsSet())
    {
        const FFrameMeta& Meta = Uploaded[I]->Meta;
        if (Meta.Slot == I)
        {
            FSlot& Target = State->Slots[Meta.Slot];
            if (Matches(Target, Meta, Now) && (!Target.Displayed.IsSet() || Meta.Sequence > Target.Displayed->Sequence))
            { Target.Displayed = Meta; Target.Issue = EIssue::None; ++Target.UploadedFrames; }
        }
        Uploaded[I].Reset(); // releases this slot's permit after acknowledging RHI completion
    }
    for (int32 I = 0; I < SlotCount; ++I) if (Decoded[I].IsSet())
    {
        const FFrameMeta Meta = Decoded[I]->Meta;
        if (Meta.Slot == I && CanUpload(State->Slots[Meta.Slot], Meta, Now))
        {
            FSlot& Target = State->Slots[Meta.Slot];
            if (Decoded[I]->bSucceeded) ++Target.DecodedFrames;
            UTexture2D* Texture = Textures.IsValidIndex(Meta.Slot) ? Textures[Meta.Slot].Get() : nullptr;
            FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
            if (!Decoded[I]->bSucceeded) { ++Target.DecodeFailures; Target.Issue = EIssue::Decode; }
            else if (!Resource || !Resource->IsInitialized() || !Resource->TextureRHI.IsValid()) { ++Target.ResourceDrops; Target.Issue = EIssue::Resource; }
            else
            {
                auto Buffer = MakeShared<FUploadBuffer, ESPMode::ThreadSafe>();
                Buffer->Pixels = MoveTemp(Decoded[I]->Pixels);
                const auto Mailbox = State->Mailbox;
                const FPermit Permit = Decoded[I]->Permit;
                Texture->UpdateTextureRegions(0, 1, &Buffer->Region, CanvasWidth * 4, 4, Buffer->Pixels.GetData(),
                    [Buffer, Mailbox, Permit, Meta](uint8*, const FUpdateTextureRegion2D*)
                    {
                        FScopeLock Lock(&Mailbox->Mutex);
                        if (Mailbox->bAlive && !Mailbox->Uploaded[Meta.Slot].IsSet())
                            Mailbox->Uploaded[Meta.Slot].Emplace(FUploadAck{Meta, Permit});
                    });
            }
        }
        Decoded[I].Reset();
    }
    // Dispatch ready slots, while a bounded process-wide FIFO gives another
    // GameInstance with pending work a turn before this one re-enters.
    const auto Process = ProcessBudget();
    for (int32 Attempt = 0; Attempt < SlotCount; ++Attempt)
    {
        const unsigned BusyMask = MRFPSVideoWorkBudget::BusyMask(*State->Budget);
        const int32 Slot = FindReadySlot(State->Slots, State->NextSlot, Now, BusyMask);
        if (Slot == INDEX_NONE) { MRFPSVideoWorkBudget::CancelDemand(*Process, *State->Budget); break; }
        FSlot& Target = State->Slots[Slot];
        const FPermit Permit = AcquirePermit(Process, State->Budget, Slot, Now);
        if (!Permit.IsValid()) break;
        FEncodedFrame Frame = MoveTemp(Target.Pending.GetValue()); Target.Pending.Reset();
        Target.LastDispatch = Now; State->NextSlot = (Slot + 1) % SlotCount;
        const auto Mailbox = State->Mailbox;
        State->Workers[Slot] = Async(EAsyncExecution::ThreadPool, [Frame = MoveTemp(Frame), Mailbox, Permit]() mutable
        {
            FDecoded Result; Result.Meta = Frame.Meta; Result.Permit = Permit;
            Result.bSucceeded = Decode(Frame, Result.Pixels);
            FScopeLock Lock(&Mailbox->Mutex);
            if (Mailbox->bAlive && !Mailbox->Decoded[Frame.Meta.Slot].IsSet())
                Mailbox->Decoded[Frame.Meta.Slot].Emplace(MoveTemp(Result));
        });
    }
#endif
}

UTexture2D* UMRFPSVideoReceiver::GetFreshTexture(int32 Slot) const
{
    using namespace MRFPSVideo;
    if (!IsInGameThread() || !bEnabled || !State || !CanRender() || !ValidSlot(Slot) || !Textures.IsValidIndex(Slot)) return nullptr;
    const FSlot& Target = State->Slots[Slot];
    if (!Target.Displayed.IsSet() || !Matches(Target, Target.Displayed.GetValue(), FPlatformTime::Seconds())) return nullptr;
    return Textures[Slot].Get();
}

UTexture2D* UMRFPSVideoReceiver::GetFreshTextureForStream(int32 Slot, uint32 ExpectedStreamId) const
{
    UTexture2D* Texture = GetFreshTexture(Slot);
    return Texture && State && MRFPSVideo::DisplayedForStream(State->Slots[Slot], ExpectedStreamId) ? Texture : nullptr;
}

FString UMRFPSVideoReceiver::GetSafeStatus(int32 Slot) const
{
    using namespace MRFPSVideo;
    if (!IsInGameThread() || !bEnabled || !State || !CanRender()) return TEXT("Video unavailable (no renderer)");
    if (!ValidSlot(Slot)) return TEXT("Video unavailable (invalid slot)");
    const FSlot& Target = State->Slots[Slot];
    if (Target.bClosed) return TEXT("Video stopped / waiting for a new stream");
    if (GetFreshTexture(Slot)) return FString::Printf(TEXT("Video live / received %d ms ago"),
        FMath::Max(0, FMath::RoundToInt((FPlatformTime::Seconds() - Target.Displayed->ArrivedAt) * 1000)));
    if (Target.Displayed.IsSet()) return TEXT("Video stale / no fresh frame");
    if (Target.Issue == EIssue::Header) return TEXT("Video unavailable (JPEG rejected)");
    if (Target.Issue == EIssue::Order) return TEXT("Video unavailable (old frame rejected)");
    if (Target.Issue == EIssue::Decode) return TEXT("Video unavailable (decode failed)");
    if (Target.Issue == EIssue::Resource) return TEXT("Video waiting for renderer");
    return Target.Stream && !Target.bClosed ? TEXT("Video receiving / waiting for fresh frame") : TEXT("No real video");
}
FString UMRFPSVideoReceiver::GetSafeStatusForStream(int32 Slot, uint32 ExpectedStreamId) const
{
    if (!IsInGameThread() || !bEnabled || !State || !MRFPSVideo::CanRender()) return TEXT("Video unavailable (no renderer)");
    if (!MRFPSVideo::ValidSlot(Slot)) return TEXT("Video unavailable (invalid slot)");
    if (!ExpectedStreamId || State->Slots[Slot].Stream != ExpectedStreamId) return TEXT("Video waiting for current stream");
    return GetSafeStatus(Slot);
}
void UMRFPSVideoReceiver::StopStream(int32 Slot, uint32 StoppedStreamId)
{
    if (IsInGameThread() && State && MRFPSVideo::ValidSlot(Slot)) MRFPSVideo::StopSlot(State->Slots[Slot], StoppedStreamId);
}

void UMRFPSVideoReceiver::ResetSlot(int32 Slot)
{
    if (IsInGameThread() && State && MRFPSVideo::ValidSlot(Slot)) MRFPSVideo::ClearSlot(State->Slots[Slot], false);
}
void UMRFPSVideoReceiver::ResetAll()
{
    if (!IsInGameThread() || !State) return;
    MRFPSVideoWorkBudget::CancelDemand(*MRFPSVideo::ProcessBudget(), *State->Budget);
    for (auto& Slot : State->Slots) MRFPSVideo::ClearSlot(Slot, true);
    FScopeLock Lock(&State->Mailbox->Mutex);
    for (int32 I = 0; I < MRFPSVideo::SlotCount; ++I)
    { State->Mailbox->Decoded[I].Reset(); State->Mailbox->Uploaded[I].Reset(); }
}
void UMRFPSVideoReceiver::StopReceiver()
{
    bEnabled = false;
    SetTickableTickType(ETickableTickType::Never);
    if (State)
    {
        if (IsInGameThread()) MRFPSVideoWorkBudget::CancelDemand(*MRFPSVideo::ProcessBudget(), *State->Budget);
        { FScopeLock Lock(&State->Mailbox->Mutex); State->Mailbox->bAlive = false;
          for (int32 I = 0; I < MRFPSVideo::SlotCount; ++I)
          { State->Mailbox->Decoded[I].Reset(); State->Mailbox->Uploaded[I].Reset(); } }
        // Work is bounded local JPEG decode and never waits on the game thread.
        // Finish it before engine codec modules can be unloaded during shutdown.
        for (auto& Worker : State->Workers) if (Worker.IsValid()) Worker.Wait();
        State.Reset();
    }
    Textures.Empty();
}
void UMRFPSVideoReceiver::Deinitialize() { StopReceiver(); Super::Deinitialize(); }
void UMRFPSVideoReceiver::BeginDestroy() { StopReceiver(); Super::BeginDestroy(); }
bool UMRFPSVideoReceiver::IsTickable() const { return bEnabled && State.IsValid() && !IsTemplate(); }
TStatId UMRFPSVideoReceiver::GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(UMRFPSVideoReceiver, STATGROUP_Tickables); }
UWorld* UMRFPSVideoReceiver::GetTickableGameObjectWorld() const { return GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr; }

#endif // UE_SERVER
