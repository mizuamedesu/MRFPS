#pragma once
#include "Commandlets/Commandlet.h"
#include "MRFPSSetupCommandlet.generated.h"

UCLASS()
class UMRFPSSetupCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UMRFPSSetupCommandlet();
    virtual int32 Main(const FString& Params) override;
};
