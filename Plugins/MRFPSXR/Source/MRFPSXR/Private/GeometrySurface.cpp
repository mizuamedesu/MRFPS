#include "GeometrySurface.h"
#include "GeometryDepthMath.h"

namespace MRFPSGeometrySurface
{
bool ProjectionMatches(TConstArrayView<FVector4f> Points, const MRFPSGeometryMath::FView& View, uint32 SourceWidth, uint32 SourceHeight)
{
    using namespace MRFPSGeometryMath;
    if (Points.Num() != int32(PointCount) || !ValidView(View) || SourceWidth == 0 || SourceHeight == 0 ||
        SourceWidth > 4096 || SourceHeight > 4096) return false;
    for (int32 I=0; I<Points.Num(); ++I)
    {
        const auto& Actual = Points[I];
        if (Actual.W == 0) continue; // Shader's explicit invalid-depth marker.
        FPoint Expected;
        const uint32 PixelX = GridPixel(I%GridWidth, GridWidth, SourceWidth);
        const uint32 PixelY = GridPixel(I/GridWidth, GridHeight, SourceHeight);
        if (!ValidPoint({Actual.X, Actual.Y, Actual.Z, Actual.W}) ||
            !UnprojectForwardDepth(View, Actual.W, PixelX, PixelY, SourceWidth, SourceHeight, Expected) ||
            FMath::Abs(Actual.X-Expected.X) > 0.5f || FMath::Abs(Actual.Y-Expected.Y) > 0.5f ||
            FMath::Abs(Actual.Z-Expected.Z) > 0.5f) return false;
    }
    return true;
}

bool Build(TConstArrayView<FVector4f> Points, FMRFPSEnvironmentGeometryFrame& Frame)
{
    using namespace MRFPSGeometryMath;
    static_assert(PointCount <= MRFPSGeometry::MaxVertices && MaximumIndices <= MRFPSGeometry::MaxIndices);
    static_assert(GridWidth == MRFPSGeometry::GridWidth && GridHeight == MRFPSGeometry::GridHeight);
    Frame.Vertices.Reset(); Frame.Indices.Reset();
    if (Points.Num() != int32(PointCount)) { Frame = {}; return false; }
    TArray<FPoint> Samples; Samples.SetNumUninitialized(PointCount);
    for (int32 I=0; I<Points.Num(); ++I) Samples[I] = {Points[I].X, Points[I].Y, Points[I].Z, Points[I].W};
    TArray<uint16> GridIndices; GridIndices.SetNumUninitialized(MaximumIndices);
    size_t Written = 0;
    if (!BuildTriangles(Samples.GetData(), Samples.Num(), GridIndices.GetData(), GridIndices.Num(), Written))
    { Frame = {}; return false; }
    TArray<int32> Mapping; Mapping.Init(INDEX_NONE, PointCount);
    Frame.Vertices.Reserve(PointCount); Frame.Indices.Reserve(static_cast<int32>(Written));
    for (size_t I=0; I<Written; ++I)
    {
        const uint16 Source = GridIndices[static_cast<int32>(I)];
        if (Mapping[Source] == INDEX_NONE)
            Mapping[Source] = Frame.Vertices.Add(FVector3f(Samples[Source].X, Samples[Source].Y, Samples[Source].Z));
        Frame.Indices.Add(static_cast<uint32>(Mapping[Source]));
    }
    return true;
}

bool Take(TOptional<FMRFPSEnvironmentGeometryFrame>& Latest, uint64 Epoch, double Now, FMRFPSEnvironmentGeometryFrame& Out)
{
    Out = {};
    if (!Latest.IsSet()) return false;
    const auto& Frame = Latest.GetValue();
    const bool Valid = Epoch != 0 && Frame.TrackingSpaceEpoch == Epoch && Frame.bTrackingValid && Frame.Sequence != 0 &&
        MRFPSGeometryMath::IsFresh(Frame.CaptureMonotonicTime, Now) && !Frame.Vertices.IsEmpty() &&
        Frame.Vertices.Num() <= MRFPSGeometry::MaxVertices && !Frame.Indices.IsEmpty() &&
        Frame.Indices.Num() <= MRFPSGeometry::MaxIndices && Frame.Indices.Num()%3 == 0;
    if (Valid) Out = MoveTemp(Latest.GetValue());
    Latest.Reset();
    return Valid;
}
}
