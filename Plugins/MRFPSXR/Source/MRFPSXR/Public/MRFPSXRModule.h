#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FMRFPSPassthroughExtension;

class MRFPSXR_API FMRFPSXRModule : public IModuleInterface
{
public:
    FMRFPSXRModule();
    virtual ~FMRFPSXRModule() override;
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    // True only after the native runtime starts the feature and resumes its layer.
    static bool IsPassthroughRunning();
private:
    TUniquePtr<FMRFPSPassthroughExtension> Extension;
};
