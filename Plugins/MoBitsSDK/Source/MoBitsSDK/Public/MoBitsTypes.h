#pragma once

#include "CoreMinimal.h"
#include "MoBitsTypes.generated.h"

UENUM(BlueprintType)
enum class EMoBitsBodyTarget : uint8
{
    None = 0,
    Chest = 4, Waist = 5, Hip = 6,
    LeftUpperArm = 14, RightUpperArm = 18,
    LeftForearm = 13, RightForearm = 17,
    LeftThigh = 7, RightThigh = 10,
    LeftShin = 8, RightShin = 11
};

/**
 * Body-targeted haptic effects. Values are the wire IDs 1..30 and must never be
 * renumbered; renaming an enumerator requires a CoreRedirects EnumRedirect.
 * Only Tap/Nudge/Impact have app definitions; Effect04..Effect30 return effect_unconfigured.
 * Global effects (PlayGlobal) use an independent integer namespace, not this enum.
 */
UENUM(BlueprintType)
enum class EMoBitsHapticEffect : uint8
{
    None = 0 UMETA(Hidden),
    Tap = 1 UMETA(ToolTip="ID 1: short, sharp tap (80 ms, 120 Hz)"),
    Nudge = 2 UMETA(ToolTip="ID 2: soft-attack nudge (150 ms, 80 Hz, 20 ms attack)"),
    Impact = 3 UMETA(ToolTip="ID 3: longer, low impact (200 ms, 40 Hz)"),
    Effect04 = 4, Effect05 = 5, Effect06 = 6, Effect07 = 7, Effect08 = 8, Effect09 = 9, Effect10 = 10,
    Effect11 = 11, Effect12 = 12, Effect13 = 13, Effect14 = 14, Effect15 = 15,
    Effect16 = 16, Effect17 = 17, Effect18 = 18, Effect19 = 19, Effect20 = 20,
    Effect21 = 21, Effect22 = 22, Effect23 = 23, Effect24 = 24, Effect25 = 25,
    Effect26 = 26, Effect27 = 27, Effect28 = 28, Effect29 = 29, Effect30 = 30
};

/** App-registered school-festival presets (body PLAY IDs, not global IDs).
 * Existing EMoBitsHapticEffect values/names are preserved for saved Blueprints.
 * No output is configured until the wearer-specific set is prepared in the app.
 */
UENUM(BlueprintType)
enum class EMoBitsMrFpsPreset : uint8
{
    None = 0 UMETA(Hidden),
    PistolRecoil = 4 UMETA(DisplayName="Pistol Recoil / 拳銃の反動"),
    HeavyRecoil = 5 UMETA(DisplayName="Heavy Recoil / 重い銃の反動"),
    AutomaticKick = 6 UMETA(DisplayName="Automatic Kick / 連射の蹴り1回"),
    Graze = 7 UMETA(DisplayName="Graze / かすり"),
    Hit = 8 UMETA(DisplayName="Hit / 命中"),
    HeavyHit = 9 UMETA(DisplayName="Heavy Hit / 重い一撃"),
    LethalHit = 10 UMETA(DisplayName="Lethal Hit / 致命傷"),
    Revive = 11 UMETA(DisplayName="Revive / 復活のゾワゾワ"),
    AutomaticVibration = 12 UMETA(DisplayName="Automatic Vibration / 連射の振動のみ"),
    AutomaticShot = 13 UMETA(DisplayName="Automatic Shot / 連射1発を追加")
};

UENUM(BlueprintType)
enum class EMoBitsConnectionState : uint8 { Disconnected, Connecting, Connected, Disconnecting, Faulted };

USTRUCT(BlueprintType)
struct MOBITSSDK_API FMoBitsReceipt
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="MoBits") int64 Id = 0;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") FString Status;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") FString Reason;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") FString DeviceId;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") float AppliedLevel = 0;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") bool bSucceeded = false;
};

USTRUCT(BlueprintType)
struct MOBITSSDK_API FMoBitsDeviceInfo
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="MoBits") FString DeviceId;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") int32 BoneId = 0;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") int32 ModelNumber = 0;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") int32 FirmwareVersion = 0;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") bool bConnected = false;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") int32 BatteryPercent = 0;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") bool bTracking = false;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") bool bEmsSupported = false;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") bool bLraSupported = false;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") bool bHapticsPermitted = false;
};

USTRUCT(BlueprintType)
struct MOBITSSDK_API FMoBitsDevicePose
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="MoBits") FString DeviceId;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") int32 BoneId = 0;
    /** Rotation in UE coordinates: X forward, Y right, Z up. */
    UPROPERTY(BlueprintReadOnly, Category="MoBits") FQuat Rotation = FQuat::Identity;
    /** Centimeters relative to the app tracking origin; an IMU skeleton estimate. */
    UPROPERTY(BlueprintReadOnly, Category="MoBits") FVector PositionCm = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") bool bHasPosition = false;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") FString PositionSource;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") double SampleAgeMs = 0;
};

USTRUCT(BlueprintType)
struct MOBITSSDK_API FMoBitsBonePose
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="MoBits") int32 BoneId = 0;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") int32 ParentBoneId = 0;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") FName Name;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") FQuat LocalRotation = FQuat::Identity;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") FVector LocalPositionCm = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct MOBITSSDK_API FMoBitsMotionFrame
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="MoBits") int64 Sequence = 0;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") int64 OriginRevision = 0;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") double ServerTimeMs = 0;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") bool bTrackingActive = false;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") TArray<FMoBitsDevicePose> Devices;
    UPROPERTY(BlueprintReadOnly, Category="MoBits") TArray<FMoBitsBonePose> Bones;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMoBitsReceiptEvent, const FMoBitsReceipt&, Receipt);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMoBitsDevicesEvent, const TArray<FMoBitsDeviceInfo>&, Devices);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMoBitsMotionEvent, const FMoBitsMotionFrame&, Frame);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMoBitsStateEvent, EMoBitsConnectionState, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMoBitsErrorEvent, const FString&, Message);
