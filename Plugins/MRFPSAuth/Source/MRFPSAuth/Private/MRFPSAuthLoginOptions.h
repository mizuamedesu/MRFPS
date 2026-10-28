#pragma once

#include "CoreMinimal.h"

class UGameInstance;
class UMRFPSAuthClientSubsystem;

namespace MRFPSAuthLogin
{
    // Shared by the real LocalPlayer injector and its negative security test.
    FString BuildForCurrentPendingConnection(const UGameInstance* Instance, const UMRFPSAuthClientSubsystem* Credentials);
}
