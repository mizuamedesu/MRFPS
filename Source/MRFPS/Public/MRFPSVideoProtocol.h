#pragma once

#include "CoreMinimal.h"
#include "MRFPSVideoProtocol.generated.h"

namespace MRFPSVideo
{
    constexpr int32 MaxFrameBytes = 60 * 1024;
    constexpr int32 ChunkBytes = 512;
    constexpr int32 DataChunksPerParity = 8;
    constexpr int32 MaxDataChunks = (MaxFrameBytes + ChunkBytes - 1) / ChunkBytes;
    constexpr int32 MaxParityChunks = (MaxDataChunks + DataChunksPerParity - 1) / DataChunksPerParity;
    constexpr int32 MaxParityBytes = MaxParityChunks * ChunkBytes;
    // A slow game tick must be able to finish a whole bounded frame, including
    // its padded parity rows, before the unchanged frame deadline expires.
    constexpr int32 MaxWireChunksPerFrame = MaxDataChunks + MaxParityChunks;
    constexpr int32 PublisherChunkBudgetPerTick = MaxWireChunksPerFrame;
    constexpr int32 RelayChunkBudgetPerTick = 2 * MaxWireChunksPerFrame;
    constexpr int32 PublisherSustainedBytesPerSecond = 3 * 1024 * 1024 / 2;
    constexpr int32 PublisherBurstBytes = 256 * 1024;
    // Admission headroom accommodates a 30 Hz producer's arrival jitter.
    constexpr double MinimumAcceptedFrameInterval = 1.0 / 60.0;
    constexpr uint8 ParityFlag = 0x80;
    constexpr int32 MaxWidth = 1280;
    constexpr int32 MaxHeight = 720;
    constexpr double FrameLifetime = 0.75;

    // A detached player window remains subscribed even when the main window
    // switches to reconstructed 3D, hides thumbnails, or follows the other slot.
    inline uint8 ObserverSubscription(bool Reconstructed,bool MultiView,int32 ViewSlot,uint8 DetachedMask)
    {
        const uint8 Main=Reconstructed ? 0 : MultiView ? 3 : ViewSlot>=0 && ViewSlot<2 ? (1u<<ViewSlot) : 0;
        return (Main | DetachedMask) & 3;
    }

    struct FPublisherRateCheck
    {
        double DecayedBytes = 0;
        double ChargedBytes = 0;
        bool bAllowed = false;
    };
    // Preview admission without charging a rejected chunk. The caller stores
    // DecayedBytes on every attempt and commits ChargedBytes only after its
    // bounded assembler accepts the payload. Stop never resets this budget.
    inline FPublisherRateCheck CheckPublisherRate(double CurrentBytes,double ElapsedSeconds,int32 PayloadBytes)
    {
        const bool ValidState=FMath::IsFinite(CurrentBytes) && FMath::IsFinite(ElapsedSeconds);
        const double Capped=FMath::IsFinite(CurrentBytes) ? FMath::Clamp(CurrentBytes,0.0,double(PublisherBurstBytes)) : double(PublisherBurstBytes);
        const double Elapsed=FMath::IsFinite(ElapsedSeconds) ? FMath::Clamp(ElapsedSeconds,0.0,1.0) : 0.0;
        FPublisherRateCheck Result;
        Result.DecayedBytes=FMath::Max(0.0,Capped-Elapsed*PublisherSustainedBytesPerSecond);
        Result.bAllowed=ValidState && PayloadBytes>0 && PayloadBytes<=ChunkBytes &&
            double(PayloadBytes)<=double(PublisherBurstBytes)-Result.DecayedBytes;
        Result.ChargedBytes=Result.DecayedBytes+(Result.bAllowed ? PayloadBytes : 0);
        return Result;
    }
}

// Each RPC fits the production 1024-byte DTLS datagram. Validate the length
// before allocating: a remote array length must never become an allocation.
USTRUCT()
struct FMRFPSVideoChunk
{
    GENERATED_BODY()
    uint8 Slot = 0;
    uint32 StreamId = 0;
    uint32 Sequence = 0;
    uint16 Width = 0;
    uint16 Height = 0;
    uint16 TotalBytes = 0;
    // Data: 0..119. Parity: bit 7 set, low 7 bits identify group 0..14.
    uint8 Index = 0;
    TArray<uint8> Payload;
    bool IsSane() const;
    bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bSuccess);
};

template<> struct TStructOpsTypeTraits<FMRFPSVideoChunk> : TStructOpsTypeTraitsBase2<FMRFPSVideoChunk>
{
    enum { WithNetSerializer = true, WithCopy = true };
};

// One incomplete frame per stream. Latest sequence wins; duplicates, conflicting
// metadata, expired partial frames and completed-frame replay are rejected.
// One padded XOR row per eight data chunks repairs exactly one missing member.
class MRFPS_API FMRFPSVideoAssembler
{
public:
    // Lifetime metadata only; survives stream resets and never stores pixels.
    uint64 CompletedFrames=0, SupersededFrames=0, ExpiredFrames=0, RecoveredChunks=0;
    bool Add(const FMRFPSVideoChunk& Chunk, double Now);
    bool Take(FMRFPSVideoChunk& Header, TArray<uint8>& Jpeg);
    void Reset();
    int32 AllocatedBytes() const { return Buffer.Num() + ParityRows.Num(); }
private:
    FMRFPSVideoChunk Meta;
    TArray<uint8> Buffer;
    TArray<uint8> ParityRows;
    TBitArray<> Received;
    TBitArray<> ReceivedParity;
    int32 Count = 0;
    uint32 SeenStream = 0;
    uint32 CompletedSequence = 0;
    double StartedAt = 0;
    bool bComplete = false;
    void DiscardPartial();
    void RecoverGroup(int32 Group);
};

// Current frame plus latest waiting frame only. Network backpressure never
// builds an unbounded video queue or interrupts gameplay's reliable messages.
class MRFPS_API FMRFPSVideoSendQueue
{
public:
    uint64 PushedFrames=0, ReplacedFrames=0, ExpiredFrames=0, SentChunks=0, FinishedFrames=0;
    void Push(const FMRFPSVideoChunk& Header, const TArray<uint8>& Jpeg, double Now);
    bool Next(FMRFPSVideoChunk& Chunk, double Now);
    void Reset();
    bool HasData() const { return !Current.IsEmpty() || !Latest.IsEmpty(); }
private:
    FMRFPSVideoChunk CurrentHeader;
    FMRFPSVideoChunk LatestHeader;
    TArray<uint8> Current;
    TArray<uint8> Latest;
    int32 Cursor = 0;
    int32 PendingParityGroup = -1;
    double CurrentAt = 0;
    double LatestAt = 0;
};
