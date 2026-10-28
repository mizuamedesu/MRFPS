#pragma once

#include "CoreMinimal.h"

/** Prevent native network categories from printing unredacted login options. */
struct MRFPSAUTH_API FMRFPSAuthLogPrivacy
{
    static void SuppressNetworkTokenLogs();
};
