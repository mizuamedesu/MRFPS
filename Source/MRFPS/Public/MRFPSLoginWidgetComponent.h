#pragma once

#include "CoreMinimal.h"
#include "Components/WidgetComponent.h"
#include "MRFPSLoginWidgetComponent.generated.h"

// Pointer input must run after this widget's render has populated its Slate
// hit-test grid, never from inside the user widget's paint-time NativeTick.
UCLASS(Transient)
class MRFPS_API UMRFPSLoginWidgetComponent : public UWidgetComponent
{
    GENERATED_BODY()
public:
    virtual void TickComponent(float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* ThisTickFunction) override;
};
