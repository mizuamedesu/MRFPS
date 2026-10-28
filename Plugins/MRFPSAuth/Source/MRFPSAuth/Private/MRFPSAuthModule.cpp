#include "Modules/ModuleManager.h"
#include "MRFPSAuthLogPrivacy.h"
#include "Misc/CoreDelegates.h"

class FMRFPSAuthModule final : public IModuleInterface
{
    FDelegateHandle ReadyHandle;
    virtual void StartupModule() override
    {
        FMRFPSAuthLogPrivacy::SuppressNetworkTokenLogs();
        ReadyHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddStatic(&FMRFPSAuthLogPrivacy::SuppressNetworkTokenLogs);
    }
    virtual void ShutdownModule() override
    {
        FCoreDelegates::OnFEngineLoopInitComplete.Remove(ReadyHandle);
    }
};

IMPLEMENT_MODULE(FMRFPSAuthModule, MRFPSAuth)
