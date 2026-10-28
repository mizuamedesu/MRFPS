#pragma once

#include "GeometryCaptureSubsystem.h"

namespace MRFPSGeometryRuntime
{
    // Called by the existing PostConfigInit module, before OpenXR creates its instance.
    void Startup();
    void Shutdown();
    bool CanRequest();
    void SetRequested(bool bRequested);
    bool IsFresh();
    uint64 TrackingSpaceEpoch();
    FString Status();
    bool Dequeue(FMRFPSEnvironmentGeometryFrame& OutFrame);
}
