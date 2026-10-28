#include "MRFPSAdminPanel.h"
#include "MRFPSGame.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/CoreStyle.h"
#include "String/LexFromString.h"
#include "Engine/World.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"

namespace
{
UTextBlock* Text(UWidgetTree* Tree,const FString& Caption,int32 Size=12)
{
    auto* Label=Tree->ConstructWidget<UTextBlock>();
    Label->SetText(FText::FromString(Caption)); Label->SetAutoWrapText(true);
    Label->SetColorAndOpacity(FLinearColor(.83,.91,1));
    Label->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),Size)); return Label;
}
UButton* Button(UWidgetTree* Tree,UVerticalBox* Body,const TCHAR* Caption)
{
    auto* Control=Tree->ConstructWidget<UButton>();
    FButtonStyle Style;
    Style.SetNormal(FSlateRoundedBoxBrush(FLinearColor(.045,.15,.29),5));
    Style.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.07,.27,.51),5));
    Style.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.025,.09,.18),5));
    Style.SetNormalPadding(FMargin(12,9)); Style.SetPressedPadding(FMargin(12,10,12,8)); Control->SetStyle(Style);
    Control->SetContent(Text(Tree,Caption)); Body->AddChildToVerticalBox(Control)->SetPadding(FMargin(0,3)); return Control;
}
UEditableTextBox* Number(UWidgetTree* Tree,UVerticalBox* Body,const TCHAR* Caption,const FString& Value)
{
    auto* Row=Tree->ConstructWidget<UHorizontalBox>();
    Row->AddChildToHorizontalBox(Text(Tree,Caption))->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    auto* Box=Tree->ConstructWidget<UEditableTextBox>(); Box->SetText(FText::FromString(Value));
    Box->SetMinDesiredWidth(110); Row->AddChildToHorizontalBox(Box);
    Body->AddChildToVerticalBox(Row)->SetPadding(FMargin(0,4)); return Box;
}
}

UMRFPSAdminPanel* UMRFPSAdminPanel::CreateFor(AMRFPSPlayerController* Owner)
{
    if (!Owner || !Owner->IsLocalController() || !Owner->IsAdmin()) return nullptr;
    auto* Panel=CreateWidget<UMRFPSAdminPanel>(Owner,StaticClass()); if (Panel) Panel->AddToViewport(110); return Panel;
}
AMRFPSPlayerController* UMRFPSAdminPanel::Controller() const
{ return Cast<AMRFPSPlayerController>(GetOwningPlayer()); }
void UMRFPSAdminPanel::NativeOnInitialized()
{
    Super::NativeOnInitialized(); SetIsFocusable(true);
    auto* Canvas=WidgetTree->ConstructWidget<UCanvasPanel>(); WidgetTree->RootWidget=Canvas;
    auto* Size=WidgetTree->ConstructWidget<USizeBox>(); Size->SetWidthOverride(460); Size->SetHeightOverride(660);
    auto* CanvasSlot=Canvas->AddChildToCanvas(Size); CanvasSlot->SetPosition(FVector2D(24,24)); CanvasSlot->SetAutoSize(true);
    auto* Surface=WidgetTree->ConstructWidget<UBorder>(); Surface->SetPadding(FMargin(20));
    Surface->SetBrush(FSlateRoundedBoxBrush(FLinearColor(.018,.031,.055,.98),9)); Size->SetContent(Surface);
    auto* Scroll=WidgetTree->ConstructWidget<UScrollBox>(); Surface->SetContent(Scroll);
    auto* Body=WidgetTree->ConstructWidget<UVerticalBox>(); Scroll->AddChild(Body);
    Body->AddChildToVerticalBox(Text(WidgetTree,TEXT("ADMIN / ARENA CONTROL"),20))->SetPadding(FMargin(0,0,0,10));
    Status=Text(WidgetTree,TEXT("Waiting for authoritative state")); Body->AddChildToVerticalBox(Status);
    Result=Text(WidgetTree,TEXT("")); Body->AddChildToVerticalBox(Result)->SetPadding(FMargin(0,5));
    Button(WidgetTree,Body,TEXT("START"))->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::Start);
    Button(WidgetTree,Body,TEXT("PAUSE COMBAT"))->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::Pause);
    Button(WidgetTree,Body,TEXT("RESUME COMBAT"))->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::Resume);
    Button(WidgetTree,Body,TEXT("RESET TO WAITING"))->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::Reset);
    Body->AddChildToVerticalBox(Text(WidgetTree,TEXT("PHYSICAL FLOOR / explicit operator registration"),14))->SetPadding(FMargin(0,15,0,4));
    Body->AddChildToVerticalBox(Text(WidgetTree,TEXT("First align both headsets at the same marked floor origin facing the same +X. Confirm only after checking both Stage floors. Independent headset origins are not automatically shared."),11));
    Button(WidgetTree,Body,TEXT("CONFIRM BOTH MANUAL ORIGINS / STAGE FLOOR"))->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::ConfirmSharedFloor);
    Button(WidgetTree,Body,TEXT("CALIBRATION / midpoint + Y heading NOW"))->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::CalibrateFloor);
    Body->AddChildToVerticalBox(Text(WidgetTree,TEXT("Fixed size; no fit, tracking scale or player teleport. Floor stays still until the next explicit calibration. Head heights never define the floor plane."),11));
    const auto* PC=Controller(); const auto* GS=PC?PC->GetWorld()->GetGameState<AMRFPSGameState>():nullptr;
    WidthBox=Number(WidgetTree,Body,TEXT("X width (metres)"),FString::SanitizeFloat(GS?GS->Layout.WidthCm/100:4.5));
    LengthBox=Number(WidgetTree,Body,TEXT("Y length (metres)"),FString::SanitizeFloat(GS?GS->Layout.LengthCm/100:7.));
    Button(WidgetTree,Body,TEXT("APPLY SIZE / waiting or paused only"))->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::SetDimensions);
    WallYawBox=Number(WidgetTree,Body,TEXT("Wall yaw relative to floor (degrees)"),TEXT("0"));
    Button(WidgetTree,Body,TEXT("PLACE WALLS / click 3D stage, Esc cancels"))->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::PlaceWalls);
    Button(WidgetTree,Body,TEXT("CLEAR ADMIN WALLS"))->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::ClearWalls);
    Body->AddChildToVerticalBox(Text(WidgetTree,TEXT("Tracking, video, authentication and UI remain live during combat pause. Resume does not arm MoBits."),11))->SetPadding(FMargin(0,9));
    Button(WidgetTree,Body,TEXT("CLOSE / F2 / Esc"))->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::Close);
}
void UMRFPSAdminPanel::NativeConstruct()
{
    Super::NativeConstruct();
    if (auto* PC=Controller(); PC && !bInputCaptured) {
        PC->FlushPressedKeys(); PC->StopHaptics(); PC->SetIgnoreMoveInput(true); PC->SetIgnoreLookInput(true); PC->bShowMouseCursor=true;
        FInputModeUIOnly Mode; Mode.SetWidgetToFocus(TakeWidget()); Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        PC->SetInputMode(Mode); bInputCaptured=true;
    }
    RefreshStatus();
}
void UMRFPSAdminPanel::ReleaseInput()
{
    if (!bInputCaptured) return; bInputCaptured=false;
    if (auto* PC=Controller()) {
        PC->SetIgnoreMoveInput(false); PC->SetIgnoreLookInput(false); PC->FlushPressedKeys();
        PC->SetInputMode(FInputModeGameOnly()); PC->bShowMouseCursor=false; PC->AdminPanelClosed();
    }
}
void UMRFPSAdminPanel::Close() { RemoveFromParent(); ReleaseInput(); }
void UMRFPSAdminPanel::NativeDestruct() { ReleaseInput(); Super::NativeDestruct(); }
FReply UMRFPSAdminPanel::NativeOnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& Key)
{
    if (Key.GetKey()==EKeys::Escape || Key.GetKey()==EKeys::F2) { Close(); return FReply::Handled(); }
    return Super::NativeOnPreviewKeyDown(Geometry,Key);
}
void UMRFPSAdminPanel::NativeTick(const FGeometry& Geometry,float DeltaSeconds)
{
    Super::NativeTick(Geometry,DeltaSeconds); Elapsed+=DeltaSeconds;
    if (Elapsed>=.2f) { Elapsed=0; RefreshStatus(); }
}
void UMRFPSAdminPanel::RefreshStatus()
{
    auto* PC=Controller(); if (!PC || !PC->IsAdmin()) { Close(); return; }
    const auto* GS=PC->GetWorld()->GetGameState<AMRFPSGameState>(); if (!GS || !Status) return;
    const TCHAR* Phase=GS->bCombatPaused?TEXT("PAUSED"):GS->Phase==EMRFPSMatchPhase::Playing?TEXT("PLAYING"):GS->Phase==EMRFPSMatchPhase::Finished?TEXT("FINISHED"):TEXT("WAITING");
    const TCHAR* Registration=GS->bNominalDesktopSession?TEXT("DESKTOP NOMINAL / no physical alignment"):GS->Layout.bCalibrated&&GS->bSharedFloorConfirmed?TEXT("PHYSICAL FLOOR CALIBRATED"):TEXT("PHYSICAL FLOOR UNAVAILABLE");
    Status->SetText(FText::FromString(FString::Printf(TEXT("%s  |  %s\nX %.2f m x Y %.2f m / layout %u / shared epoch %u\nMeasured anchor distance %.2f m (not fitted)"),
        Phase,Registration,GS->Layout.WidthCm/100,GS->Layout.LengthCm/100,GS->Layout.Revision,GS->SharedFloorRevision,GS->MeasuredAnchorDistanceCm/100)));
    Result->SetText(FText::FromString(PC->AdminStatus));
}
void UMRFPSAdminPanel::Start() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::Start); }
void UMRFPSAdminPanel::Pause() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::Pause); }
void UMRFPSAdminPanel::Resume() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::Resume); }
void UMRFPSAdminPanel::Reset() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::Reset); }
void UMRFPSAdminPanel::ConfirmSharedFloor() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::ConfirmSharedFloor); }
void UMRFPSAdminPanel::CalibrateFloor() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::CalibrateFloor); }
void UMRFPSAdminPanel::ClearWalls() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::ClearAdminWalls); }
void UMRFPSAdminPanel::SetDimensions()
{
    auto* PC=Controller(); if (!PC) return;
    double Width=0,Length=0;
    if (!WidthBox || !LengthBox || !LexTryParseString(Width,*WidthBox->GetText().ToString()) || !LexTryParseString(Length,*LengthBox->GetText().ToString()) ||
        !FMath::IsFinite(Width) || !FMath::IsFinite(Length)) { PC->AdminStatus=TEXT("LAYOUT_DIMENSIONS_INVALID"); return; }
    PC->RequestAdminAction(EMRFPSAdminAction::SetDimensions,FVector2D(Width*100,Length*100));
}
void UMRFPSAdminPanel::PlaceWalls()
{
    auto* PC=Controller(); if (!PC) return;
    double Yaw=0;
    if (!WallYawBox || !LexTryParseString(Yaw,*WallYawBox->GetText().ToString()) || !FMath::IsFinite(Yaw) || FMath::Abs(Yaw)>360) {
        PC->AdminStatus=TEXT("ADMIN_WALL_INVALID_YAW"); return;
    }
    PC->AdminWallYaw=Yaw; Close(); PC->SetAdminWallPlacement(true);
}
