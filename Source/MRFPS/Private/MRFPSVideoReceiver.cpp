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

#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "Misc/AutomationTest.h"
#include <limits>

namespace MRFPSVideoTests
{
bool MakeJpeg(TArray<uint8>& Packet)
{
    FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    FImage Image(16, 8, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
    for (int64 I = 0; I < Image.RawData.Num(); I += 4)
    { Image.RawData[I] = 32; Image.RawData[I + 1] = 128; Image.RawData[I + 2] = 224; Image.RawData[I + 3] = 255; }
    TArray64<uint8> Compressed;
    if (!FImageUtils::CompressImage(Compressed, TEXT("jpg"), Image, 85) || Compressed.Num() > MRFPSVideo::MaxJpegBytes) return false;
    Packet.Append(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
    return true;
}
MRFPSVideo::FEncodedFrame Frame(uint32 Stream, uint32 Sequence, double Time)
{
    MRFPSVideo::FEncodedFrame Result;
    Result.Meta = {0, Stream, Sequence, 0, 16, 8, Time};
    return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoHeaderTest, "MRFPS.Video.JpegValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoHeaderTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSVideo;
    TArray<uint8> Packet;
    if (!TestTrue(TEXT("Real engine JPEG encoder produces the fixture"), MRFPSVideoTests::MakeJpeg(Packet))) return false;
    TestTrue(TEXT("Actual JPEG dimensions agree with the envelope"), ValidJpegHeader(Packet, 16, 8));
    TestFalse(TEXT("Width is checked against encoded SOF, not trusted metadata"), ValidJpegHeader(Packet, 17, 8));
    TestFalse(TEXT("Height is checked against encoded SOF"), ValidJpegHeader(Packet, 16, 9));
    TestFalse(TEXT("Invalid dimensions cannot allocate pixels"), ValidJpegHeader(Packet, MAX_int32, MAX_int32));
    TestFalse(TEXT("Zero dimensions are rejected"), ValidJpegHeader(Packet, 0, 8));
    TArray<uint8> Truncated = Packet; Truncated.Pop();
    TestFalse(TEXT("Truncated JPEG is rejected before codec entry"), ValidJpegHeader(Truncated, 16, 8));
    TArray<uint8> Oversized = Packet; Oversized.SetNumZeroed(MaxJpegBytes + 1);
    TestFalse(TEXT("Encoded byte cap is independent of dimensions"), ValidJpegHeader(Oversized, 16, 8));
    const TArray<uint8> BadSegment{0xff,0xd8,0xff,0xe1,0xff,0xff,0xff,0xd9};
    TestFalse(TEXT("Segment length cannot escape the bounded packet"), ValidJpegHeader(BadSegment, 16, 8));
    FRandomStream Random(640360);
    for (int32 I = 0; I < 200; ++I)
    {
        TArray<uint8> Bytes; Bytes.SetNumUninitialized(4 + I % 128);
        for (uint8& Byte : Bytes) Byte = static_cast<uint8>(Random.RandRange(0,255));
        Bytes[0]=0xff; Bytes[1]=0xd8; Bytes[Bytes.Num()-2]=0xff; Bytes.Last()=0xd9;
        TestFalse(TEXT("Bounded malformed marker input is rejected"), ValidJpegHeader(Bytes,640,360));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoDecodeTest, "MRFPS.Video.NativeDecode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoDecodeTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSVideo;
    FEncodedFrame Input = MRFPSVideoTests::Frame(1,1,100);
    if (!TestTrue(TEXT("Real JPEG fixture"),MRFPSVideoTests::MakeJpeg(Input.Jpeg))) return false;
    TArray<uint8> Pixels;
    if (!TestTrue(TEXT("Production decoder uses real engine JPEG codec without GPU/network"),Decode(Input,Pixels))) return false;
    TestEqual(TEXT("Output allocation is the fixed bounded canvas"),Pixels.Num(),CanvasBytes);
    TestEqual(TEXT("Letterbox is black"),Pixels[0],static_cast<uint8>(0));
    TestEqual(TEXT("Letterbox stays opaque"),Pixels[3],static_cast<uint8>(255));
    const int32 Center=(CanvasHeight/2*CanvasWidth+CanvasWidth/2)*4;
    TestTrue(TEXT("JPEG B channel survives decoding/resampling"),FMath::Abs(static_cast<int32>(Pixels[Center])-32)<24);
    TestTrue(TEXT("JPEG G channel survives decoding/resampling"),FMath::Abs(static_cast<int32>(Pixels[Center+1])-128)<24);
    TestTrue(TEXT("JPEG R channel survives decoding/resampling"),FMath::Abs(static_cast<int32>(Pixels[Center+2])-224)<24);
    TestEqual(TEXT("Video pixels stay opaque"),Pixels[Center+3],static_cast<uint8>(255));
    Input.Meta.Width=15;
    TestFalse(TEXT("Worker independently rechecks decoded dimensions"),Decode(Input,Pixels));
    FImage Native(CanvasWidth,CanvasHeight,ERawImageFormat::BGRA8,EGammaSpace::sRGB);
    for (int64 I=0; I<Native.RawData.Num(); I+=4)
    { Native.RawData[I]=17; Native.RawData[I+1]=33; Native.RawData[I+2]=49; Native.RawData[I+3]=0; }
    TestTrue(TEXT("Native canvas size uses the bounded copy path"),ComposeCanvas(Native,Pixels));
    TestEqual(TEXT("Native canvas allocation stays fixed"),Pixels.Num(),CanvasBytes);
    TestEqual(TEXT("Native canvas copies B"),Pixels[Center],static_cast<uint8>(17));
    TestEqual(TEXT("Native canvas copies G"),Pixels[Center+1],static_cast<uint8>(33));
    TestEqual(TEXT("Native canvas copies R"),Pixels[Center+2],static_cast<uint8>(49));
    TestEqual(TEXT("Native canvas always forces opaque alpha"),Pixels[Center+3],static_cast<uint8>(255));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoSchedulingTest, "MRFPS.Video.FairScheduling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoSchedulingTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSVideo;
    FSlot Slots[SlotCount]; int32 Next = 0; int32 Dispatches[SlotCount] = {0,0};
    for (uint32 Sequence = 1; Sequence <= 120; ++Sequence)
    {
        const double Now = 100 + Sequence / 60.0;
        // Both sources always have a newer latest frame when the sole work
        // permit becomes available. Source zero must not win every dispatch.
        for (int32 I = 0; I < SlotCount; ++I) Accept(Slots[I],MRFPSVideoTests::Frame(1,Sequence,Now));
        const int32 Ready = FindReadySlot(Slots,Next,Now);
        if (!TestTrue(TEXT("A ready slot exists at 60 Hz aggregate scheduling"),ValidSlot(Ready))) return false;
        TestEqual(TEXT("Successful dispatch rotates priority"),Ready,Next);
        Slots[Ready].LastDispatch=Now; Slots[Ready].Pending.Reset(); ++Dispatches[Ready]; Next=(Ready+1)%SlotCount;
    }
    TestEqual(TEXT("First continuous source receives half the opportunities"),Dispatches[0],60);
    TestEqual(TEXT("Second continuous source cannot starve"),Dispatches[1],60);
    for (int32 I=0;I<SlotCount;++I) {
        Accept(Slots[I],MRFPSVideoTests::Frame(1,121,103)); Slots[I].LastDispatch=103;
    }
    TestEqual(TEXT("Per-slot gate prevents redundant immediate work"),FindReadySlot(Slots,Next,103),INDEX_NONE);
    Slots[0].Pending.Reset(); Slots[1].LastDispatch=102;
    TestEqual(TEXT("Missing preferred source does not idle the other"),FindReadySlot(Slots,0,103),1);
    TestEqual(TEXT("A busy source cannot prevent an idle source from progressing"),FindReadySlot(Slots,0,103,1u),1);
    TestEqual(TEXT("The busy source cannot be dispatched again"),FindReadySlot(Slots,0,103,2u),INDEX_NONE);
    TestEqual(TEXT("Expired pending input is discarded instead of scheduled"),FindReadySlot(Slots,0,104),INDEX_NONE);
    TestFalse(TEXT("Discard leaves no stale queued JPEG"),Slots[1].Pending.IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoConcurrentBudgetTest, "MRFPS.Video.ConcurrentBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoConcurrentBudgetTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSVideo;
    const auto Global=MakeShared<FProcessBudget, ESPMode::ThreadSafe>();
    const auto First=MakeShared<FReceiverBudget, ESPMode::ThreadSafe>();
    const auto Second=MakeShared<FReceiverBudget, ESPMode::ThreadSafe>();
    FPermit Left=AcquirePermit(Global,First,0,100), Right=AcquirePermit(Global,First,1,100);
    if (!TestTrue(TEXT("Both views may hold a decode/upload permit together"),Left.IsValid() && Right.IsValid())) return false;
    TestEqual(TEXT("Process-wide in-flight work is bounded to two"),Global->Active.load(),2);
    TestEqual(TEXT("Both receiver slots are busy"),MRFPSVideoWorkBudget::BusyMask(*First),3u);
    TestFalse(TEXT("A second frame cannot overlap a slot"),AcquirePermit(Global,First,0,100).IsValid());
    TestFalse(TEXT("A second receiver cannot exceed the process cap"),AcquirePermit(Global,Second,0,100).IsValid());

    FMailbox Mailbox;
    FDecoded LeftDone; LeftDone.Meta.Slot=0; LeftDone.Meta.Sequence=1; LeftDone.Permit=Left;
    FDecoded RightDone; RightDone.Meta.Slot=1; RightDone.Meta.Sequence=2; RightDone.Permit=Right;
    Mailbox.Decoded[0].Emplace(MoveTemp(LeftDone)); Mailbox.Decoded[1].Emplace(MoveTemp(RightDone));
    Left.Reset(); Right.Reset();
    TestEqual(TEXT("A pending completion keeps both permits occupied"),Global->Active.load(),2);
    TestEqual(TEXT("Each view keeps its own completion"),Mailbox.Decoded[0]->Meta.Sequence,static_cast<uint32>(1));
    TestEqual(TEXT("The second completion is not overwritten"),Mailbox.Decoded[1]->Meta.Sequence,static_cast<uint32>(2));
    Mailbox.Decoded[0].Reset();
    TestEqual(TEXT("A consumed completion releases exactly one permit"),Global->Active.load(),1);
    FPermit Other=AcquirePermit(Global,Second,0,100.1);
    TestTrue(TEXT("A different receiver may use the released permit"),Other.IsValid());
    TestFalse(TEXT("A still-pending upload keeps its slot busy"),AcquirePermit(Global,First,1,100.1).IsValid());
    Mailbox.Decoded[1].Reset(); Other.Reset();
    TestEqual(TEXT("All permits return when their last references are gone"),Global->Active.load(),0);
    TestEqual(TEXT("Original slot occupancy is cleared"),MRFPSVideoWorkBudget::BusyMask(*First),0u);

    FPermit Deferred;
    TWeakPtr<FReceiverBudget, ESPMode::ThreadSafe> WeakReceiver;
    {
        const auto Transient=MakeShared<FReceiverBudget, ESPMode::ThreadSafe>();
        WeakReceiver=Transient;
        Deferred=AcquirePermit(Global,Transient,0,100.2);
    }
    TestTrue(TEXT("A late callback keeps its receiver budget alive"),Deferred.IsValid() && WeakReceiver.IsValid());
    Deferred.Reset();
    TestFalse(TEXT("Receiver budget is released after the last callback"),WeakReceiver.IsValid());
    TestEqual(TEXT("Late callback returns the process permit"),Global->Active.load(),0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoReceiverFairnessTest, "MRFPS.Video.ReceiverFairness",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoReceiverFairnessTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSVideo;
    const auto Global=MakeShared<FProcessBudget, ESPMode::ThreadSafe>();
    const auto First=MakeShared<FReceiverBudget, ESPMode::ThreadSafe>();
    const auto Second=MakeShared<FReceiverBudget, ESPMode::ThreadSafe>();
    FPermit FirstLeft=AcquirePermit(Global,First,0,100), FirstRight=AcquirePermit(Global,First,1,100);
    if (!TestTrue(TEXT("One receiver can fill both idle permits"),FirstLeft.IsValid() && FirstRight.IsValid())) return false;
    TestFalse(TEXT("Another receiver queues while permits are busy"),AcquirePermit(Global,Second,0,100.01).IsValid());
    FirstLeft.Reset();
    TestFalse(TEXT("Earlier-ticking receiver cannot overtake an existing waiter"),AcquirePermit(Global,First,0,100.02).IsValid());
    FPermit SecondLeft=AcquirePermit(Global,Second,0,100.03);
    if (!TestTrue(TEXT("The waiting receiver gets the released permit"),SecondLeft.IsValid())) return false;
    FirstRight.Reset();
    TestFalse(TEXT("The other receiver also cannot skip the first receiver's turn"),AcquirePermit(Global,Second,1,100.04).IsValid());
    FirstRight=AcquirePermit(Global,First,1,100.05);
    TestTrue(TEXT("The first receiver advances next"),FirstRight.IsValid());
    FirstRight.Reset(); SecondLeft.Reset();
    MRFPSVideoWorkBudget::CancelDemand(*Global,*Second);
    TestEqual(TEXT("Explicit cancellation removes waiting state"),Global->WaiterCount,0);

    FirstLeft=AcquirePermit(Global,First,0,101); FirstRight=AcquirePermit(Global,First,1,101);
    TestFalse(TEXT("An absent receiver can leave one pending turn"),AcquirePermit(Global,Second,0,101).IsValid());
    FirstLeft.Reset();
    FirstLeft=AcquirePermit(Global,First,0,101+MRFPSVideoWorkBudget::DemandFreshSeconds);
    TestTrue(TEXT("An abandoned waiter expires instead of blocking forever"),FirstLeft.IsValid());
    FirstLeft.Reset(); FirstRight.Reset();
    TestEqual(TEXT("Fair scheduling returns every permit"),Global->Active.load(),0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoLifecycleTest, "MRFPS.Video.FrameLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoLifecycleTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSVideo;
    FSlot Slot;
    TestFalse(TEXT("Stream zero is invalid"),CanAccept(Slot,0,1));
    TestFalse(TEXT("Sequence zero is invalid"),CanAccept(Slot,1,0));
    TestTrue(TEXT("First nonzero generation is accepted"),CanAccept(Slot,10,1));
    Accept(Slot,MRFPSVideoTests::Frame(10,1,100));
    const FFrameMeta First=Slot.Pending->Meta;
    TestTrue(TEXT("Current completion is usable within TTL"),CanUpload(Slot,First,100.5));
    TestFalse(TEXT("An exact one-second-old frame is expired"),CanUpload(Slot,First,101));
    TestFalse(TEXT("Future timestamp is not fresh"),CanUpload(Slot,First,99));
    TestFalse(TEXT("Nonfinite timestamp cannot be fresh"),Fresh(100,std::numeric_limits<double>::quiet_NaN()));
    TestFalse(TEXT("Duplicate sequence is rejected"),CanAccept(Slot,10,1));
    TestFalse(TEXT("Older generation is rejected even with a larger sequence"),CanAccept(Slot,9,999));
    Slot.Displayed=First;
    TestTrue(TEXT("Displayed frame matches its expected publisher generation"),DisplayedForStream(Slot,10));
    TestFalse(TEXT("Zero expected stream can never expose a displayed frame"),DisplayedForStream(Slot,0));
    TestFalse(TEXT("Fresh old player image is hidden immediately for a new expected publisher"),DisplayedForStream(Slot,11));
    TestFalse(TEXT("Already displayed completion is not uploaded again"),CanUpload(Slot,First,100.5));
    Accept(Slot,MRFPSVideoTests::Frame(10,2,100.2));
    const FFrameMeta SlowerWorker=Slot.Pending->Meta;
    Accept(Slot,MRFPSVideoTests::Frame(10,3,100.3));
    const FFrameMeta LatestPending=Slot.Pending->Meta;
    TestTrue(TEXT("Fresh slower worker advances display despite newer pending input"),CanUpload(Slot,SlowerWorker,100.5));
    TestEqual(TEXT("Latest pending frame is retained during an older completion"),Slot.Pending->Meta.Sequence,static_cast<uint32>(3));
    Slot.Displayed=SlowerWorker;
    TestTrue(TEXT("Latest pending completion can subsequently advance display"),CanUpload(Slot,LatestPending,100.5));
    Slot.Displayed=LatestPending;
    TestFalse(TEXT("Older completion cannot regress a newer displayed sequence"),CanUpload(Slot,SlowerWorker,100.5));
    const FFrameMeta PreviousGeneration=LatestPending;
    Accept(Slot,MRFPSVideoTests::Frame(11,1,100.3));
    TestFalse(TEXT("Pending new publisher is not yet GPU-acknowledged display"),DisplayedForStream(Slot,11));
    TestFalse(TEXT("Old decode/upload completion cannot cross a generation change"),Matches(Slot,PreviousGeneration,100.5));
    TestFalse(TEXT("Liveness allowance never permits an older generation upload"),CanUpload(Slot,PreviousGeneration,100.5));
    TestFalse(TEXT("Retired stream cannot reopen"),CanAccept(Slot,10,9999));
    ClearSlot(Slot,false);
    TestFalse(TEXT("ResetSlot closes the old generation"),CanAccept(Slot,11,2));
    TestTrue(TEXT("A higher server generation can reopen the slot"),CanAccept(Slot,12,1));
    Accept(Slot,MRFPSVideoTests::Frame(12,1,100.4));
    const FFrameMeta OldSession=Slot.Pending->Meta;
    ClearSlot(Slot,true);
    Accept(Slot,MRFPSVideoTests::Frame(12,1,100.5));
    TestFalse(TEXT("ResetAll fences old work even if server IDs are reused"),Matches(Slot,OldSession,100.6));
    TestFalse(TEXT("Liveness allowance never permits a pre-reset worker upload"),CanUpload(Slot,OldSession,100.6));
    const FFrameMeta Current=Slot.Pending->Meta;
    const uint64 CurrentEpoch=Slot.Epoch;
    TestFalse(TEXT("An older server stop cannot close a newer pending stream"),StopSlot(Slot,11));
    TestEqual(TEXT("Old stop preserves the current local epoch"),Slot.Epoch,CurrentEpoch);
    TestTrue(TEXT("Newer pending work remains uploadable after delayed old stop"),CanUpload(Slot,Current,100.6));
    Slot.Displayed=Current;
    TestFalse(TEXT("An old stop cannot blank a newer displayed texture"),StopSlot(Slot,11));
    TestTrue(TEXT("Displayed current publisher remains exposed"),DisplayedForStream(Slot,12));
    TestTrue(TEXT("Stop for the current publisher closes it"),StopSlot(Slot,12));
    TestFalse(TEXT("Its late queued/decode completion cannot publish"),CanUpload(Slot,Current,100.6));
    TestFalse(TEXT("Closed stream cannot restart with a later sequence"),CanAccept(Slot,12,999));
    FSlot BeforeFirstFrame;
    TestTrue(TEXT("Stop before the first frame records its server generation"),StopSlot(BeforeFirstFrame,20));
    TestFalse(TEXT("Delayed first frame cannot reopen an already stopped stream"),CanAccept(BeforeFirstFrame,20,1));
    TestFalse(TEXT("Older first frame cannot cross the stop watermark"),CanAccept(BeforeFirstFrame,19,999));
    TestTrue(TEXT("Only a higher server generation may open after the stop"),CanAccept(BeforeFirstFrame,21,1));
    FSlot Wrap;
    Accept(Wrap,MRFPSVideoTests::Frame(1,MAX_uint32,100));
    TestFalse(TEXT("Sequence wrap is rejected"),CanAccept(Wrap,1,1));
    TestTrue(TEXT("New generation permits a reset sequence"),CanAccept(Wrap,2,1));
    return true;
}
#endif

#endif // UE_SERVER
