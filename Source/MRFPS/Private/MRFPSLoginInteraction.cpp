#include "MRFPSLoginInteraction.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "InputCoreTypes.h"

void UMRFPSLoginInteraction::RefreshPointer() { SimulatePointerMovement(); }

TWeakPtr<SWidget> UMRFPSLoginInteraction::HoveredTarget() const
{
    const FWidgetPath Path=LastWidgetPath.ToWidgetPath();
    if (!Path.IsValid()) return {};
    // Use the owning control, not its text child, so movement between the
    // label and padding of the same button does not cancel a valid click.
    TWeakPtr<SWidget> Result=Path.Widgets.Last().Widget;
    for (int32 I=Path.Widgets.Num()-1;I>=0;--I) {
        const auto& Widget=Path.Widgets[I].Widget;
        const FName Type=Widget->GetType();
        if (Type==FName(TEXT("SButton")) || Type==FName(TEXT("SEditableTextBox")) || Type==FName(TEXT("SEditableText"))) Result=Widget;
    }
    return Result;
}

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
    TSharedPtr<FSlateUser> OwnedUser;
    if (PressedKeys.Contains(EKeys::LeftMouseButton) && FSlateApplication::IsInitialized() && VirtualUser.IsValid()) {
        if (auto User=FSlateApplication::Get().GetUser(VirtualUser->GetUserIndex())) {
            auto Owned=CapturedByThisPointer.Pin();
            if (Owned && User->GetPointerCaptor(static_cast<uint32>(PointerIndex))==Owned)
                OwnedUser=User;
        }
    }
    // Clear our ownership first: capture-lost and mouse-leave delegates may
    // re-enter this component. Never cancel another component's capture.
    CapturedByThisPointer.Reset();
    PressedKeys.Remove(EKeys::LeftMouseButton);
    LastWidgetPath=FWeakWidgetPath();
    WeakHoveredWidgetComponent.Reset();
    LastHitResult=FHitResult();
    bIsHoveredWidgetInteractable=false;
    bIsHoveredWidgetFocusable=false;
    bIsHoveredWidgetHitTestVisible=false;
    if (OwnedUser && FSlateApplication::IsInitialized() && VirtualUser.IsValid()) {
        const uint32 Index=static_cast<uint32>(PointerIndex);
        OwnedUser->ReleaseCapture(Index);
        if (!OwnedUser->GetPointerCaptor(Index)) {
            // UE's ReleaseCapture records the old captor path as the last
            // hover even if the pointer already left it while captured. An
            // empty synthetic move emits leave without a click and resets that
            // path; the next real trace can then enter the correct control.
            const FPointerEvent Event(VirtualUser->GetUserIndex(),Index,LocalHitLocation,LocalHitLocation,
                PressedKeys,FKey(),0.f,ModifierKeys);
            FSlateApplication::Get().RoutePointerMoveEvent(FWidgetPath(),Event,true);
        }
    }
}
