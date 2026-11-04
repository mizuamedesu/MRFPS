#pragma once

#include "CoreMinimal.h"
#include <atomic>

struct FMRFPSMoBitsIdentifyRequest
{
    std::atomic<bool> Cancelled{false};
};

namespace MRFPSMoBitsIdentify
{
using FRequest = TSharedRef<FMRFPSMoBitsIdentifyRequest, ESPMode::ThreadSafe>;
// WaitingForDevice is true only if no LRA command was attempted.
void Start(const FString& DeviceId, FRequest Request, TFunction<void(bool, int32, bool)> Completion);
}
