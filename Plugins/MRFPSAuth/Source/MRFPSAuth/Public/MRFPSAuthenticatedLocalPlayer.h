#pragma once

#include "CoreMinimal.h"
#include "Engine/LocalPlayer.h"
#include "MRFPSAuthenticatedLocalPlayer.generated.h"

/** Injects verified credentials in NMT_Login, never in ClientTravel URLs. */
UCLASS()
class MRFPSAUTH_API UMRFPSAuthenticatedLocalPlayer : public ULocalPlayer
{
    GENERATED_BODY()
public:
    virtual FString GetGameLoginOptions() const override;
};
