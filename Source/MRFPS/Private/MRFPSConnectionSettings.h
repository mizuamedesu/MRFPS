#pragma once

#include "CoreMinimal.h"
#include "Misc/ConfigCacheIni.h"

namespace MRFPSConnectionSettings
{
// Both login and the in-game menu show the same saved endpoint, or this
// default when no endpoint has been saved yet.
inline FString InitialServerAddress(FConfigCacheIni* Config, const FString& IniFilename)
{
    FString Address=TEXT("mrfps.mizuame.app:7777");
    if (Config) Config->GetString(TEXT("MRFPS.Connection"),TEXT("Server"),Address,IniFilename);
    return Address.Left(255);
}

inline FString InitialServerAddress()
{
    return InitialServerAddress(GConfig,GGameUserSettingsIni);
}
}
