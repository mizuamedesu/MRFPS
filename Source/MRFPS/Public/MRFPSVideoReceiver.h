#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "MRFPSVideoReceiver.generated.h"

class UTexture2D;
struct FMRFPSVideoReceiverState;

/** Receives complete, already authorized JPEG frames. This class opens no network connection. */
UCLASS()
class MRFPS_API UMRFPSVideoReceiver : public UGameInstanceSubsystem, public FTickableGameObject
{
    GENERATED_BODY()
public:
    UMRFPSVideoReceiver();
    virtual ~UMRFPSVideoReceiver() override;

    /** Game thread only. True means accepted into the latest-frame queue, not decoded yet.
     * Slot is 0/1. Nonzero StreamId is a server-assigned increasing generation;
     * nonzero Sequence strictly increases within that generation (no wrap).
     * JPEG <=60 KiB; actual 8-bit JPEG dimensions must match Width/Height and be <=1280x720.
     */
    bool SubmitFrame(int32 Slot, uint32 StreamId, uint32 Sequence, int32 Width, int32 Height, TArray<uint8> Jpeg);
    /** Fixed 640x360 letterboxed texture, or null when no uploaded frame is younger than one second. */
    UTexture2D* GetFreshTexture(int32 Slot) const;
    /** Also requires the RHI-cleanup-acknowledged frame's StreamId to match. This is not a GPU presentation fence. */
    UTexture2D* GetFreshTextureForStream(int32 Slot, uint32 ExpectedStreamId) const;
    /** Safe metadata only. Never returns image bytes, credentials, a URL, or remote error text. */
    FString GetSafeStatus(int32 Slot) const;
    /** Same fixed diagnostics, with an exact current publisher generation check. */
    FString GetSafeStatusForStream(int32 Slot, uint32 ExpectedStreamId) const;
    /** Close through this server generation only; an older stop cannot affect newer work/display. */
    void StopStream(int32 Slot, uint32 StoppedStreamId);
    /** Closes the current slot generation and invalidates queued/late work. A higher StreamId may reopen it. */
    void ResetSlot(int32 Slot);
    /** Clears all generations for a new game-server session, invalidating every old worker/upload. */
    void ResetAll();

    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void BeginDestroy() override;
    virtual void Tick(float DeltaTime) override;
    virtual bool IsTickable() const override;
    virtual bool IsTickableWhenPaused() const override { return true; }
    virtual TStatId GetStatId() const override;
    virtual UWorld* GetTickableGameObjectWorld() const override;

private:
    void StopReceiver();
    UPROPERTY(Transient) TArray<TObjectPtr<UTexture2D>> Textures;
    // Forward-safe for UHT's generated vtable-helper constructor. Accessed only
    // by this subsystem on the game thread; workers receive only its mailbox.
    TSharedPtr<FMRFPSVideoReceiverState> State;
    bool bEnabled = false;
};
