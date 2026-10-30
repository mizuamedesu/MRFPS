#include "MRFPSLoginInteraction.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "InputCoreTypes.h"

void UMRFPSLoginInteraction::RefreshPointer() { SimulatePointerMovement(); }

bool UMRFPSLoginInteraction::BeginPointer()
{
    CapturedByThisPointer.Reset();
    if (!FSlateApplication::IsInitialized() || !VirtualUser.IsValid()) return false;
    auto User=FSlateApplication::Get().GetUser(VirtualUser->GetUserIndex());
    if (!User || User->GetPointerCaptor(static_cast<uint32>(PointerIndex))) return false;
    PressPointerKey(EKeys::LeftMouseButton);
    const auto Captor=User->GetPointerCaptor(static_cast<uint32>(PointerIndex));
    const FWidgetPath Path=LastWidgetPath.ToWidgetPath();
    if (Captor && Path.IsValid() && Path.ContainsWidget(Captor.Get())) { CapturedByThisPointer=Captor; return true; }
    // The component cannot prove which widget owns capture. Never release an
    // unidentified captor belonging to another interaction component.
    PressedKeys.Remove(EKeys::LeftMouseButton);
    return false;
}

void UMRFPSLoginInteraction::EndPointer()
{
    bool OwnsCapture=false;
    if (PressedKeys.Contains(EKeys::LeftMouseButton) && FSlateApplication::IsInitialized() && VirtualUser.IsValid()) {
        if (auto User=FSlateApplication::Get().GetUser(VirtualUser->GetUserIndex())) {
            auto Owned=CapturedByThisPointer.Pin();
            OwnsCapture=Owned && User->GetPointerCaptor(static_cast<uint32>(PointerIndex))==Owned;
        }
    }
    if (OwnsCapture) ReleasePointerKey(EKeys::LeftMouseButton);
    else CancelPointer();
    CapturedByThisPointer.Reset();
}

void UMRFPSLoginInteraction::CancelPointer()
{
    // SButton clears its pressed state on capture loss without firing OnClicked.
    // Do not synthesize mouse-up: it could activate the last hovered button.
    if (PressedKeys.Contains(EKeys::LeftMouseButton) && FSlateApplication::IsInitialized() && VirtualUser.IsValid()) {
        if (auto User=FSlateApplication::Get().GetUser(VirtualUser->GetUserIndex())) {
            auto Owned=CapturedByThisPointer.Pin();
            if (Owned && User->GetPointerCaptor(static_cast<uint32>(PointerIndex))==Owned)
                User->ReleaseCapture(static_cast<uint32>(PointerIndex));
        }
    }
    CapturedByThisPointer.Reset();
    PressedKeys.Remove(EKeys::LeftMouseButton);
    LastWidgetPath=FWeakWidgetPath();
    WeakHoveredWidgetComponent.Reset();
    LastHitResult=FHitResult();
    bIsHoveredWidgetInteractable=false;
    bIsHoveredWidgetFocusable=false;
    bIsHoveredWidgetHitTestVisible=false;
}
