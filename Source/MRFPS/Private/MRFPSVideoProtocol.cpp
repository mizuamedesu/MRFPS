#include "MRFPSVideoProtocol.h"

namespace
{
int32 DataChunkCount(int32 TotalBytes)
{
    return FMath::DivideAndRoundUp(TotalBytes, MRFPSVideo::ChunkBytes);
}
int32 ParityChunkCount(int32 TotalBytes)
{
    return FMath::DivideAndRoundUp(DataChunkCount(TotalBytes), MRFPSVideo::DataChunksPerParity);
}
bool IsParity(const FMRFPSVideoChunk& C) { return (C.Index & MRFPSVideo::ParityFlag) != 0; }
int32 ParityGroup(const FMRFPSVideoChunk& C) { return C.Index & 0x7f; }
bool SaneHeader(const FMRFPSVideoChunk& C)
{
    const bool bCommon = C.Slot < 2 && C.StreamId != 0 && C.Sequence != 0 &&
        C.Width > 0 && C.Width <= MRFPSVideo::MaxWidth && C.Height > 0 && C.Height <= MRFPSVideo::MaxHeight &&
        C.TotalBytes >= 4 && C.TotalBytes <= MRFPSVideo::MaxFrameBytes;
    if (!bCommon) return false;
    return IsParity(C) ? ParityGroup(C) < ParityChunkCount(C.TotalBytes) :
        int32(C.Index) < DataChunkCount(C.TotalBytes);
}
int32 PayloadSize(const FMRFPSVideoChunk& C)
{
    return IsParity(C) ? MRFPSVideo::ChunkBytes :
        FMath::Min(MRFPSVideo::ChunkBytes, int32(C.TotalBytes) - int32(C.Index) * MRFPSVideo::ChunkBytes);
}
}

bool FMRFPSVideoChunk::IsSane() const { return SaneHeader(*this) && Payload.Num() == PayloadSize(*this); }

bool FMRFPSVideoChunk::NetSerialize(FArchive& Ar, UPackageMap*, bool& bSuccess)
{
    bSuccess = false;
    if (Ar.IsSaving() && !IsSane()) { Ar.SetError(); return false; }
    Ar << Slot << StreamId << Sequence << Width << Height << TotalBytes << Index;
    uint16 Length = static_cast<uint16>(Payload.Num());
    Ar << Length;
    if (Ar.IsError() || !SaneHeader(*this) || Length > MRFPSVideo::ChunkBytes || Length != PayloadSize(*this))
    {
        Ar.SetError(); return false;
    }
    if (Ar.IsLoading()) Payload.SetNumUninitialized(Length);
    Ar.Serialize(Payload.GetData(), Length);
    bSuccess = !Ar.IsError();
    return bSuccess;
}

void FMRFPSVideoAssembler::DiscardPartial()
{
    Buffer.Reset(); ParityRows.Reset(); Received.Reset(); ReceivedParity.Reset(); Count = 0; bComplete = false;
}
void FMRFPSVideoAssembler::Reset()
{
    DiscardPartial(); Meta = FMRFPSVideoChunk(); SeenStream = CompletedSequence = 0;
}
bool FMRFPSVideoAssembler::Add(const FMRFPSVideoChunk& C, double Now)
{
    if (!C.IsSane() || !FMath::IsFinite(Now)) return false;
    if (C.StreamId < SeenStream) return false;
    if (C.StreamId > SeenStream) { Reset(); SeenStream = C.StreamId; }
    if (!Buffer.IsEmpty() && Now - StartedAt > MRFPSVideo::FrameLifetime)
    {
        ++ExpiredFrames;
        CompletedSequence = FMath::Max(CompletedSequence, Meta.Sequence); DiscardPartial();
    }
    if (C.Sequence <= CompletedSequence) return false;
    if (!Buffer.IsEmpty() && C.Sequence < Meta.Sequence) return false;
    if (Buffer.IsEmpty() || C.Sequence > Meta.Sequence)
    {
        if (!Buffer.IsEmpty()) ++SupersededFrames;
        DiscardPartial(); Meta = C; Meta.Payload.Reset();
        Buffer.SetNumUninitialized(C.TotalBytes);
        Received.Init(false, DataChunkCount(C.TotalBytes));
        ReceivedParity.Init(false, ParityChunkCount(C.TotalBytes));
        ParityRows.SetNumUninitialized(ReceivedParity.Num() * MRFPSVideo::ChunkBytes);
        StartedAt = Now;
    }
    if (C.Slot != Meta.Slot || C.Width != Meta.Width || C.Height != Meta.Height || C.TotalBytes != Meta.TotalBytes) return false;
    const int32 Group = IsParity(C) ? ParityGroup(C) : int32(C.Index) / MRFPSVideo::DataChunksPerParity;
    if (IsParity(C))
    {
        uint8* Row = ParityRows.GetData() + Group * MRFPSVideo::ChunkBytes;
        if (ReceivedParity[Group]) return FMemory::Memcmp(Row, C.Payload.GetData(), C.Payload.Num()) == 0;
        FMemory::Memcpy(Row, C.Payload.GetData(), C.Payload.Num());
        ReceivedParity[Group] = true;
    }
    else
    {
        const int32 Offset = int32(C.Index) * MRFPSVideo::ChunkBytes;
        if (Received[C.Index]) return FMemory::Memcmp(Buffer.GetData()+Offset, C.Payload.GetData(), C.Payload.Num()) == 0;
        FMemory::Memcpy(Buffer.GetData()+Offset, C.Payload.GetData(), C.Payload.Num());
        Received[C.Index] = true; ++Count;
    }
    RecoverGroup(Group);
    bComplete = Count == Received.Num();
    return true;
}
void FMRFPSVideoAssembler::RecoverGroup(int32 Group)
{
    if (!ReceivedParity[Group]) return;
    const int32 First = Group * MRFPSVideo::DataChunksPerParity;
    const int32 End = FMath::Min(First + MRFPSVideo::DataChunksPerParity, Received.Num());
    int32 Missing = -1;
    for (int32 I = First; I < End; ++I)
    {
        if (Received[I]) continue;
        if (Missing != -1) return; // XOR can never resolve two unknown members.
        Missing = I;
    }
    if (Missing == -1) return;
    uint8 Recovered[MRFPSVideo::ChunkBytes];
    FMemory::Memcpy(Recovered, ParityRows.GetData() + Group * MRFPSVideo::ChunkBytes, sizeof(Recovered));
    for (int32 I = First; I < End; ++I)
    {
        if (I == Missing) continue;
        const int32 Offset = I * MRFPSVideo::ChunkBytes;
        const int32 Length = FMath::Min(MRFPSVideo::ChunkBytes, Buffer.Num() - Offset);
        for (int32 Byte = 0; Byte < Length; ++Byte) Recovered[Byte] ^= Buffer[Offset + Byte];
        // The remainder of a short final chunk is implicitly zero padded.
    }
    const int32 Offset = Missing * MRFPSVideo::ChunkBytes;
    const int32 Length = FMath::Min(MRFPSVideo::ChunkBytes, Buffer.Num() - Offset);
    FMemory::Memcpy(Buffer.GetData() + Offset, Recovered, Length);
    Received[Missing] = true; ++Count;
    ++RecoveredChunks;
}
bool FMRFPSVideoAssembler::Take(FMRFPSVideoChunk& Header, TArray<uint8>& Jpeg)
{
    if (!bComplete) return false;
    ++CompletedFrames;
    Header = Meta; Header.Index = 0; Header.Payload.Reset();
    Jpeg = MoveTemp(Buffer); CompletedSequence = Meta.Sequence; DiscardPartial();
    return true;
}

void FMRFPSVideoSendQueue::Reset()
{
    Current.Reset(); Latest.Reset(); Cursor = 0; PendingParityGroup = -1;
}
void FMRFPSVideoSendQueue::Push(const FMRFPSVideoChunk& Header, const TArray<uint8>& Jpeg, double Now)
{
    if (!SaneHeader(Header) || !FMath::IsFinite(Now) || Jpeg.Num() != Header.TotalBytes) return;
    ++PushedFrames;
    if (Current.IsEmpty())
    {
        // A new frame supersedes a waiting frame left between drain/promotion.
        if (!Latest.IsEmpty()) ++ReplacedFrames;
        Latest.Reset();
        CurrentHeader = Header; Current = Jpeg; Cursor = 0; PendingParityGroup = -1; CurrentAt = Now;
    }
    else { if (!Latest.IsEmpty()) ++ReplacedFrames; LatestHeader = Header; Latest = Jpeg; LatestAt = Now; }
}
bool FMRFPSVideoSendQueue::Next(FMRFPSVideoChunk& Chunk, double Now)
{
    if (!FMath::IsFinite(Now)) return false;
    if (!Current.IsEmpty() && Now - CurrentAt > MRFPSVideo::FrameLifetime)
    { ++ExpiredFrames; Current.Reset(); Cursor = 0; PendingParityGroup = -1; }
    if (Current.IsEmpty())
    {
        if (Latest.IsEmpty()) return false;
        Current = MoveTemp(Latest); CurrentHeader = LatestHeader; CurrentAt = LatestAt; Cursor = 0; PendingParityGroup = -1;
        if (Now - CurrentAt > MRFPSVideo::FrameLifetime) { ++ExpiredFrames; Current.Reset(); return false; }
    }
    Chunk = CurrentHeader;
    ++SentChunks;
    if (PendingParityGroup >= 0)
    {
        Chunk.Index = static_cast<uint8>(MRFPSVideo::ParityFlag | PendingParityGroup);
        Chunk.Payload.SetNumUninitialized(MRFPSVideo::ChunkBytes);
        FMemory::Memzero(Chunk.Payload.GetData(), MRFPSVideo::ChunkBytes);
        const int32 First = PendingParityGroup * MRFPSVideo::DataChunksPerParity;
        const int32 End = FMath::Min(First + MRFPSVideo::DataChunksPerParity, DataChunkCount(Current.Num()));
        for (int32 I = First; I < End; ++I)
        {
            const int32 Offset = I * MRFPSVideo::ChunkBytes;
            const int32 Length = FMath::Min(MRFPSVideo::ChunkBytes, Current.Num() - Offset);
            for (int32 Byte = 0; Byte < Length; ++Byte) Chunk.Payload[Byte] ^= Current[Offset + Byte];
        }
        PendingParityGroup = -1;
        // Keep the source bytes until the final group's parity has been emitted.
        if (Cursor == Current.Num()) { ++FinishedFrames; Current.Reset(); Cursor = 0; }
        return true;
    }
    const int32 DataIndex = Cursor / MRFPSVideo::ChunkBytes;
    Chunk.Index = static_cast<uint8>(DataIndex);
    const int32 Length = FMath::Min(MRFPSVideo::ChunkBytes, Current.Num() - Cursor);
    Chunk.Payload.Reset(Length); Chunk.Payload.Append(Current.GetData()+Cursor, Length);
    Cursor += Length;
    if ((DataIndex + 1) % MRFPSVideo::DataChunksPerParity == 0 || Cursor == Current.Num())
        PendingParityGroup = DataIndex / MRFPSVideo::DataChunksPerParity;
    return true;
}
