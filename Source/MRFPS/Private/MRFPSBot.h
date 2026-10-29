#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "BehaviorTree/BTTaskNode.h"
#include "MRFPSBot.generated.h"

class AMRFPSGameMode;
class AMRFPSPawn;
class AMRFPSPlayerState;
class UBehaviorTree;
class UEnvQuery;
struct FEnvQueryResult;

/** Server-owned controller. It never represents an authenticated network client. */
UCLASS(Transient, NotBlueprintable)
class AMRFPSBotController : public AAIController
{
    GENERATED_BODY()
public:
    AMRFPSBotController();
    bool InitializeBot(AMRFPSPawn* Fighter, AMRFPSPawn* Human);
    void Step(float DeltaSeconds);
    void QueryFinished(TSharedPtr<FEnvQueryResult> Result);
    bool IsPreparedFor(const AMRFPSPawn* Human) const;
    void ResetNavigationForLayout();
    bool bAutoStartPending=true;
    bool bPausedForTracking=false;
private:
    UPROPERTY(Transient) TObjectPtr<UBehaviorTree> RuntimeTree;
    UPROPERTY(Transient) TObjectPtr<UEnvQuery> RuntimeQuery;
    TWeakObjectPtr<AMRFPSPawn> HumanPlayer;
    FVector Destination=FVector::ZeroVector;
    bool bHasDestination=false;
    bool bQueryInFlight=false;
    bool bQuerySucceeded=false;
    bool bWasDead=false;
    bool bBrainReady=false;
    bool bLoggedMove=false;
    bool bLoggedShot=false;
    bool bLoggedQuery=false;
    int32 QueryId=INDEX_NONE;
    double NextQueryAt=0;
    double NextStepAt=0;
    double LastStepAt=-1;
    double QueryDeadline=0;
    double NextShotAt=0;
    double ReloadAt=0;
    double NextFailureLogAt=0;
    double NextGeometryLogAt=0;
};

/** Persistent behavior-tree task; all authority stays in the controller. */
UCLASS(Transient, NotBlueprintable)
class UMRFPSBotCombatTask : public UBTTaskNode
{
    GENERATED_BODY()
public:
    UMRFPSBotCombatTask();
    virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory) override;
    virtual void TickTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,float DeltaSeconds) override;
};

namespace MRFPSBot
{
    bool IsServerBot(const AMRFPSPawn* Fighter);
    bool CanMove(UWorld* World,const AMRFPSPawn* Fighter,const FVector& Start,const FVector& End);
    bool IsPrepared(const AMRFPSGameMode* Mode);
    bool IsTrackingPause(const AMRFPSGameMode* Mode);
    bool ClaimTrackingPause(AMRFPSGameMode* Mode);
    void CancelAutoStart(AMRFPSGameMode* Mode);
    int32 CountHumanFighters(const AMRFPSGameMode* Mode,const AMRFPSPlayerState* Exclude=nullptr);
    void BeforeHumanAdmission(AMRFPSGameMode* Mode);
    bool ResizeSolo(AMRFPSGameMode* Mode,double Width,double Length,FString& Error);
    void Maintain(AMRFPSGameMode* Mode,float DeltaSeconds);
}
