#pragma once

#include "GeometryCaptureSubsystem.h"
#include "GeometryDepthMath.h"

namespace MRFPSGeometrySurface
{
    // Retains acquisition metadata on success; any failure empties the entire frame.
    bool Build(TConstArrayView<FVector4f> Points, FMRFPSEnvironmentGeometryFrame& Frame);
    // Checks GPU positions against the returned camera pose/frustum and the
    // sample's actual forward depth. Rejects disagreement; never replaces data.
    bool ProjectionMatches(TConstArrayView<FVector4f> Points, const MRFPSGeometryMath::FView& View,
        uint32 SourceWidth, uint32 SourceHeight);
    // A single-consumer freshness/epoch gate. No stale or failed output survives.
    bool Take(TOptional<FMRFPSEnvironmentGeometryFrame>& Latest, uint64 Epoch, double Now,
        FMRFPSEnvironmentGeometryFrame& Out);
}
