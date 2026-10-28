#include "MRFPSGeometryProtocol.h"
#include "MRFPSArenaLayout.h"

namespace
{
int32 Chunks(int32 Size) { return FMath::DivideAndRoundUp(Size,MRFPSGeometryWire::ChunkBytes); }
int32 Groups(int32 Size) { return FMath::DivideAndRoundUp(Chunks(Size),MRFPSGeometryWire::ParityGroupSize); }
bool IsParity(const FMRFPSGeometryChunk& C) { return (C.Index&MRFPSGeometryWire::ParityFlag)!=0; }
int32 GroupOf(const FMRFPSGeometryChunk& C) { return C.Index&0x7f; }
bool HeaderSane(const FMRFPSGeometryChunk& C)
{
    if (C.Slot>=2 || !C.StreamId || !C.Calibration || !C.LayoutRevision || !C.Sequence || C.VertexCount<3 ||
        C.VertexCount>MRFPSGeometryWire::MaxVertices || !C.TriangleCount || C.TriangleCount>MRFPSGeometryWire::MaxTriangles ||
        C.TotalBytes!=6*(int32(C.VertexCount)+int32(C.TriangleCount))) return false;
    return IsParity(C) ? GroupOf(C)<Groups(C.TotalBytes) : int32(C.Index)<Chunks(C.TotalBytes);
}
int32 PayloadSize(const FMRFPSGeometryChunk& C)
{
    return IsParity(C)?MRFPSGeometryWire::ChunkBytes:FMath::Min(MRFPSGeometryWire::ChunkBytes,int32(C.TotalBytes)-int32(C.Index)*MRFPSGeometryWire::ChunkBytes);
}
bool SameFrame(const FMRFPSGeometryChunk& A,const FMRFPSGeometryChunk& B)
{
    return A.Slot==B.Slot && A.StreamId==B.StreamId && A.Calibration==B.Calibration && A.LayoutRevision==B.LayoutRevision &&
        A.Sequence==B.Sequence && A.CaptureMs==B.CaptureMs && A.VertexCount==B.VertexCount &&
        A.TriangleCount==B.TriangleCount && A.TotalBytes==B.TotalBytes;
}
uint16 Read16(TConstArrayView<uint8> Bytes,int32 At) { return uint16(Bytes[At])|(uint16(Bytes[At+1])<<8); }
void Write16(TArray<uint8>& Bytes,uint16 Value) { Bytes.Add(uint8(Value)); Bytes.Add(uint8(Value>>8)); }
}

bool MRFPSGeometryWire::Inside(const FVector& P)
{
    // Wire safety envelope only. Every publication is additionally checked
    // against the current rotated/configured arena and its layout revision.
    return !P.ContainsNaN() && FMath::Abs(P.X)<=MRFPSArenaLimits::MaxCoordinateCm &&
        FMath::Abs(P.Y)<=MRFPSArenaLimits::MaxCoordinateCm && P.Z>=-1000 && P.Z<=4000;
}
bool MRFPSGeometryWire::CaptureFresh(uint32 CaptureMs,double ServerSeconds,double MaxAge)
{
    if (!FMath::IsFinite(ServerSeconds) || ServerSeconds<0 || ServerSeconds>1.e9 || !FMath::IsFinite(MaxAge) || MaxAge<0) return false;
    const uint32 NowMs=static_cast<uint32>(static_cast<uint64>(ServerSeconds*1000));
    const int32 Age=static_cast<int32>(NowMs-CaptureMs);
    // Server-world time estimate may lead by at most 250 ms. No wall-clock or
    // unconverted client monotonic timestamp is compared across machines.
    return Age>=-250 && Age<=MaxAge*1000;
}
bool MRFPSGeometryWire::AdvanceCalibrationFence(uint32& Fence,uint32 LastServerGeneration)
{
    const uint32 Latest=FMath::Max(Fence,LastServerGeneration);
    if (Latest>=MAX_uint32-1) return false;
    Fence=Latest+1; return true;
}
bool MRFPSGeometryWire::CalibrationAcknowledged(uint32 Fence,uint32 ServerGeneration)
{ return Fence>0 && ServerGeneration>=Fence; }
bool FMRFPSGeometryChunk::IsSane() const { return HeaderSane(*this) && Payload.Num()==PayloadSize(*this); }
bool FMRFPSGeometryChunk::NetSerialize(FArchive& Ar,UPackageMap*,bool& bSuccess)
{
    bSuccess=false;
    if (Ar.IsSaving() && !IsSane()) { Ar.SetError(); return false; }
    Ar<<Slot<<StreamId<<Calibration<<LayoutRevision<<Sequence<<CaptureMs<<VertexCount<<TriangleCount<<TotalBytes<<Index;
    uint16 Length=static_cast<uint16>(Payload.Num()); Ar<<Length;
    if (Ar.IsError() || !HeaderSane(*this) || Length>MRFPSGeometryWire::ChunkBytes || Length!=PayloadSize(*this)) {
        Ar.SetError(); return false;
    }
    if (Ar.IsLoading()) Payload.SetNumUninitialized(Length);
    Ar.Serialize(Payload.GetData(),Length); bSuccess=!Ar.IsError(); return bSuccess;
}
bool FMRFPSGeometryMesh::IsSane() const
{
    if (Vertices.Num()<3 || Vertices.Num()>MRFPSGeometryWire::MaxVertices || Indices.Num()<3 ||
        Indices.Num()>MRFPSGeometryWire::MaxTriangles*3 || Indices.Num()%3) return false;
    for (const FVector& P:Vertices) if (!MRFPSGeometryWire::Inside(P)) return false;
    for (int32 I=0;I<Indices.Num();I+=3) {
        const int32 A=Indices[I],B=Indices[I+1],C=Indices[I+2];
        if (!Vertices.IsValidIndex(A) || !Vertices.IsValidIndex(B) || !Vertices.IsValidIndex(C) || A==B || A==C || B==C) return false;
        const FVector AB=Vertices[B]-Vertices[A],AC=Vertices[C]-Vertices[A],BC=Vertices[C]-Vertices[B];
        if (AB.SizeSquared()>FMath::Square(MRFPSGeometryWire::MaxTriangleEdge) || AC.SizeSquared()>FMath::Square(MRFPSGeometryWire::MaxTriangleEdge) ||
            BC.SizeSquared()>FMath::Square(MRFPSGeometryWire::MaxTriangleEdge) || FVector::CrossProduct(AB,AC).SizeSquared()<1.e-4) return false;
    }
    return true;
}
bool FMRFPSGeometryMesh::Encode(FMRFPSGeometryChunk& Header,TArray<uint8>& Bytes) const
{
    Bytes.Reset(); if (!IsSane()) return false;
    FMRFPSGeometryMesh Quantized; Quantized.Indices=Indices; Quantized.Vertices.Reserve(Vertices.Num());
    for (const FVector& P:Vertices) Quantized.Vertices.Emplace(FMath::RoundToInt(P.X),FMath::RoundToInt(P.Y),FMath::RoundToInt(P.Z));
    if (!Quantized.IsSane()) return false;
    Header.VertexCount=uint16(Vertices.Num()); Header.TriangleCount=uint16(Indices.Num()/3);
    Header.TotalBytes=uint16(6*(Header.VertexCount+Header.TriangleCount)); Bytes.Reserve(Header.TotalBytes);
    for (const FVector& P:Quantized.Vertices) { Write16(Bytes,uint16(int16(P.X))); Write16(Bytes,uint16(int16(P.Y))); Write16(Bytes,uint16(int16(P.Z))); }
    for (int32 Index:Indices) Write16(Bytes,uint16(Index));
    return true;
}
bool FMRFPSGeometryMesh::Decode(const FMRFPSGeometryChunk& Header,TConstArrayView<uint8> Bytes,FMRFPSGeometryMesh& Out)
{
    Out=FMRFPSGeometryMesh();
    if (!HeaderSane(Header) || Bytes.Num()!=Header.TotalBytes) return false;
    FMRFPSGeometryMesh Result; Result.Vertices.Reserve(Header.VertexCount); Result.Indices.Reserve(Header.TriangleCount*3);
    int32 At=0;
    for (int32 I=0;I<Header.VertexCount;++I,At+=6) Result.Vertices.Emplace(int16(Read16(Bytes,At)),int16(Read16(Bytes,At+2)),int16(Read16(Bytes,At+4)));
    for (;At<Bytes.Num();At+=2) Result.Indices.Add(Read16(Bytes,At));
    if (!Result.IsSane()) return false;
    Out=MoveTemp(Result); return true;
}
bool FMRFPSGeometryMesh::Trace(const FVector& Start,const FVector& End,double& Fraction,FVector& Point) const
{
    if (Start.ContainsNaN() || End.ContainsNaN() || Vertices.Num()<3 || Vertices.Num()>MRFPSGeometryWire::MaxVertices ||
        Indices.Num()<3 || Indices.Num()>MRFPSGeometryWire::MaxTriangles*3 || Indices.Num()%3) return false;
    const double LengthSquared=FVector::DistSquared(Start,End);
    if (!FMath::IsFinite(LengthSquared) || LengthSquared<1.e-8) return false;
    for (const FVector& P:Vertices) if (P.ContainsNaN()) return false;
    bool Found=false; double Best=1;
    for (int32 I=0;I<Indices.Num();I+=3) {
        if (!Vertices.IsValidIndex(Indices[I]) || !Vertices.IsValidIndex(Indices[I+1]) || !Vertices.IsValidIndex(Indices[I+2])) return false;
        const FVector& A=Vertices[Indices[I]]; const FVector& B=Vertices[Indices[I+1]]; const FVector& C=Vertices[Indices[I+2]];
        FVector Hit,Normal;
        if (FMath::SegmentTriangleIntersection(Start,End,A,B,C,Hit,Normal)) {
            const double T=FVector::DotProduct(Hit-Start,End-Start)/LengthSquared;
            if (T>=0 && T<=Best) { Best=T; Point=Hit; Found=true; }
        }
    }
    if (Found) Fraction=Best;
    return Found;
}

void FMRFPSGeometryAssembler::Discard()
{ Data.Reset(); Parity.Reset(); Received.Reset(); ReceivedParity.Reset(); Count=0; bComplete=false; }
void FMRFPSGeometryAssembler::Reset()
{ Discard(); Meta=FMRFPSGeometryChunk(); SeenStream=ClosedSequence=0; }
bool FMRFPSGeometryAssembler::Add(const FMRFPSGeometryChunk& C,double Now)
{
    if (!C.IsSane() || !FMath::IsFinite(Now) || C.StreamId<SeenStream) return false;
    if (C.StreamId>SeenStream) { Reset(); SeenStream=C.StreamId; }
    if (!Data.IsEmpty() && (Now<StartedAt || Now-StartedAt>MRFPSGeometryWire::PartialLifetime)) {
        ClosedSequence=FMath::Max(ClosedSequence,Meta.Sequence); Discard();
    }
    if (C.Sequence<=ClosedSequence || (!Data.IsEmpty() && C.Sequence<Meta.Sequence)) return false;
    if (Data.IsEmpty() || C.Sequence>Meta.Sequence) {
        Discard(); Meta=C; Meta.Payload.Reset(); StartedAt=Now;
        // Reserve exact bounded sizes rather than accumulating geometric growth
        // slack when switching repeatedly between small and large snapshots.
        Data.Empty(C.TotalBytes); Data.SetNumUninitialized(C.TotalBytes); Received.Init(false,Chunks(C.TotalBytes));
        ReceivedParity.Init(false,Groups(C.TotalBytes)); const int32 ParityBytes=ReceivedParity.Num()*MRFPSGeometryWire::ChunkBytes;
        Parity.Empty(ParityBytes); Parity.SetNumUninitialized(ParityBytes);
    }
    if (!SameFrame(C,Meta)) return false;
    const int32 Group=IsParity(C)?GroupOf(C):int32(C.Index)/MRFPSGeometryWire::ParityGroupSize;
    if (IsParity(C)) {
        uint8* Row=Parity.GetData()+Group*MRFPSGeometryWire::ChunkBytes;
        if (ReceivedParity[Group]) return FMemory::Memcmp(Row,C.Payload.GetData(),C.Payload.Num())==0;
        FMemory::Memcpy(Row,C.Payload.GetData(),C.Payload.Num()); ReceivedParity[Group]=true;
    } else {
        const int32 Offset=int32(C.Index)*MRFPSGeometryWire::ChunkBytes;
        if (Received[C.Index]) return FMemory::Memcmp(Data.GetData()+Offset,C.Payload.GetData(),C.Payload.Num())==0;
        FMemory::Memcpy(Data.GetData()+Offset,C.Payload.GetData(),C.Payload.Num()); Received[C.Index]=true; ++Count;
    }
    Recover(Group); bComplete=Count==Received.Num(); return true;
}
void FMRFPSGeometryAssembler::Recover(int32 Group)
{
    if (!ReceivedParity[Group]) return;
    const int32 First=Group*MRFPSGeometryWire::ParityGroupSize,End=FMath::Min(First+MRFPSGeometryWire::ParityGroupSize,Received.Num());
    int32 Missing=-1;
    for (int32 I=First;I<End;++I) if (!Received[I]) { if (Missing!=-1) return; Missing=I; }
    if (Missing<0) return;
    uint8 Row[MRFPSGeometryWire::ChunkBytes]; FMemory::Memcpy(Row,Parity.GetData()+Group*MRFPSGeometryWire::ChunkBytes,sizeof(Row));
    for (int32 I=First;I<End;++I) if (I!=Missing) {
        const int32 Offset=I*MRFPSGeometryWire::ChunkBytes,Size=FMath::Min(MRFPSGeometryWire::ChunkBytes,Data.Num()-Offset);
        for (int32 B=0;B<Size;++B) Row[B]^=Data[Offset+B];
    }
    const int32 Offset=Missing*MRFPSGeometryWire::ChunkBytes,Size=FMath::Min(MRFPSGeometryWire::ChunkBytes,Data.Num()-Offset);
    FMemory::Memcpy(Data.GetData()+Offset,Row,Size); Received[Missing]=true; ++Count;
}
bool FMRFPSGeometryAssembler::Take(FMRFPSGeometryChunk& Header,TArray<uint8>& Bytes)
{
    if (!bComplete) return false;
    Header=Meta; Bytes=MoveTemp(Data); ClosedSequence=Meta.Sequence; Discard(); return true;
}
void FMRFPSGeometrySendQueue::Reset()
{ Current.Reset(); Latest.Reset(); Cursor=0; PendingParity=-1; }
void FMRFPSGeometrySendQueue::Push(const FMRFPSGeometryChunk& Header,const TArray<uint8>& Bytes,double Now)
{
    if (Bytes.Num()!=Header.TotalBytes || !HeaderSane(Header) || !FMath::IsFinite(Now)) return;
    LatestMeta=Header; LatestMeta.Payload.Reset(); Latest=Bytes; LatestAt=Now;
}
bool FMRFPSGeometrySendQueue::Next(FMRFPSGeometryChunk& Out,double Now)
{
    if (!FMath::IsFinite(Now)) return false;
    if (!Current.IsEmpty() && (Now<CurrentAt || Now-CurrentAt>MRFPSGeometryWire::PartialLifetime)) { Current.Reset(); PendingParity=-1; }
    if (Current.IsEmpty()) {
        if (Latest.IsEmpty()) return false;
        if (Now<LatestAt || Now-LatestAt>MRFPSGeometryWire::PartialLifetime) { Latest.Reset(); return false; }
        Current=MoveTemp(Latest); CurrentMeta=LatestMeta; CurrentAt=LatestAt; Cursor=0; PendingParity=-1;
    }
    Out=CurrentMeta;
    if (PendingParity>=0) {
        Out.Index=MRFPSGeometryWire::ParityFlag|uint8(PendingParity); Out.Payload.SetNumZeroed(MRFPSGeometryWire::ChunkBytes);
        const int32 First=PendingParity*MRFPSGeometryWire::ParityGroupSize,End=FMath::Min(First+MRFPSGeometryWire::ParityGroupSize,Chunks(Current.Num()));
        for (int32 I=First;I<End;++I) {
            const int32 Offset=I*MRFPSGeometryWire::ChunkBytes,Size=FMath::Min(MRFPSGeometryWire::ChunkBytes,Current.Num()-Offset);
            for (int32 B=0;B<Size;++B) Out.Payload[B]^=Current[Offset+B];
        }
        PendingParity=-1; if (Cursor==Chunks(Current.Num())) Current.Reset();
    } else {
        Out.Index=uint8(Cursor); const int32 Offset=Cursor*MRFPSGeometryWire::ChunkBytes;
        Out.Payload.Reset(); Out.Payload.Append(Current.GetData()+Offset,FMath::Min(MRFPSGeometryWire::ChunkBytes,Current.Num()-Offset));
        ++Cursor; if (Cursor%MRFPSGeometryWire::ParityGroupSize==0 || Cursor==Chunks(Current.Num())) PendingParity=(Cursor-1)/MRFPSGeometryWire::ParityGroupSize;
    }
    return Out.IsSane();
}
