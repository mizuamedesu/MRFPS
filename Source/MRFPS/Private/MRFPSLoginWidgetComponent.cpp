#include "MRFPSLoginWidgetComponent.h"
#include "MRFPSLoginMenu.h"

UMRFPSLoginWidgetComponent::UMRFPSLoginWidgetComponent()
{
    // UWorld updates player cameras after PostPhysics, before PostUpdateWork.
    // The head-attached panel and its pointer must use that updated pose.
    PrimaryComponentTick.TickGroup=TG_PostUpdateWork;
}

void UMRFPSLoginWidgetComponent::TickComponent(float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* ThisTickFunction)
{
    const double Before=LastWidgetRenderTime;
    Super::TickComponent(DeltaTime,TickType,ThisTickFunction);
    // The engine changes this time only after DrawWindow has completed. If
    // it skipped drawing (hidden, invalid size or NullRHI), do not use the
    // previous frame's stale hit grid.
    if (LastWidgetRenderTime!=Before && GetRenderTarget() && GetSlateWindow().IsValid() && IsWidgetVisible()) {
        if (auto* Menu=Cast<UMRFPSLoginMenu>(GetWidget())) Menu->TickXRPointerAfterPaint(this,DeltaTime);
    }
    // The callback may close the menu and destroy this component.
}
