#include "MRFPSAdminPanel.h"
#include "MRFPSGame.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/CoreStyle.h"
#include "Misc/Paths.h"
#include "String/LexFromString.h"
#include "Engine/World.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"
namespace {
FSlateFontInfo JP(int32 Size) { return FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),Size); }
UTextBlock* Text(UWidgetTree* T,const FString& V,int32 Size=15) {
    auto* W=T->ConstructWidget<UTextBlock>();W->SetText(FText::FromString(V));W->SetFont(JP(Size));W->SetColorAndOpacity(FLinearColor(.88,.94,1));return W;
}
UButton* Button(UWidgetTree* T,const TCHAR* Label,bool Accent=false) {
    auto* W=T->ConstructWidget<UButton>();FButtonStyle S;
    S.SetNormal(FSlateRoundedBoxBrush(Accent?FLinearColor(.06,.42,.88):FLinearColor(.07,.13,.22),6));
    S.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.13,.44,.7),6));S.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.03,.2,.4),6));
    S.SetNormalPadding(FMargin(12,11));S.SetPressedPadding(FMargin(12,12,12,10));W->SetStyle(S);
    auto* Caption=Text(T,Label);Caption->SetJustification(ETextJustify::Center);W->SetContent(Caption);return W;
}
void Add(UVerticalBox* Body,UWidget* W) { Body->AddChildToVerticalBox(W)->SetPadding(FMargin(0,4)); }
void Row(UWidgetTree* T,UVerticalBox* Body,std::initializer_list<UButton*> Buttons) {
    auto* H=T->ConstructWidget<UHorizontalBox>();for(auto* B:Buttons) { auto* Item=H->AddChildToHorizontalBox(B);Item->SetSize(FSlateChildSize(ESlateSizeRule::Fill));Item->SetPadding(FMargin(3,0)); }Add(Body,H);
}
UEditableTextBox* Number(UWidgetTree* T,UHorizontalBox* H,const TCHAR* Name,double V) {
    H->AddChildToHorizontalBox(Text(T,Name))->SetPadding(FMargin(5,8,8,0));auto* W=T->ConstructWidget<UEditableTextBox>();
    W->SetText(FText::FromString(FString::SanitizeFloat(V)));
    auto Field=FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>(TEXT("NormalEditableTextBox"));
    Field.SetFont(JP(16));Field.SetForegroundColor(FSlateColor(FLinearColor(.9f,.95f,1.f)));W->SetWidgetStyle(Field);
    W->SetMinDesiredWidth(80);W->SetSelectAllTextWhenFocused(true);H->AddChildToHorizontalBox(W)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));return W;
}
}
UMRFPSAdminPanel* UMRFPSAdminPanel::CreateFor(AMRFPSPlayerController* Owner) {
    if(!Owner||!Owner->IsLocalController()||!Owner->IsAdmin()) return nullptr;
    auto* P=CreateWidget<UMRFPSAdminPanel>(Owner,StaticClass());if(P) P->AddToViewport(110);return P;
}
AMRFPSPlayerController* UMRFPSAdminPanel::Controller() const { return Cast<AMRFPSPlayerController>(GetOwningPlayer()); }
void UMRFPSAdminPanel::NativeOnInitialized() {
    Super::NativeOnInitialized();SetIsFocusable(true);auto* Canvas=WidgetTree->ConstructWidget<UCanvasPanel>();WidgetTree->RootWidget=Canvas;
    auto* Size=WidgetTree->ConstructWidget<USizeBox>();Size->SetWidthOverride(520);Size->SetHeightOverride(570);
    auto* PanelSlot=Canvas->AddChildToCanvas(Size);PanelSlot->SetPosition(FVector2D(24,24));PanelSlot->SetAutoSize(true);
    auto* Surface=WidgetTree->ConstructWidget<UBorder>();Surface->SetPadding(FMargin(20));Surface->SetBrush(FSlateRoundedBoxBrush(FLinearColor(.018,.03,.055,.98),10));Size->SetContent(Surface);
    auto* Body=WidgetTree->ConstructWidget<UVerticalBox>();Surface->SetContent(Body);Add(Body,Text(WidgetTree,TEXT("アリーナ管理"),23));
    Status=Text(WidgetTree,TEXT("待機中"));Add(Body,Status);Result=Text(WidgetTree,TEXT(""),13);Add(Body,Result);
    auto* StartB=Button(WidgetTree,TEXT("開始"),true);StartB->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::Start);
    auto* PauseB=Button(WidgetTree,TEXT("一時停止"));PauseB->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::Pause);
    auto* ResumeB=Button(WidgetTree,TEXT("再開"));ResumeB->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::Resume);Row(WidgetTree,Body,{StartB,PauseB,ResumeB});
    auto* BuildB=Button(WidgetTree,TEXT("建築モード  B"),true);BuildB->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::PlaceWalls);Add(Body,BuildB);
    auto* OriginB=Button(WidgetTree,TEXT("原点登録"));OriginB->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::ConfirmSharedFloor);
    OriginB->SetToolTipText(FText::FromString(TEXT("2台を同じ床の原点に合わせ、同じ正面方向を向けてから登録します。")));
    auto* CalibrateB=Button(WidgetTree,TEXT("床の位置調整"));CalibrateB->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::CalibrateFloor);
    CalibrateB->SetToolTipText(FText::FromString(TEXT("現在の2人の中点と向きへ床を合わせます。大きさは変えません。")));Row(WidgetTree,Body,{OriginB,CalibrateB});
    auto* Dimensions=WidgetTree->ConstructWidget<UHorizontalBox>();const auto* PC=Controller();const auto* GS=PC?PC->GetWorld()->GetGameState<AMRFPSGameState>():nullptr;
    WidthBox=Number(WidgetTree,Dimensions,TEXT("幅m"),GS?GS->Layout.WidthCm/100:4.5);LengthBox=Number(WidgetTree,Dimensions,TEXT("長さm"),GS?GS->Layout.LengthCm/100:7.);Add(Body,Dimensions);
    auto* SizeB=Button(WidgetTree,TEXT("サイズ変更"));SizeB->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::SetDimensions);Add(Body,SizeB);
    auto* ClearB=Button(WidgetTree,TEXT("建築を消す"));ClearB->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::ClearWalls);
    auto* ResetB=Button(WidgetTree,TEXT("試合リセット"));ResetB->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::Reset);
    auto* ResetBuildingsB=Button(WidgetTree,TEXT("建築リセット"));ResetBuildingsB->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::ResetBuildings);
    ResetBuildingsB->SetToolTipText(FText::FromString(TEXT("追加した建築を消し、壁と箱を初期配置に戻します。")));Row(WidgetTree,Body,{ClearB,ResetBuildingsB,ResetB});
    auto* CloseB=Button(WidgetTree,TEXT("閉じる"));CloseB->OnClicked.AddDynamic(this,&UMRFPSAdminPanel::Close);Add(Body,CloseB);
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
    auto* PC=Controller();if(!PC||!PC->IsAdmin()) { Close();return; }
    const auto* GS=PC->GetWorld()->GetGameState<AMRFPSGameState>();if(!GS||!Status) return;
    const TCHAR* Phase=GS->bCombatPaused?TEXT("一時停止"):GS->Phase==EMRFPSMatchPhase::Playing?TEXT("対戦中"):
        GS->Phase==EMRFPSMatchPhase::BetweenRounds?TEXT("ラウンド間"):GS->Phase==EMRFPSMatchPhase::Finished?TEXT("終了"):TEXT("待機中");
    Status->SetText(FText::FromString(FString::Printf(TEXT("%s   %.1f × %.1fm"),Phase,GS->Layout.WidthCm/100,GS->Layout.LengthCm/100)));
    Result->SetText(FText::FromString(PC->AdminFeedback()));
}
void UMRFPSAdminPanel::Start() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::Start); }
void UMRFPSAdminPanel::Pause() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::Pause); }
void UMRFPSAdminPanel::Resume() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::Resume); }
void UMRFPSAdminPanel::Reset() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::Reset); }
void UMRFPSAdminPanel::ConfirmSharedFloor() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::ConfirmSharedFloor); }
void UMRFPSAdminPanel::CalibrateFloor() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::CalibrateFloor); }
void UMRFPSAdminPanel::ClearWalls() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::ClearAdminWalls); }
void UMRFPSAdminPanel::ResetBuildings() { if (auto* PC=Controller()) PC->RequestAdminAction(EMRFPSAdminAction::ResetBuildings); }
void UMRFPSAdminPanel::SetDimensions()
{
    auto* PC=Controller(); if (!PC) return;
    double Width=0,Length=0;
    if (!WidthBox || !LengthBox || !LexTryParseString(Width,*WidthBox->GetText().ToString()) || !LexTryParseString(Length,*LengthBox->GetText().ToString()) ||
        !FMath::IsFinite(Width) || !FMath::IsFinite(Length)) { PC->SetAdminFeedback(TEXT("LAYOUT_DIMENSIONS_INVALID")); return; }
    PC->RequestAdminAction(EMRFPSAdminAction::SetDimensions,FVector2D(Width*100,Length*100));
}
void UMRFPSAdminPanel::PlaceWalls() { if(auto* PC=Controller()) { Close();PC->SetAdminWallPlacement(true); } }
