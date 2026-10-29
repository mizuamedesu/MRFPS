#include "MRFPSMenu.h"
#include "MRFPSConnectionSettings.h"

#include "MRFPSGame.h"
#include "MRFPSHapticsComponent.h"
#include "MRFPSAuthClientSubsystem.h"
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
#include "Components/SizeBox.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"
#include "Misc/ConfigCacheIni.h"
#include "String/LexFromString.h"
#include "Styling/CoreStyle.h"
#include "Misc/Paths.h"

namespace MRFPSMenu
{
const FLinearColor Surface(.024f, .040f, .067f, 1.f);
const FLinearColor Secondary(.058f, .092f, .145f, 1.f);
const FLinearColor Blue(.035f, .265f, .79f, 1.f);
const FLinearColor White(.91f, .95f, 1.f, 1.f);
const FLinearColor Muted(.43f, .55f, .70f, 1.f);
const FLinearColor Highlight(.25f, .61f, 1.f, 1.f);

UTextBlock* Text(UWidgetTree* Tree, const FString& Value, int32 Size, FLinearColor Color, bool bBold = false)
{
    auto* Widget = Tree->ConstructWidget<UTextBlock>();
    Widget->SetText(FText::FromString(Value));
    Widget->SetFont(FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),Size));
    Widget->SetColorAndOpacity(FSlateColor(Color));
    return Widget;
}

void Space(UWidgetTree* Tree, UVerticalBox* Parent, float Height)
{
    auto* Widget = Tree->ConstructWidget<USizeBox>();
    Widget->SetHeightOverride(Height);
    Parent->AddChildToVerticalBox(Widget);
}

UButton* Button(UWidgetTree* Tree, const FName Name, const FString& Label, FLinearColor Color, int32 Size = 13)
{
    auto* Widget = Tree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
    FButtonStyle Style;
    Style.SetNormal(FSlateRoundedBoxBrush(Color, 6.f));
    Style.SetHovered(FSlateRoundedBoxBrush(FMath::Lerp(Color, Highlight, .23f), 6.f));
    Style.SetPressed(FSlateRoundedBoxBrush(FMath::Lerp(Color, FLinearColor::Black, .18f), 6.f));
    Style.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.035f, .052f, .077f, 1.f), 6.f));
    Style.SetNormalPadding(FMargin(12, 10));
    Style.SetPressedPadding(FMargin(12, 11, 12, 9));
    Widget->SetStyle(Style);
    auto* Caption = Text(Tree, Label, Size, White, true);
    Caption->SetJustification(ETextJustify::Center);
    Widget->SetContent(Caption);
    return Widget;
}

void RowButton(UHorizontalBox* Row, UButton* Widget, bool bLast = false)
{
    auto* Slot = Row->AddChildToHorizontalBox(Widget);
    Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    Slot->SetPadding(FMargin(0, 0, bLast ? 0 : 8, 0));
}

void Divider(UWidgetTree* Tree, UVerticalBox* Parent)
{
    Space(Tree, Parent, 17);
    auto* Size = Tree->ConstructWidget<USizeBox>();
    Size->SetHeightOverride(1);
    auto* Line = Tree->ConstructWidget<UBorder>();
    Line->SetBrush(FSlateColorBrush(FLinearColor(.080f, .13f, .20f, 1.f)));
    Line->SetPadding(FMargin(0));
    Size->SetContent(Line);
    Parent->AddChildToVerticalBox(Size);
    Space(Tree, Parent, 17);
}
}

UMRFPSMenu* UMRFPSMenu::CreateFor(AMRFPSPlayerController* Owner)
{
    if (!Owner || !Owner->IsLocalController()) return nullptr;
    auto* Menu = CreateWidget<UMRFPSMenu>(Owner, StaticClass());
    if (Menu) Menu->AddToViewport(100);
    return Menu;
}

AMRFPSPlayerController* UMRFPSMenu::Controller() const
{
    return Cast<AMRFPSPlayerController>(GetOwningPlayer());
}

void UMRFPSMenu::NativeOnInitialized()
{
    Super::NativeOnInitialized();
    SetIsFocusable(true);
    if (!WidgetTree) WidgetTree = NewObject<UWidgetTree>(this, TEXT("MenuWidgetTree"));
    if (WidgetTree->RootWidget) return;
    using namespace MRFPSMenu;

    auto* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("MenuRoot"));
    WidgetTree->RootWidget = Canvas;
    auto* Backdrop = WidgetTree->ConstructWidget<UBorder>();
    Backdrop->SetBrush(FSlateColorBrush(FLinearColor(.005f, .010f, .020f, .82f)));
    auto* BackdropSlot = Canvas->AddChildToCanvas(Backdrop);
    BackdropSlot->SetAnchors(FAnchors(0, 0, 1, 1));
    BackdropSlot->SetOffsets(FMargin(0));

    auto* Width = WidgetTree->ConstructWidget<USizeBox>();
    Width->SetWidthOverride(560);
    Width->SetHeightOverride(610);
    auto* PanelSlot = Canvas->AddChildToCanvas(Width);
    PanelSlot->SetAnchors(FAnchors(.5f, .5f));
    PanelSlot->SetAlignment(FVector2D(.5, .5));
    PanelSlot->SetAutoSize(true);
    auto* Panel = WidgetTree->ConstructWidget<UBorder>();
    Panel->SetBrush(FSlateRoundedBoxBrush(Surface, 12.f, FLinearColor(.09f, .20f, .35f, 1.f), 1.f));
    Panel->SetPadding(FMargin(28, 24));
    Width->SetContent(Panel);
    auto* Body = WidgetTree->ConstructWidget<UVerticalBox>();
    auto* Scroll=WidgetTree->ConstructWidget<UScrollBox>();
    Panel->SetContent(Scroll); Scroll->AddChild(Body);

    Body->AddChildToVerticalBox(Text(WidgetTree, TEXT("アリーナ"), 10, Highlight, true));
    Space(WidgetTree, Body, 4);
    auto* Header = WidgetTree->ConstructWidget<UHorizontalBox>();
    Body->AddChildToVerticalBox(Header);
    Header->AddChildToHorizontalBox(Text(WidgetTree, TEXT("MRFPS"), 34, White, true))->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    auto* CloseButton = Button(WidgetTree, TEXT("CloseMenu"), TEXT("閉じる"), Secondary, 10);
    auto* CloseSlot = Header->AddChildToHorizontalBox(CloseButton);
    CloseSlot->SetVerticalAlignment(VAlign_Center);
    CloseButton->OnClicked.AddDynamic(this, &UMRFPSMenu::Close);
    Space(WidgetTree, Body, 17);

    auto* StatusPanel = WidgetTree->ConstructWidget<UBorder>();
    StatusPanel->SetBrush(FSlateRoundedBoxBrush(FLinearColor(.037f, .065f, .105f, 1.f), 7.f));
    StatusPanel->SetPadding(FMargin(13, 10));
    auto* StatusBody = WidgetTree->ConstructWidget<UVerticalBox>();
    StatusPanel->SetContent(StatusBody);
    RoleText = Text(WidgetTree, TEXT("プレビュー"), 13, White, true);
    MatchText = Text(WidgetTree, TEXT("接続待ち"), 11, Muted);
    MatchText->SetAutoWrapText(true);
    StatusBody->AddChildToVerticalBox(RoleText);
    StatusBody->AddChildToVerticalBox(MatchText)->SetPadding(FMargin(0, 4, 0, 0));
    Body->AddChildToVerticalBox(StatusPanel);
    Space(WidgetTree, Body, 16);

    Body->AddChildToVerticalBox(Text(WidgetTree, TEXT("接続先"), 10, Muted, true));
    Space(WidgetTree, Body, 6);
    ServerBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("ServerAddress"));
    FEditableTextBoxStyle Field = FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>(TEXT("NormalEditableTextBox"));
    Field.SetFont(FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),15));
    Field.SetPadding(FMargin(12, 10));
    Field.SetForegroundColor(FSlateColor(White));
    Field.SetBackgroundImageNormal(FSlateRoundedBoxBrush(FLinearColor(.012f, .024f, .044f, 1.f), 6.f,
        FLinearColor(.10f, .16f, .25f, 1.f), 1.f));
    Field.SetBackgroundImageHovered(FSlateRoundedBoxBrush(FLinearColor(.016f, .032f, .057f, 1.f), 6.f, Highlight, 1.f));
    Field.SetBackgroundImageFocused(FSlateRoundedBoxBrush(FLinearColor(.016f, .032f, .057f, 1.f), 6.f, Highlight, 1.f));
    ServerBox->SetWidgetStyle(Field);
    ServerBox->SetHintText(FText::FromString(TEXT("サーバー名:ポート")));
    ServerBox->SetSelectAllTextWhenFocused(true);
    ServerBox->SetText(FText::FromString(MRFPSConnectionSettings::InitialServerAddress()));
    ServerBox->OnTextChanged.AddDynamic(this, &UMRFPSMenu::AddressChanged);
    Body->AddChildToVerticalBox(ServerBox);
    Space(WidgetTree,Body,8);
    AccessTokenBox=WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(),TEXT("AccessToken"));
    AccessTokenBox->SetWidgetStyle(Field); AccessTokenBox->SetIsPassword(true);
    AccessTokenBox->SetHintText(FText::FromString(TEXT("接続用トークン")));
    Body->AddChildToVerticalBox(AccessTokenBox);
    AuthenticationText=Text(WidgetTree,TEXT("認証情報を確認中"),10,Muted);
    AuthenticationText->SetAutoWrapText(true); Body->AddChildToVerticalBox(AuthenticationText);
    Space(WidgetTree, Body, 9);
    auto* JoinRow = WidgetTree->ConstructWidget<UHorizontalBox>();
    auto* PlayerJoin = Button(WidgetTree, TEXT("JoinPlayer"), TEXT("プレイ"), Blue);
    auto* InspectorJoin = Button(WidgetTree, TEXT("JoinInspector"), TEXT("観戦"), Secondary);
    auto* AdminJoin = Button(WidgetTree, TEXT("JoinAdmin"), TEXT("管理"), Secondary);
    PlayerJoin->OnClicked.AddDynamic(this, &UMRFPSMenu::JoinPlayer);
    InspectorJoin->OnClicked.AddDynamic(this, &UMRFPSMenu::JoinInspector);
    AdminJoin->OnClicked.AddDynamic(this, &UMRFPSMenu::JoinAdmin);
    RowButton(JoinRow, PlayerJoin);
    RowButton(JoinRow, InspectorJoin);
    RowButton(JoinRow, AdminJoin, true);
    Body->AddChildToVerticalBox(JoinRow);
    Space(WidgetTree,Body,8);
    auto* DeviceLoginButton=Button(WidgetTree,TEXT("OpenDeviceLogin"),TEXT("サーバー設定・デバイスログイン"),Secondary);
    DeviceLoginButton->OnClicked.AddDynamic(this,&UMRFPSMenu::OpenDeviceLogin);
    Body->AddChildToVerticalBox(DeviceLoginButton);

    Divider(WidgetTree, Body);
    Body->AddChildToVerticalBox(Text(WidgetTree, TEXT("プレイヤー"), 10, Muted, true));
    Space(WidgetTree, Body, 7);
    AlignButton = Button(WidgetTree, TEXT("AlignArena"), TEXT("原点登録"), Secondary);
    AlignButton->OnClicked.AddDynamic(this, &UMRFPSMenu::Align);
    Body->AddChildToVerticalBox(AlignButton);
    AlignButton->SetToolTipText(FText::FromString(TEXT("共有原点に立ち、決めた正面を向いて登録します")));
    Space(WidgetTree, Body, 12);
    auto* HapticsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
    ConnectButton = Button(WidgetTree, TEXT("ConnectMoBits"), TEXT("MoBits接続"), Secondary, 12);
    ArmButton = Button(WidgetTree, TEXT("ArmMoBits"), TEXT("有効化"), Blue, 12);
    StopButton = Button(WidgetTree, TEXT("StopMoBits"), TEXT("停止"), FLinearColor(.31f, .040f, .065f, 1.f), 12);
    ConnectButton->OnClicked.AddDynamic(this, &UMRFPSMenu::ConnectHaptics);
    ArmButton->OnClicked.AddDynamic(this, &UMRFPSMenu::ArmHaptics);
    StopButton->OnClicked.AddDynamic(this, &UMRFPSMenu::StopHaptics);
    ArmButton->SetToolTipText(FText::FromString(TEXT("装着・調整済みのMoBitsを有効にします。")));
    RowButton(HapticsRow, ConnectButton);
    RowButton(HapticsRow, ArmButton);
    RowButton(HapticsRow, StopButton, true);
    Body->AddChildToVerticalBox(HapticsRow);
    HapticsText = Text(WidgetTree, TEXT("MoBits 未接続"), 10, Muted);
    HapticsText->SetAutoWrapText(true);
    auto* HapticsHeight = WidgetTree->ConstructWidget<USizeBox>();
    HapticsHeight->SetMinDesiredHeight(30);
    HapticsHeight->SetContent(HapticsText);
    Body->AddChildToVerticalBox(HapticsHeight)->SetPadding(FMargin(0, 6, 0, 0));

    Space(WidgetTree, Body, 8);
    Body->AddChildToVerticalBox(Text(WidgetTree, TEXT("視点"), 10, Muted, true));
    Space(WidgetTree, Body, 7);
    auto* Cameras = WidgetTree->ConstructWidget<UHorizontalBox>();
    PlayerOneButton = Button(WidgetTree, TEXT("ViewPlayerOne"), TEXT("P1の映像"), Secondary, 12);
    PlayerTwoButton = Button(WidgetTree, TEXT("ViewPlayerTwo"), TEXT("P2の映像"), Secondary, 12);
    FreeButton = Button(WidgetTree, TEXT("ViewFree"), TEXT("自由視点"), Secondary, 12);
    PlayerOneButton->OnClicked.AddDynamic(this, &UMRFPSMenu::ViewPlayerOne);
    PlayerTwoButton->OnClicked.AddDynamic(this, &UMRFPSMenu::ViewPlayerTwo);
    FreeButton->OnClicked.AddDynamic(this, &UMRFPSMenu::ViewFree);
    RowButton(Cameras, PlayerOneButton);
    RowButton(Cameras, PlayerTwoButton);
    RowButton(Cameras, FreeButton, true);
    Body->AddChildToVerticalBox(Cameras);

    ErrorText = Text(WidgetTree, TEXT(""), 10, FLinearColor(1.f, .35f, .40f, 1.f));
    ErrorText->SetAutoWrapText(true);
    Body->AddChildToVerticalBox(ErrorText)->SetPadding(FMargin(0, 9, 0, 0));
    Space(WidgetTree, Body, 11);
    Body->AddChildToVerticalBox(Text(WidgetTree, TEXT("F1 メニュー   Esc 解除・停止"), 10, Muted));
}

void UMRFPSMenu::NativeConstruct()
{
    Super::NativeConstruct();
    if (auto* PC = Controller(); PC && !bInputCaptured)
    {
        PC->StopHaptics();
        if (auto* Pawn = PC->GetPawn<AMRFPSPawn>())
        {
            Pawn->SetFiring(false);
            Pawn->SetDesktopShield(false);
        }
        PC->FlushPressedKeys();
        PC->SetIgnoreMoveInput(true);
        PC->SetIgnoreLookInput(true);
        PC->bShowMouseCursor = true;
        FInputModeUIOnly Mode;
        Mode.SetWidgetToFocus(TakeWidget());
        Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        PC->SetInputMode(Mode);
        bInputCaptured = true;
    }
    RefreshStatus();
}

void UMRFPSMenu::ReleaseInput()
{
    if (!bInputCaptured) return;
    bInputCaptured = false;
    if (auto* PC = Controller())
    {
        PC->SetIgnoreMoveInput(false);
        PC->SetIgnoreLookInput(false);
        PC->FlushPressedKeys();
        PC->SetInputMode(FInputModeGameOnly());
        PC->bShowMouseCursor = false;
    }
}

void UMRFPSMenu::NativeDestruct()
{
    SaveAddress();
    ReleaseInput();
    Super::NativeDestruct();
}

void UMRFPSMenu::Close()
{
    SaveAddress();
    ReleaseInput();
    RemoveFromParent();
}

FReply UMRFPSMenu::NativeOnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Key)
{
    if (Key.GetKey() == EKeys::F1 || Key.GetKey() == EKeys::Escape)
    {
        if (Key.GetKey() == EKeys::Escape) if (auto* PC = Controller()) PC->StopHaptics();
        Close();
        return FReply::Handled();
    }
    return Super::NativeOnPreviewKeyDown(Geometry, Key);
}

void UMRFPSMenu::NativeTick(const FGeometry& Geometry, float DeltaTime)
{
    Super::NativeTick(Geometry, DeltaTime);
    RefreshElapsed += DeltaTime;
    if (RefreshElapsed >= .2f)
    {
        RefreshElapsed = 0;
        RefreshStatus();
    }
}

bool UMRFPSMenu::ReadAddress(FString& Address, FString& Error) const
{
    Address = ServerBox ? ServerBox->GetText().ToString().TrimStartAndEnd() : FString();
    if (Address.IsEmpty() || Address.Len() > 255)
    {
        Error = TEXT("接続先を入力してください");
        return false;
    }
    for (const TCHAR C : Address)
    {
        if (!FChar::IsAlnum(C) && C != '.' && C != ':' && C != '-' && C != '[' && C != ']')
        {
            Error = TEXT("接続先に空白やURLは使えません");
            return false;
        }
    }
    FString Host = Address;
    FString Port;
    if (Address.StartsWith(TEXT("[")))
    {
        int32 CloseBracket;
        if (!Address.FindChar(']', CloseBracket) || CloseBracket < 2)
        {
            Error = TEXT("IPv6は [アドレス]:ポート で入力してください");
            return false;
        }
        Host = Address.Mid(1, CloseBracket - 1);
        const FString Tail = Address.Mid(CloseBracket + 1);
        if (!Tail.IsEmpty())
        {
            if (!Tail.StartsWith(TEXT(":")))
            {
                Error = TEXT("ポート番号を確認してください");
                return false;
            }
            Port = Tail.Mid(1);
            if (Port.IsEmpty()) { Error = TEXT("ポート番号を入力してください"); return false; }
        }
    }
    else
    {
        int32 Colon;
        if (Address.FindChar(':', Colon))
        {
            Host = Address.Left(Colon);
            Port = Address.Mid(Colon + 1);
            if (Port.IsEmpty()) { Error = TEXT("ポート番号を入力してください"); return false; }
        }
    }
    if (Host.IsEmpty()) { Error = TEXT("接続先を入力してください"); return false; }
    if (!Port.IsEmpty())
    {
        int32 Number = 0;
        if (!Port.IsNumeric() || !LexTryParseString(Number, *Port) || Number < 1 || Number > 65535)
        {
            Error = TEXT("ポートは1〜65535で指定してください");
            return false;
        }
    }
    return true;
}

void UMRFPSMenu::SaveAddress()
{
    FString Address, Error;
    if (GConfig && ReadAddress(Address, Error))
    {
        GConfig->SetString(TEXT("MRFPS.Connection"), TEXT("Server"), *Address, GGameUserSettingsIni);
        GConfig->Flush(false, GGameUserSettingsIni);
    }
}

void UMRFPSMenu::Join(bool bInspector)
{ JoinRole(bInspector?TEXT("inspector"):TEXT("player")); }
void UMRFPSMenu::JoinRole(const FString& Role)
{
    FString Address;
    ActionError.Empty();
    if (!ReadAddress(Address, ActionError)) { RefreshStatus(); return; }
    if (auto* PC = Controller())
    {
          auto* Credentials=PC->GetGameInstance()->GetSubsystem<UMRFPSAuthClientSubsystem>();
          if (!Credentials) { ActionError=TEXT("AUTH_CONFIG"); RefreshStatus(); return; }
          if (AccessTokenBox && !AccessTokenBox->GetText().IsEmpty() && !Credentials->SetAccessToken(AccessTokenBox->GetText().ToString(),ActionError)) { RefreshStatus(); return; }
        SaveAddress();
        PC->ConnectionError.Empty();
        if (auto* Instance=PC->GetGameInstance<UMRFPSGameInstance>()) Instance->CancelNextDeviceConnection();
        if (Role==TEXT("admin")) PC->MRFPSConnectAdmin(Address);
        else PC->MRFPSConnect(Address,Role==TEXT("inspector"));
        if (PC->ConnectionError.IsEmpty()) Close();
        else RefreshStatus();
    }
}

void UMRFPSMenu::JoinPlayer() { Join(false); }
void UMRFPSMenu::JoinInspector() { Join(true); }
void UMRFPSMenu::JoinAdmin() { JoinRole(TEXT("admin")); }
void UMRFPSMenu::OpenDeviceLogin() { if (auto* PC=Controller()) PC->OpenDeviceLogin(); }
void UMRFPSMenu::Align() { if (auto* PC = Controller()) PC->Calibrate(); RefreshStatus(); }
void UMRFPSMenu::ArmHaptics() { if (auto* PC = Controller()) PC->ArmHaptics(); RefreshStatus(); }
void UMRFPSMenu::StopHaptics() { if (auto* PC = Controller()) PC->StopHaptics(); RefreshStatus(); }
void UMRFPSMenu::ViewPlayerOne() { if (auto* PC = Controller()) PC->MRFPSView(0); Close(); }
void UMRFPSMenu::ViewPlayerTwo() { if (auto* PC = Controller()) PC->MRFPSView(1); Close(); }
void UMRFPSMenu::ViewFree() { if (auto* PC = Controller()) PC->FreeView(); Close(); }

void UMRFPSMenu::ConnectHaptics()
{
    ActionError.Empty();
    if (auto* PC = Controller()) if (auto* Pawn = PC->GetPawn<AMRFPSPawn>(); Pawn && Pawn->Haptics)
        Pawn->Haptics->Connect(ActionError);
    RefreshStatus();
}

void UMRFPSMenu::AddressChanged(const FText& Value)
{
    ActionError.Empty();
    if (auto* PC = Controller()) PC->ConnectionError.Empty();
    RefreshStatus();
}

void UMRFPSMenu::RefreshStatus()
{
    auto* PC=Controller();if(!PC||!RoleText) return;
    auto* Pawn=PC->GetPawn<AMRFPSPawn>();const auto* Player=PC->GetPlayerState<AMRFPSPlayerState>();
    const auto* State=GetWorld()?GetWorld()->GetGameState<AMRFPSGameState>():nullptr;
    const bool Inspector=PC->IsInspector();
    if(AuthenticationText) if(auto* Credentials=PC->GetGameInstance()->GetSubsystem<UMRFPSAuthClientSubsystem>())
        AuthenticationText->SetText(FText::FromString(Credentials->HasAccessToken()?TEXT("認証情報を設定済み"):TEXT("接続用トークンを設定してください")));
    const FString Role=PC->GetNetMode()==NM_Standalone?TEXT("プレビュー"):PC->IsAdmin()?TEXT("管理者"):Inspector?TEXT("観戦"):FString::Printf(TEXT("プレイヤー%d"),Player?Player->Slot+1:0);
    RoleText->SetText(FText::FromString(Role));
    FString Match=TEXT("接続待ち");
    if(State&&PC->GetNetMode()!=NM_Standalone) {
        Match=State->bCombatPaused?TEXT("一時停止"):State->Phase==EMRFPSMatchPhase::Playing?TEXT("対戦中"):
            State->Phase==EMRFPSMatchPhase::BetweenRounds?TEXT("ラウンド間"):
            State->Phase==EMRFPSMatchPhase::Finished?(State->WinnerSlot<0?TEXT("引き分け"):FString::Printf(TEXT("P%dの勝利"),State->WinnerSlot+1)):TEXT("開始待ち");
        if(State->bSoloBotSession&&!State->bSoloGeometryReady) Match+=TEXT("  障害物スキャン未受信");
        if(Pawn&&!Pawn->bCalibrated) Match+=TEXT("  原点を登録してください");
    }
    MatchText->SetText(FText::FromString(Match));
    const bool Fighter=Pawn&&!Inspector;AlignButton->SetIsEnabled(Fighter);
    const auto* Haptics=Fighter?Pawn->Haptics.Get():nullptr;
    const auto Connection=Haptics?Haptics->GetConnectionState():EMoBitsConnectionState::Disconnected;
    const bool Connected=Connection==EMoBitsConnectionState::Connected;
    ConnectButton->SetIsEnabled(Haptics&&!Connected&&Connection!=EMoBitsConnectionState::Connecting);
    ArmButton->SetIsEnabled(Haptics&&Connected&&!Haptics->IsHapticsArmed());StopButton->SetIsEnabled(Fighter);
    const FString DeviceStatus=!Fighter?TEXT(""):Haptics&&Haptics->IsHapticsArmed()?TEXT("MoBits 有効"):Connected?TEXT("MoBits 接続済み"):Connection==EMoBitsConnectionState::Connecting?TEXT("MoBits 接続中"):TEXT("MoBits 未接続");
    HapticsText->SetText(FText::FromString(DeviceStatus));
    PlayerOneButton->SetIsEnabled(Inspector&&PC->FindPlayer(0));PlayerTwoButton->SetIsEnabled(Inspector&&PC->FindPlayer(1));FreeButton->SetIsEnabled(Inspector);
    FString Error=ActionError.IsEmpty()?PC->ConnectionError:ActionError;
    if(!Error.IsEmpty()&&(Error.StartsWith(TEXT("AUTH_"))||Error.StartsWith(TEXT("DTLS_")))) Error=TEXT("認証情報を確認して再接続してください");
    else if(ActionError.IsEmpty()&&!PC->ConnectionError.IsEmpty()) Error=TEXT("接続できません。接続先と認証情報を確認してください");
    ErrorText->SetText(FText::FromString(Error.Left(100)));
}
