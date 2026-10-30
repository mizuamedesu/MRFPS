#pragma once

#include "CoreMinimal.h"
#include "Components/WidgetInteractionComponent.h"
#include "MRFPSLoginInteraction.generated.h"

class UWidgetComponent;

/** Cancels an interrupted login pointer without synthesizing a click. */
UCLASS(Transient)
class MRFPS_API UMRFPSLoginInteraction : public UWidgetInteractionComponent
{
    GENERATED_BODY()
public:
    bool BeginPointer();
    void EndPointer();
    void CancelPointer();
    void RefreshPointer();
    void SetLoginWorldWidget(UWidgetComponent* Widget);
    TWeakPtr<SWidget> HoveredTarget() const;
private:
    TWeakPtr<SWidget> CapturedByThisPointer;
    TWeakObjectPtr<UWidgetComponent> LoginWorldWidget;
    int32 LastIntersection=-1;
};
