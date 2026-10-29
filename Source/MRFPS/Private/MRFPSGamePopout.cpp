#include "MRFPSGame.h"
#include "MRFPSInspectorHudLayout.h"

#if !UE_SERVER && (PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX)
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
// Scene-capture render targets can contain an undefined/inverted alpha
// channel. Treat the inspector's video as an opaque image, like a viewport.
class SMRFPSOpaqueCapture : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SMRFPSOpaqueCapture) {} SLATE_END_ARGS()
    void Construct(const FArguments&,const FSlateBrush* InBrush) { Brush=InBrush; }
    virtual FVector2D ComputeDesiredSize(float) const override { return Brush ? Brush->ImageSize : FVector2D::ZeroVector; }
    virtual int32 OnPaint(const FPaintArgs&,const FGeometry& Geometry,const FSlateRect&,FSlateWindowElementList& Elements,
                          int32 Layer,const FWidgetStyle& Style,bool bParentEnabled) const override
    {
        if (Brush && Brush->DrawAs!=ESlateBrushDrawType::NoDrawType) {
            const ESlateDrawEffect Effects=ESlateDrawEffect::IgnoreTextureAlpha |
                (ShouldBeEnabled(bParentEnabled) ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect);
            FSlateDrawElement::MakeBox(Elements,Layer,Geometry.ToPaintGeometry(),Brush,Effects,
                Style.GetColorAndOpacityTint()*Brush->GetTint(Style));
        }
        return Layer;
    }
private:
    const FSlateBrush* Brush=nullptr;
};

class SMRFPSPlayerVideo : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMRFPSPlayerVideo) {} SLATE_END_ARGS()
    void Construct(const FArguments&,TWeakObjectPtr<AMRFPSPlayerController> InOwner,int32 InSlot)
    {
        Owner=InOwner; Slot=InSlot;
        Brush.DrawAs=ESlateBrushDrawType::Image;
        Brush.ImageSize=FVector2D(1280,720);
        ChildSlot
        [
            SNew(SOverlay)
            +SOverlay::Slot()
            [ SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor::Black).Padding(0) ]
            +SOverlay::Slot()
            [
                SNew(SScaleBox).Stretch(EStretch::ScaleToFit)
                [ SNew(SImage).Image(this,&SMRFPSPlayerVideo::Image).Visibility(this,&SMRFPSPlayerVideo::ImageVisibility) ]
            ]
            +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
            [
                SNew(STextBlock).Text(FText::FromString(TEXT("映像を待っています")))
                .Font(FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),18))
                .ColorAndOpacity(FSlateColor(FLinearColor::White)).Visibility(this,&SMRFPSPlayerVideo::WaitingVisibility)
            ]
        ];
    }
private:
    UTexture2D* Frame() const { const auto* PC=Owner.Get(); return PC ? PC->GetQuestVideoTexture(Slot) : nullptr; }
    const FSlateBrush* Image() const
    {
        auto* Texture=Frame();
        Brush.SetResourceObject(Texture);
        if (Texture && Texture->GetSizeX()>0 && Texture->GetSizeY()>0) Brush.ImageSize=FVector2D(Texture->GetSizeX(),Texture->GetSizeY());
        return &Brush;
    }
    EVisibility ImageVisibility() const { return Frame() ? EVisibility::Visible : EVisibility::Collapsed; }
    EVisibility WaitingVisibility() const { return Frame() ? EVisibility::Collapsed : EVisibility::Visible; }
    TWeakObjectPtr<AMRFPSPlayerController> Owner;
    int32 Slot=0;
    mutable FSlateBrush Brush;
};

// A second free camera shares the existing authenticated observer connection.
// It has its own transform and input, while its scene capture keeps updating
// even when this window does not own keyboard focus.
class SMRFPSInspectorVideo : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMRFPSInspectorVideo) {} SLATE_END_ARGS()
    void Construct(const FArguments&,TWeakObjectPtr<AMRFPSPlayerController> InOwner)
    {
        Owner=InOwner;
        Brush.DrawAs=ESlateBrushDrawType::Image;
        Brush.ImageSize=FVector2D(960,540);
        if (auto* PC=Owner.Get()) Brush.SetResourceObject(PC->GetInspectorWindowTarget());
        ChildSlot
        [
            SNew(SOverlay)
            +SOverlay::Slot()
            [ SNew(SScaleBox).Stretch(EStretch::ScaleToFit) [ SNew(SMRFPSOpaqueCapture,&Brush) ] ]
            +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(0,18,0,0)
            [
                SNew(SBorder).Visibility(this,&SMRFPSInspectorVideo::RoundVisibility)
                .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                .BorderBackgroundColor(FLinearColor(.008f,.025f,.045f,.78f)).Padding(FMargin(20,6))
                [
                    SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
                    [ SNew(STextBlock).Text(this,&SMRFPSInspectorVideo::RoundHeader)
                      .Font(FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),20))
                      .ColorAndOpacity(FSlateColor(FLinearColor(.75f,1.f,.96f))) ]
                    +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0,4,0,0)
                    [ SNew(STextBlock).Text(this,&SMRFPSInspectorVideo::RoundScore)
                      .Font(FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),16))
                      .ColorAndOpacity(FSlateColor(FLinearColor(.55f,.8f,.9f))) ]
                ]
            ]
        ];
    }
    virtual bool SupportsKeyboardFocus() const override { return true; }
    virtual FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent& Event) override
    {
        FReply Reply=FReply::Handled().SetUserFocus(SharedThis(this),EFocusCause::Mouse);
        if (Event.GetEffectingButton()==EKeys::RightMouseButton) {
            bLooking=true;
            Reply.CaptureMouse(SharedThis(this)).UseHighPrecisionMouseMovement(SharedThis(this));
        }
        return Reply;
    }
    virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent& Event) override
    {
        if (Event.GetEffectingButton()==EKeys::RightMouseButton) { bLooking=false; return FReply::Handled().ReleaseMouseCapture(); }
        return FReply::Handled();
    }
    virtual FReply OnMouseMove(const FGeometry&,const FPointerEvent& Event) override
    {
        if (!bLooking || !HasMouseCapture()) return FReply::Unhandled();
        if (auto* PC=Owner.Get()) if (auto* Camera=PC->GetInspectorWindowCamera()) {
            FRotator Rotation=Camera->GetComponentRotation();
            Rotation.Yaw+=Event.GetCursorDelta().X*.12;
            Rotation.Pitch=FMath::Clamp(Rotation.Pitch-Event.GetCursorDelta().Y*.12,-89.,89.);
            Camera->SetWorldRotation(Rotation);
        }
        return FReply::Handled();
    }
    virtual FReply OnKeyDown(const FGeometry&,const FKeyEvent& Event) override
    {
        if (Event.GetKey()==EKeys::Escape) {
            bLooking=false; ClearKeys();
            return HasMouseCapture()?FReply::Handled().ReleaseMouseCapture():FReply::Handled();
        }
        const int32 Index=MoveKey(Event.GetKey());
        if (Index==INDEX_NONE) return FReply::Unhandled();
        Keys[Index]=true; return FReply::Handled();
    }
    virtual FReply OnKeyUp(const FGeometry&,const FKeyEvent& Event) override
    {
        const int32 Index=MoveKey(Event.GetKey());
        if (Index==INDEX_NONE) return FReply::Unhandled();
        Keys[Index]=false; return FReply::Handled();
    }
    virtual void OnFocusLost(const FFocusEvent& Event) override
    {
        bLooking=false; ClearKeys(); SCompoundWidget::OnFocusLost(Event);
    }
    virtual void Tick(const FGeometry& Geometry,const double Time,const float DeltaTime) override
    {
        SCompoundWidget::Tick(Geometry,Time,DeltaTime);
        if (!FApp::HasFocus() || !HasKeyboardFocus()) { ClearKeys(); return; }
        if (auto* PC=Owner.Get()) if (auto* Camera=PC->GetInspectorWindowCamera()) {
            const FRotator Rotation=Camera->GetComponentRotation();
            const FVector Direction=Rotation.Vector()*double(Keys[0]-Keys[1])+
                FRotationMatrix(Rotation).GetUnitAxis(EAxis::Y)*double(Keys[2]-Keys[3])+
                FVector::UpVector*double(Keys[4]-Keys[5]);
            if (!Direction.IsNearlyZero()) Camera->AddWorldOffset(Direction.GetClampedToMaxSize(1)*650*FMath::Clamp(DeltaTime,0.f,.05f));
        }
    }
private:
    const AMRFPSGameState* RoundState() const
    {
        const auto* PC=Owner.Get();
        const auto* World=PC?PC->GetWorld():nullptr;
        return World?World->GetGameState<AMRFPSGameState>():nullptr;
    }
    FText RoundHeader() const { const auto* State=RoundState(); return State?FText::FromString(State->RoundHeaderText()):FText::GetEmpty(); }
    FText RoundScore() const { const auto* State=RoundState(); return State?FText::FromString(State->RoundScoreText(-1)):FText::GetEmpty(); }
    EVisibility RoundVisibility() const { return RoundState()?EVisibility::HitTestInvisible:EVisibility::Collapsed; }
    int32 MoveKey(const FKey& Key) const
    {
        const FKey Mapping[]={EKeys::W,EKeys::S,EKeys::D,EKeys::A,EKeys::E,EKeys::Q};
        for (int32 I=0;I<6;++I) if (Key==Mapping[I]) return I;
        return INDEX_NONE;
    }
    void ClearKeys() { for (bool& Key:Keys) Key=false; }
    TWeakObjectPtr<AMRFPSPlayerController> Owner;
    FSlateBrush Brush;
    bool Keys[6]={};
    bool bLooking=false;
};
}
#endif

void AMRFPSPlayerController::OpenPlayerVideoWindow(int32 Slot)
{
#if !UE_SERVER && (PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX)
    if (!IsLocalController() || !IsInspector() || Slot<0 || Slot>=2 || !FSlateApplication::IsInitialized()) return;
    if (auto Existing=PlayerVideoWindows[Slot].Pin()) { Existing->BringToFront(true); return; }
    const TWeakObjectPtr<AMRFPSPlayerController> WeakThis(this);
    TSharedRef<SWindow> Window=SNew(SWindow)
        .Title(FText::FromString(FString::Printf(TEXT("MRFPS - P%d"),Slot+1)))
        .ClientSize(FVector2D(960,540))
        .SupportsMinimize(true).SupportsMaximize(true)
        [ SNew(SMRFPSPlayerVideo,WeakThis,Slot) ];
    Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([WeakThis,Slot](const TSharedRef<SWindow>& Closed) {
        if (auto* PC=WeakThis.Get()) {
            if (PC->PlayerVideoWindows[Slot].Pin()==Closed) PC->PlayerVideoWindows[Slot].Reset();
        }
    }));
    PlayerVideoWindows[Slot]=Window;
    FSlateApplication::Get().AddWindow(Window);
#endif
}

bool AMRFPSPlayerController::TryOpenInspectorWindowAtCursor()
{
#if !UE_SERVER && (PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX)
    float X=0,Y=0;
    if (!IsLocalController() || !IsInspector() || !GetMousePosition(X,Y)) return false;
    int32 Width=0,Height=0; GetViewportSize(Width,Height);
    const FMRFPSInspectorHudLayout Layout{float(Width)};
    const int32 Button=Layout.HitButton(X,Y);
    if (Button>=0 && Button<2) { OpenPlayerVideoWindow(Button); return true; }
    if (Button==2) { OpenInspectorViewWindow(); return true; }
#endif
    return false;
}

void AMRFPSPlayerController::OpenInspectorViewWindow()
{
#if !UE_SERVER && (PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX)
    if (!IsLocalController() || !IsInspector() || !FSlateApplication::IsInitialized() || !GetWorld()) return;
    if (auto Existing=InspectorViewWindow.Pin()) { Existing->BringToFront(true); return; }
    InspectorWindowTarget=NewObject<UTextureRenderTarget2D>(this);
    InspectorWindowTarget->InitAutoFormat(960,540);
    InspectorWindowTarget->ClearColor=FLinearColor::Black;
    InspectorWindowCamera=NewObject<USceneCaptureComponent2D>(this);
    InspectorWindowCamera->TextureTarget=InspectorWindowTarget;
    InspectorWindowCamera->CaptureSource=ESceneCaptureSource::SCS_FinalColorLDR;
    InspectorWindowCamera->FOVAngle=90;
    InspectorWindowCamera->bCaptureEveryFrame=true;
    InspectorWindowCamera->bCaptureOnMovement=false;
    InspectorWindowCamera->ShowFlags.SetMotionBlur(false);
    InspectorWindowCamera->RegisterComponentWithWorld(GetWorld());
    FVector Position; FRotator Rotation; GetPlayerViewPoint(Position,Rotation);
    InspectorWindowCamera->SetWorldLocationAndRotation(Position,Rotation);
    InspectorWindowCamera->CaptureScene();
    const TWeakObjectPtr<AMRFPSPlayerController> WeakThis(this);
    TSharedRef<SWindow> Window=SNew(SWindow)
        .Title(FText::FromString(TEXT("MRFPS - 観戦2")))
        .ClientSize(FVector2D(960,540))
        .SupportsMinimize(true).SupportsMaximize(true)
        [ SNew(SMRFPSInspectorVideo,WeakThis) ];
    Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([WeakThis](const TSharedRef<SWindow>& Closed) {
        if (auto* PC=WeakThis.Get()) if (PC->InspectorViewWindow.Pin()==Closed) {
            PC->InspectorViewWindow.Reset();
            if (PC->InspectorWindowCamera) PC->InspectorWindowCamera->DestroyComponent();
            PC->InspectorWindowCamera=nullptr; PC->InspectorWindowTarget=nullptr;
        }
    }));
    InspectorViewWindow=Window;
    FSlateApplication::Get().AddWindow(Window);
#endif
}

void AMRFPSPlayerController::CloseInspectorViewWindow()
{
#if !UE_SERVER && (PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX)
    auto Window=InspectorViewWindow.Pin(); InspectorViewWindow.Reset();
    if (Window) Window->RequestDestroyWindow();
    if (InspectorWindowCamera) InspectorWindowCamera->DestroyComponent();
    InspectorWindowCamera=nullptr; InspectorWindowTarget=nullptr;
#endif
}

void AMRFPSPlayerController::ClosePlayerVideoWindows()
{
#if !UE_SERVER && (PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX)
    for (auto& WeakWindow:PlayerVideoWindows) {
        if (auto Window=WeakWindow.Pin()) Window->RequestDestroyWindow();
        WeakWindow.Reset();
    }
#endif
}

void AMRFPSPlayerController::ClosePlayerVideoWindow(int32 Slot)
{
#if !UE_SERVER && (PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX)
    if (Slot<0 || Slot>=2) return;
    auto Window=PlayerVideoWindows[Slot].Pin(); PlayerVideoWindows[Slot].Reset();
    if (Window) Window->RequestDestroyWindow();
#endif
}

uint8 AMRFPSPlayerController::PlayerVideoWindowMask() const
{
    uint8 Mask=0;
#if !UE_SERVER && (PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX)
    for (int32 Slot=0;Slot<2;++Slot) if (PlayerVideoWindows[Slot].IsValid()) Mask|=(1u<<Slot);
#endif
    return Mask;
}
