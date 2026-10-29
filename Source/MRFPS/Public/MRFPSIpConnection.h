#pragma once

#include "CoreMinimal.h"
#include "IpConnection.h"
#include "MRFPSConsentPolicy.h"
#include "MRFPSIpConnection.generated.h"

/** A bounded consent pause on an already authenticated connection, never a global timeout override. */
UCLASS(Transient,Config=Engine)
class MRFPS_API UMRFPSIpConnection : public UIpConnection
{
    GENERATED_BODY()
public:
    virtual float GetTimeoutValue() override;
    double BeginCaptureConsentPause(double Seconds);
    double BeginCaptureConsentPauseUntil(double Deadline);
    void EndCaptureConsentPause();
private:
    MRFPSConsent::FPauseLease CaptureConsentLease;
};
