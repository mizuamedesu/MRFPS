#include "MRFPSAuthLogPrivacy.h"

#include "Engine/Engine.h"
#include "EngineLogs.h"
#include "Misc/OutputDeviceNull.h"
#if IS_MONOLITHIC || UE_MERGED_MODULES
#include "PacketHandlers/StatelessConnectHandlerComponent.h"
#endif

void FMRFPSAuthLogPrivacy::SuppressNetworkTokenLogs()
{
    LogNet.SetVerbosity(ELogVerbosity::NoLogging);
    LogNetTraffic.SetVerbosity(ELogVerbosity::NoLogging);
#if IS_MONOLITHIC || UE_MERGED_MODULES
    LogHandshake.SetVerbosity(ELogVerbosity::NoLogging);
#elif UE_ALLOW_EXEC_COMMANDS
    if (GEngine)
    {
        // LogHandshake is not DLL-exported. The engine's public Exec route can
        // address its registered category. "off" means Fatal in UE5.8, then an
        // unqualified command toggles that known nonzero level to NoLogging.
        FOutputDeviceNull Sink;
        GEngine->Exec(nullptr, TEXT("Log LogHandshake off"), Sink);
        GEngine->Exec(nullptr, TEXT("Log LogHandshake"), Sink);
    }
#endif
}
