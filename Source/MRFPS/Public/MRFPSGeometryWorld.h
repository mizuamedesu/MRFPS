#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MRFPSGeometryProtocol.h"
#include "MRFPSGeometryWorld.generated.h"

class AMRFPSPawn;
class AActor;
struct FMRFPSGeometryWorldState;

/** Two bounded measured surface snapshots. Server collision is CPU-only;
 * renderer clients may visualize them without creating any Chaos bodies. */
UCLASS()
class MRFPS_API UMRFPSGeometryWorld : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual bool IsTickableWhenPaused() const override { return true; }
    virtual TStatId GetStatId() const override;
    bool Commit(AMRFPSPawn* Owner,const FMRFPSGeometryChunk& Header,FMRFPSGeometryMesh Mesh);
    void RemoveOwner(AMRFPSPawn* Owner);
    void ResetAll();
    bool Trace(const FVector& Start,const FVector& End,FVector& HitPoint) const;
    bool HasLiveMeasurement(int32 Slot) const;
    /** Conservative authority-only broad phase for placing a bot. A triangle's
     * bounds may overlap even if the triangle itself does not. */
    bool OverlapsMeasuredBounds(const FBox& Bounds) const;
    /** Read-only visual foot placement on validated replicated measurements.
     * Combat still uses the authority-only Trace entry point. */
    bool SampleAvatarGround(const FVector& Probe,float& GroundZ) const;
    FString GetSafeStatus() const;
protected:
    virtual bool DoesSupportWorldType(EWorldType::Type Type) const override;
private:
    TSharedPtr<FMRFPSGeometryWorldState> State;
    UPROPERTY(Transient) TArray<TObjectPtr<AActor>> RenderActors;
    bool OwnerValid(int32 Slot) const;
    bool TraceMeasuredSurface(const FVector& Start,const FVector& End,FVector& HitPoint) const;
    bool OwnerIdentityValid(int32 Slot) const;
    void ClearSlot(int32 Slot,bool Retire=true);
    void UpdateVisual(int32 Slot);
};
