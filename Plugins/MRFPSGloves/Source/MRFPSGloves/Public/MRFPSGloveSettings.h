#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MRFPSGloveSettings.generated.h"

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="MRFPS ContactGlove"))
class MRFPSGLOVES_API UMRFPSGloveSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    UPROPERTY(Config, EditAnywhere, Category="Connection") bool bEnabled = true;
    UPROPERTY(Config, EditAnywhere, Category="Connection") FString BindAddress = TEXT("0.0.0.0");
    UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(ClampMin="1024", ClampMax="65535")) int32 ListenPort = 25788;
    // Pin this to the PC bridge's LAN IPv4 address on Quest. Empty never means 'trust all'.
    UPROPERTY(Config, EditAnywhere, Category="Connection") FString AllowedSender = TEXT("127.0.0.1");
    UPROPERTY(Config, EditAnywhere, Category="Connection") FString DeviceId;
    UPROPERTY(Config, EditAnywhere, Category="Connection") FString StationAddress = TEXT("127.0.0.1");
    UPROPERTY(Config, EditAnywhere, Category="Connection") int32 StationPort = 25790;
    // Enable on a PC with DivingStation. Disable for a Quest receiving the PC relay.
    UPROPERTY(Config, EditAnywhere, Category="Connection") bool bConnectToStation = PLATFORM_WINDOWS != 0;
    UPROPERTY(Config, EditAnywhere, Category="Tracking", meta=(ClampMin="0.02", ClampMax="1.0")) float StaleAfterSeconds = 0.15f;
    // Hand relative to the physical tracker, after converting tracker axes to UE basis.
    // Resulting hand frame: +X towards fingertips, +Z out of the back of the hand.
    UPROPERTY(Config, EditAnywhere, Category="Tracking|Mount Calibration") FTransform LeftTrackerToHand = FTransform::Identity;
    UPROPERTY(Config, EditAnywhere, Category="Tracking|Mount Calibration") FTransform RightTrackerToHand = FTransform::Identity;
};
