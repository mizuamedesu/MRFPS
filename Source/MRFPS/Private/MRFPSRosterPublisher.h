#pragma once

#include "CoreMinimal.h"

class IHttpRequest;
class UWorld;

// A single authenticated HTTPS writer, available only to the dedicated server.
class FMRFPSRosterPublisher
{
public:
    ~FMRFPSRosterPublisher();
    bool Start();
    void Tick(UWorld* World, double Now);
    static FString HmacSha256(const FString& Key, const FString& Body);
private:
    FString Secret;
    FString Instance;
    uint64 Sequence = 0;
    double NextPublishAt = 0;
    TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Pending;
};
