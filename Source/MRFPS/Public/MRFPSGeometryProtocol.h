#pragma once

#include "CoreMinimal.h"
#include "MRFPSGeometryProtocol.generated.h"

namespace MRFPSGeometryWire
{
    constexpr int32 MaxVertices=2048;
    constexpr int32 MaxTriangles=4096;
    constexpr int32 MaxBytes=6*(MaxVertices+MaxTriangles);
    constexpr int32 ChunkBytes=512;
    constexpr int32 ParityGroupSize=8;
    constexpr uint8 ParityFlag=0x80;
    constexpr double PartialLifetime=.75;
    constexpr double SnapshotLifetime=1.5;
    constexpr double MaxCaptureAge=.75;
    constexpr double MaxTriangleEdge=150;
    bool MRFPS_API Inside(const FVector& Point);
    bool MRFPS_API CaptureFresh(uint32 CaptureMs,double ServerSeconds,double MaxAge=MaxCaptureAge);
    bool MRFPS_API AdvanceCalibrationFence(uint32& Fence,uint32 LastServerGeneration);
    bool MRFPS_API CalibrationAcknowledged(uint32 Fence,uint32 ServerGeneration);
}

/** Bounded geometry fragment. Vertices are signed 1 cm arena XYZ, indices uint16 LE.
 * Upload StreamId equals the acknowledged pawn calibration generation; relay
 * StreamId is assigned by the server. Calibration must match the publisher pawn.
 */
USTRUCT()
struct FMRFPSGeometryChunk
{
    GENERATED_BODY()
    uint8 Slot=0;
    uint32 StreamId=0;
    uint32 Calibration=0;
    uint32 LayoutRevision=1;
    uint32 Sequence=0;
    uint32 CaptureMs=0;
    uint16 VertexCount=0;
    uint16 TriangleCount=0;
    uint16 TotalBytes=0;
    uint8 Index=0;
    TArray<uint8> Payload;
    bool IsSane() const;
    bool NetSerialize(FArchive& Ar,UPackageMap* Map,bool& bSuccess);
};
template<> struct TStructOpsTypeTraits<FMRFPSGeometryChunk> : TStructOpsTypeTraitsBase2<FMRFPSGeometryChunk>
{ enum { WithNetSerializer=true,WithCopy=true }; };

struct MRFPS_API FMRFPSGeometryMesh
{
    TArray<FVector> Vertices;
    TArray<int32> Indices;
    bool IsSane() const;
    bool Encode(FMRFPSGeometryChunk& Header,TArray<uint8>& Bytes) const;
    static bool Decode(const FMRFPSGeometryChunk& Header,TConstArrayView<uint8> Bytes,FMRFPSGeometryMesh& Out);
    /** Two-sided closest surface hit on the finite segment; no Chaos allocation. */
    bool Trace(const FVector& Start,const FVector& End,double& Fraction,FVector& Point) const;
};

class MRFPS_API FMRFPSGeometryAssembler
{
public:
    bool Add(const FMRFPSGeometryChunk& Chunk,double Now);
    bool Take(FMRFPSGeometryChunk& Header,TArray<uint8>& Bytes);
    void Reset();
    int32 AllocatedBytes() const { return int32(Data.GetAllocatedSize()+Parity.GetAllocatedSize()+Received.GetAllocatedSize()+ReceivedParity.GetAllocatedSize()); }
private:
    FMRFPSGeometryChunk Meta;
    TArray<uint8> Data,Parity;
    TBitArray<> Received,ReceivedParity;
    uint32 SeenStream=0,ClosedSequence=0;
    int32 Count=0;
    double StartedAt=0;
    bool bComplete=false;
    void Discard();
    void Recover(int32 Group);
};

/** One current plus one latest snapshot; no unbounded network backlog. */
class MRFPS_API FMRFPSGeometrySendQueue
{
public:
    void Push(const FMRFPSGeometryChunk& Header,const TArray<uint8>& Bytes,double Now);
    bool Next(FMRFPSGeometryChunk& Out,double Now);
    void Reset();
private:
    FMRFPSGeometryChunk CurrentMeta,LatestMeta;
    TArray<uint8> Current,Latest;
    double CurrentAt=0,LatestAt=0;
    int32 Cursor=0,PendingParity=-1;
};
