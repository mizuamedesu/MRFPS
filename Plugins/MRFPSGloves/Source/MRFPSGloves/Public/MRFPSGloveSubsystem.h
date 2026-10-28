#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "MRFPSGloveSubsystem.generated.h"

class FSocket;
class FInternetAddr;

UENUM(BlueprintType)
enum class EMRFPSGloveHand : uint8 { Left, Right };

USTRUCT(BlueprintType)
struct MRFPSGLOVES_API FMRFPSGloveHandFrame
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString DeviceId;
    UPROPERTY(BlueprintReadOnly) bool bFlexValid = false;
    UPROPERTY(BlueprintReadOnly) bool bPoseValid = false;
    UPROPERTY(BlueprintReadOnly) bool bControllerValid = false;
    UPROPERTY(BlueprintReadOnly) bool bJointRotationsValid = false;
    UPROPERTY(BlueprintReadOnly) bool bWristImuValid = false;
    UPROPERTY(BlueprintReadOnly) bool bDeviceStatusValid = false;
    UPROPERTY(BlueprintReadOnly) bool bConnected = false;
    UPROPERTY(BlueprintReadOnly) bool bFlexStreamFresh = false;
    UPROPERTY(BlueprintReadOnly) int32 BatteryPercent = -1;
    UPROPERTY(BlueprintReadOnly) double DeviceStatusReceivedAt = -1.0;
    // Last receipt time in this process's monotonic clock. These are separate streams.
    UPROPERTY(BlueprintReadOnly) double FlexReceivedAt = -1.0;
    UPROPERTY(BlueprintReadOnly) double PoseReceivedAt = -1.0;
    UPROPERTY(BlueprintReadOnly) double ControllerReceivedAt = -1.0;
    UPROPERTY(BlueprintReadOnly) double JointsReceivedAt = -1.0;
    UPROPERTY(BlueprintReadOnly) double WristReceivedAt = -1.0;
    UPROPERTY(BlueprintReadOnly) float FlexAgeSeconds = 0.0f;
    // Thumb, index, middle, ring, little. 0=open, 1=closed. Never initialized as tracked.
    UPROPERTY(BlueprintReadOnly) TArray<float> FingerCurls;
    // 3 normalized flexion values per finger, followed separately by the five splays.
    UPROPERTY(BlueprintReadOnly) TArray<float> Flexion;
    UPROPERTY(BlueprintReadOnly) TArray<float> Splay;
    // Vendor HandQuat basis, thumb->little, 3 local rotations each. Retarget before animation.
    UPROPERTY(BlueprintReadOnly) TArray<FQuat> FingerLocalRotations;
    // /MRFPS/Tracker extension after mounting calibration: UE tracking space,
    // anatomically +X=fingertips, +Z=dorsal. Not a controller Aim pose.
    UPROPERTY(BlueprintReadOnly) FTransform TrackingTransform = FTransform::Identity;
    // Raw vendor Wrist IMU basis, not a tracked position or a Quest-relative aim transform.
    UPROPERTY(BlueprintReadOnly) FQuat WristImuRotation = FQuat::Identity;
    UPROPERTY(BlueprintReadOnly) float Trigger = 0.0f;
    UPROPERTY(BlueprintReadOnly) float Grip = 0.0f;
    UPROPERTY(BlueprintReadOnly) bool bTriggerPressed = false;
    UPROPERTY(BlueprintReadOnly) bool bAButtonPressed = false;
    UPROPERTY(BlueprintReadOnly) bool bBButtonPressed = false;

    FMRFPSGloveHandFrame()
    {
        FingerCurls.Init(0.0f, 5);
        Flexion.Init(0.0f, 15);
        Splay.Init(0.0f, 5);
    }

    bool IsOpenPalm(float MaximumCurl = 0.24f) const
    {
        if (!bFlexValid || FingerCurls.Num() != 5) return false;
        for (const float Curl : FingerCurls) if (Curl > MaximumCurl) return false;
        return true;
    }
};

UCLASS()
class MRFPSGLOVES_API UMRFPSGloveSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
    GENERATED_BODY()
public:
    UMRFPSGloveSubsystem();
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void BeginDestroy() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override { return Socket != nullptr && !IsTemplate(); }
    virtual UWorld* GetTickableGameObjectWorld() const override { return GetWorld(); }

    // True means fresh flex data, independently of whether a positional tracker is present.
    UFUNCTION(BlueprintCallable, Category="MRFPS|Gloves")
    bool GetLatestHand(EMRFPSGloveHand Hand, FMRFPSGloveHandFrame& Frame) const;
    UFUNCTION(BlueprintPure, Category="MRFPS|Gloves") bool IsReceiving() const;

    // For an OpenXR/SteamVR pose supplied by the local pawn; does not invent a pose.
    void SubmitTrackedPose(EMRFPSGloveHand Hand, const FTransform& TrackingPose, bool bTracked);

private:
    void SendStationControl(const ANSICHAR* Address);
    bool ProcessMessage(const uint8* Data, int32 Size, double Now);
    void ProcessPacket(const uint8* Data, int32 Size, double Now, int32 Depth = 0);
    FSocket* Socket = nullptr;
    TSharedPtr<FInternetAddr> Station;
    FString AllowedSender;
    FString PinnedDeviceId;
    FString SelectedDeviceId;
    float StaleAfter = 0.15f;
    bool bStationConnection = false;
    int32 ReceiverPort = 25788;
    double NextConnectAt = 0.0;
    FMRFPSGloveHandFrame Hands[2];
    FTransform TrackerMounts[2] = { FTransform::Identity, FTransform::Identity };
    bool bMountValid[2] = { true, true };
};
