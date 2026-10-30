#include "MRFPSLoginMenu.h"
#include "MRFPSDeviceAuthUrl.h"
#include "MRFPSConnectionSettings.h"
#include "MRFPSLoginInteraction.h"
#include "MRFPSLoginWidgetComponent.h"
#include "MRFPSRetryAfter.h"

#include "MRFPSGame.h"
#include "MRFPSAuthClientSubsystem.h"
#include "MRFPSGloveSubsystem.h"
#include "Engine/GameInstance.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Camera/CameraComponent.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/WidgetComponent.h"
#include "Components/WidgetInteractionComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "HttpModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MotionControllerComponent.h"
#include "Materials/MaterialInterface.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSLogin,Log,All);

namespace
{
const FLinearColor Surface(.024f,.040f,.067f,1), White(.91f,.95f,1,1), Muted(.5f,.62f,.76f,1), Blue(.035f,.265f,.79f,1);
constexpr const TCHAR* AuthUrl=TEXT("https://mrfps-admin.mizuame.app/api/device/exchange");

UTextBlock* Text(UWidgetTree* Tree,const FString& Value,int32 Size,FLinearColor Color)
{
    auto* T=Tree->ConstructWidget<UTextBlock>(); T->SetText(FText::FromString(Value));
    T->SetFont(FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),Size));
    T->SetColorAndOpacity(FSlateColor(Color)); T->SetAutoWrapText(true); return T;
}
void StyleButton(UButton* B,FLinearColor Color)
{
    FButtonStyle Style;
    Style.SetNormal(FSlateRoundedBoxBrush(Color,6));
    Style.SetHovered(FSlateRoundedBoxBrush(FMath::Lerp(Color,FLinearColor(.25f,.61f,1,1),.3f),6));
    Style.SetPressed(FSlateRoundedBoxBrush(FMath::Lerp(Color,FLinearColor::Black,.2f),6));
    Style.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.035f,.052f,.077f,1),6));
    Style.SetNormalPadding(FMargin(8,6)); Style.SetPressedPadding(FMargin(8,7,8,5)); B->SetStyle(Style);
}
UButton* Button(UWidgetTree* Tree,const FString& Name,const FString& Label,FLinearColor Color)
{
    auto* B=Tree->ConstructWidget<UButton>(UButton::StaticClass(),FName(*Name)); StyleButton(B,Color);
    auto* Caption=Text(Tree,Label,17,White); Caption->SetJustification(ETextJustify::Center); B->SetContent(Caption); return B;
}
void AddSpace(UWidgetTree* Tree,UVerticalBox* Parent,float Height)
{
    auto* Box=Tree->ConstructWidget<USizeBox>(); Box->SetHeightOverride(Height); Parent->AddChild(Box);
}
void AddKeyRow(UWidgetTree* Tree,UVerticalBox* Page,UMRFPSLoginMenu* Menu,const TArray<FString>& Keys)
{
    auto* Row=Tree->ConstructWidget<UHorizontalBox>(); Page->AddChild(Row);
    for (const auto& Value:Keys) {
        auto* Key=Tree->ConstructWidget<UMRFPSLoginKey>(); Key->KeyValue=Value; Key->Login=Menu;
        StyleButton(Key,FLinearColor(.058f,.092f,.145f,1));
        auto* Caption=Text(Tree,Value==TEXT("BACK")?TEXT("⌫"):Value,17,White);
        Caption->SetJustification(ETextJustify::Center); Key->SetContent(Caption);
        Key->OnClicked.AddDynamic(Key,&UMRFPSLoginKey::PressedKey);
        auto* Slot=Row->AddChildToHorizontalBox(Key); Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); Slot->SetPadding(FMargin(2));
    }
}
TArray<FString> Characters(const TCHAR* Chars)
{
    TArray<FString> Result; for (const TCHAR* C=Chars;*C;++C) Result.Add(FString::Chr(*C)); return Result;
}
}

void UMRFPSLoginKey::PressedKey() { if (Login.IsValid()) Login->InsertKey(KeyValue); }

UMRFPSLoginMenu* UMRFPSLoginMenu::CreateFor(AMRFPSPlayerController* Owner)
{
    if (!Owner || !Owner->IsLocalController()) return nullptr;
    auto* Menu=CreateWidget<UMRFPSLoginMenu>(Owner,StaticClass()); if (!Menu) return nullptr;
    Menu->Controller=Owner; Menu->bOpen=true;
#if PLATFORM_ANDROID
    Menu->bVR=true;
#else
    Menu->bVR=UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled();
#endif
    if (Menu->bVR) {
        auto* Pawn=Owner->GetPawn<AMRFPSPawn>();
        if (!Pawn || !Pawn->Camera || !Pawn->XROrigin) { Menu->bOpen=false; return nullptr; }
        if (auto* GI=Cast<UMRFPSGameInstance>(Owner->GetGameInstance())) GI->FireTransition.BeginLogin();
        auto* Component=NewObject<UMRFPSLoginWidgetComponent>(Pawn);
        Component->SetWidgetSpace(EWidgetSpace::World); Component->SetDrawSize(FVector2D(900,720));
        Component->SetPivot(FVector2D(.5f,.5f)); Component->SetTwoSided(true);
        Component->SetTickWhenOffscreen(true);
        Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Component->SetCollisionResponseToAllChannels(ECR_Ignore); Component->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);
        Component->SetupAttachment(Pawn->Camera); Component->SetRelativeLocation(FVector(115,0,-5));
        Component->SetRelativeRotation(FRotator(0,180,0)); Component->SetRelativeScale3D(FVector(.1));
        Component->RegisterComponent(); Menu->WorldWidget=Component; Component->SetWidget(Menu);
        auto* Pointer=NewObject<UMRFPSLoginInteraction>(Pawn);
        Pointer->SetupAttachment(Pawn->XROrigin); Pointer->InteractionDistance=400;
        Pointer->TraceChannel=ECC_Visibility;
        Pointer->SetLoginWorldWidget(Component);
        Pointer->RegisterComponent(); Pointer->SetComponentTickEnabled(false); Menu->Interaction=Pointer;
        // Engine debug lines are compiled out in Shipping. These tiny,
        // collision-free meshes remain visible to the local user in both
        // Development and Shipping builds and do not affect widget hit tests.
        if (auto* Cylinder=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cylinder.Cylinder"))) {
            auto* Ray=NewObject<UStaticMeshComponent>(Pawn); Ray->SetStaticMesh(Cylinder);
            Ray->SetupAttachment(Pointer); Ray->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Ray->SetCastShadow(false); Ray->SetRelativeRotation(FRotator(90,0,0));
            if (auto* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Tracer.M_Tracer"))) Ray->SetMaterial(0,Material);
            Ray->SetRelativeLocation(FVector(200,0,0)); Ray->SetRelativeScale3D(FVector(.014,.014,4));
            Ray->SetVisibility(false);
            Ray->RegisterComponent(); Menu->PointerRay=Ray;
        }
        if (auto* Sphere=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Sphere.Sphere"))) {
            auto* Dot=NewObject<UStaticMeshComponent>(Pawn); Dot->SetStaticMesh(Sphere);
            Dot->SetupAttachment(Pointer); Dot->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Dot->SetCastShadow(false); Dot->SetRelativeScale3D(FVector(.035));
            if (auto* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Tracer.M_Tracer"))) Dot->SetMaterial(0,Material);
            Dot->SetVisibility(false); Dot->RegisterComponent(); Menu->PointerDot=Dot;
        }
        UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_INIT interaction=%d ray=%d dot=%d"),
            Menu->Interaction!=nullptr,Menu->PointerRay!=nullptr,Menu->PointerDot!=nullptr);
    } else {
        if (auto* GI=Cast<UMRFPSGameInstance>(Owner->GetGameInstance())) GI->FireTransition.BeginLogin();
        Menu->AddToViewport(120);
    }
    return Menu;
}

FString UMRFPSLoginMenu::ControlPath()
{
#if UE_SERVER
    return FString();
#elif PLATFORM_ANDROID
    const FString Dir=UMRFPSAuthClientSubsystem::PrivateDirectory();
    return Dir.IsEmpty()?FString():FPaths::Combine(Dir,TEXT("device-control.json"));
#else
    FString Name=TEXT("MRFPS-Client"); FParse::Value(FCommandLine::Get(),TEXT("MRFPSWindowName="),Name);
    if (Name.IsEmpty() || Name.Len()>64) return FString();
    for (TCHAR C:Name) if (!((C>='A' && C<='Z') || (C>='a' && C<='z') || (C>='0' && C<='9') || C=='_' || C=='-')) return FString();
    FString Dir;
#if PLATFORM_WINDOWS
    Dir=FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
#elif PLATFORM_MAC
    Dir=FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
    if (!Dir.IsEmpty()) Dir=FPaths::Combine(Dir,TEXT("Library/Application Support"));
#else
    Dir=FPlatformMisc::GetEnvironmentVariable(TEXT("XDG_STATE_HOME"));
    if (Dir.IsEmpty()) { Dir=FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")); if (!Dir.IsEmpty()) Dir=FPaths::Combine(Dir,TEXT(".local/state")); }
#endif
    return Dir.IsEmpty() || FPaths::IsRelative(Dir)?FString():FPaths::Combine(Dir,TEXT("MRFPS/DeviceLogin/controls"),Name+TEXT(".json"));
#endif
}

void UMRFPSLoginMenu::NativeOnInitialized()
{
    Super::NativeOnInitialized(); SetIsFocusable(true);
    if (!WidgetTree) WidgetTree=NewObject<UWidgetTree>(this,TEXT("LoginWidgetTree"));
    if (WidgetTree->RootWidget) return;
    auto* Root=WidgetTree->ConstructWidget<UBorder>();
    Root->SetBrush(FSlateRoundedBoxBrush(Surface,10)); Root->SetPadding(FMargin(24,18)); WidgetTree->RootWidget=Root;
    auto* Outer=WidgetTree->ConstructWidget<UScrollBox>(); Root->SetContent(Outer);
    auto* Body=WidgetTree->ConstructWidget<UVerticalBox>(); Outer->AddChild(Body);
    Heading=Text(WidgetTree,TEXT("サーバー設定"),28,White); Body->AddChild(Heading); AddSpace(WidgetTree,Body,12);
    Status=Text(WidgetTree,TEXT("接続先を確認してください"),16,Muted); Body->AddChild(Status); AddSpace(WidgetTree,Body,8);
    PointerHint=Text(WidgetTree,TEXT("コントローラーを確認"),14,Muted); Body->AddChild(PointerHint);
    AddressPage=WidgetTree->ConstructWidget<UVerticalBox>(); Body->AddChild(AddressPage);
    auto Field=FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>(TEXT("NormalEditableTextBox"));
    Field.SetFont(FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),20));
    Field.SetPadding(FMargin(10,8)); Field.SetForegroundColor(FSlateColor(White));
    AddressBox=WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(),TEXT("ServerAddress"));
    AddressBox->SetWidgetStyle(Field); AddressBox->SetHintText(FText::FromString(TEXT("サーバー名:ポート")));
    AddressBox->SetText(FText::FromString(MRFPSConnectionSettings::InitialServerAddress()));
    AddressPage->AddChild(AddressBox); AddSpace(WidgetTree,AddressPage,8);
    auto* NextButton=Button(WidgetTree,TEXT("ServerNext"),TEXT("次へ"),Blue);
    NextButton->OnClicked.AddDynamic(this,&UMRFPSLoginMenu::Next); AddressPage->AddChild(NextButton);
    AddSpace(WidgetTree,AddressPage,8);
    // Includes every character accepted by the endpoint validator; the full
    // keyboard also works when Quest's native virtual keyboard is unavailable.
    AddKeyRow(WidgetTree,AddressPage,this,Characters(TEXT("1234567890")));
    AddKeyRow(WidgetTree,AddressPage,this,Characters(TEXT("qwertyuiop")));
    AddKeyRow(WidgetTree,AddressPage,this,Characters(TEXT("asdfghjkl")));
    AddKeyRow(WidgetTree,AddressPage,this,Characters(TEXT("zxcvbnm")));
    AddKeyRow(WidgetTree,AddressPage,this,{TEXT("."),TEXT("-"),TEXT(":"),TEXT("["),TEXT("]"),TEXT("BACK")});

    LoginPage=WidgetTree->ConstructWidget<UVerticalBox>(); Body->AddChild(LoginPage);
    auto* RoleRow=WidgetTree->ConstructWidget<UHorizontalBox>(); LoginPage->AddChild(RoleRow);
    auto* P=Button(WidgetTree,TEXT("RolePlayer"),TEXT("プレイヤー"),Blue);
    auto* I=Button(WidgetTree,TEXT("RoleInspector"),TEXT("観戦"),Blue);
    auto* A=Button(WidgetTree,TEXT("RoleAdmin"),TEXT("管理"),Blue);
    P->OnClicked.AddDynamic(this,&UMRFPSLoginMenu::SelectPlayer);
    I->OnClicked.AddDynamic(this,&UMRFPSLoginMenu::SelectInspector);
    A->OnClicked.AddDynamic(this,&UMRFPSLoginMenu::SelectAdmin);
    for (auto* ButtonValue:{P,I,A}) { auto* RowSlot=RoleRow->AddChildToHorizontalBox(ButtonValue); RowSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); RowSlot->SetPadding(FMargin(2)); }
    RoleLabel=Text(WidgetTree,TEXT("プレイヤー"),16,Muted); LoginPage->AddChild(RoleLabel); AddSpace(WidgetTree,LoginPage,6);
    CodeBox=WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(),TEXT("DeviceCode"));
    CodeBox->SetWidgetStyle(Field); CodeBox->SetHintText(FText::FromString(TEXT("6桁のコード")));
    CodeBox->SetIsPassword(true); LoginPage->AddChild(CodeBox); AddSpace(WidgetTree,LoginPage,6);
    AddKeyRow(WidgetTree,LoginPage,this,Characters(TEXT("123")));
    AddKeyRow(WidgetTree,LoginPage,this,Characters(TEXT("456")));
    AddKeyRow(WidgetTree,LoginPage,this,Characters(TEXT("789")));
    AddKeyRow(WidgetTree,LoginPage,this,{TEXT("0"),TEXT("BACK")}); AddSpace(WidgetTree,LoginPage,6);
    auto* LoginRow=WidgetTree->ConstructWidget<UHorizontalBox>(); LoginPage->AddChild(LoginRow);
    auto* BackButton=Button(WidgetTree,TEXT("LoginBack"),TEXT("戻る"),FLinearColor(.058f,.092f,.145f,1));
    auto* LoginButton=Button(WidgetTree,TEXT("DeviceLogin"),TEXT("ログイン"),Blue);
    BackButton->OnClicked.AddDynamic(this,&UMRFPSLoginMenu::Back); LoginButton->OnClicked.AddDynamic(this,&UMRFPSLoginMenu::Exchange);
    for (auto* ButtonValue:{BackButton,LoginButton}) { auto* RowSlot=LoginRow->AddChildToHorizontalBox(ButtonValue); RowSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); RowSlot->SetPadding(FMargin(2)); }
    TrustButton=Button(WidgetTree,TEXT("ResetIssuerTrust"),TEXT("保存済みの公開鍵を解除"),FLinearColor(.31f,.04f,.065f,1));
    TrustButton->OnClicked.AddDynamic(this,&UMRFPSLoginMenu::ResetTrust); LoginPage->AddChild(TrustButton);
    TrustButton->SetVisibility(ESlateVisibility::Collapsed); SetStage(false);
}

void UMRFPSLoginMenu::NativeConstruct()
{
    Super::NativeConstruct();
    FString InputMode;
    if (FParse::Value(FCommandLine::Get(),TEXT("MRFPSLoginInput="),InputMode)) {
        if (InputMode==TEXT("quest_axis")) PointerInput.Mode=MRFPSLoginPointer::EMode::QuestAxis;
        else if (InputMode==TEXT("quest_button")) PointerInput.Mode=MRFPSLoginPointer::EMode::QuestButton;
        else if (InputMode==TEXT("glove_controller")) PointerInput.Mode=MRFPSLoginPointer::EMode::GloveController;
        else if (InputMode==TEXT("glove_flex")) PointerInput.Mode=MRFPSLoginPointer::EMode::GloveFlex;
        else if (InputMode!=TEXT("auto")) UE_LOG(LogMRFPSLogin,Warning,TEXT("Unknown MRFPSLoginInput; using auto"));
    }
    if (auto* PC=Controller.Get()) {
        PC->StopHaptics(); PC->FlushPressedKeys(); PC->SetIgnoreMoveInput(true); PC->SetIgnoreLookInput(true);
        if (bVR) { PC->bShowMouseCursor=false; PC->SetInputMode(FInputModeGameOnly()); }
        else { PC->bShowMouseCursor=true; FInputModeUIOnly Mode; Mode.SetWidgetToFocus(TakeWidget()); Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock); PC->SetInputMode(Mode); }
    }
    if (PointerHint) PointerHint->SetVisibility(bVR?ESlateVisibility::HitTestInvisible:ESlateVisibility::Collapsed);
    if (bVR) {
        FocusLostHandle=FCoreDelegates::ApplicationWillDeactivateDelegate.AddUObject(this,&UMRFPSLoginMenu::HandleFocusLost);
        FocusGainedHandle=FCoreDelegates::ApplicationHasReactivatedDelegate.AddUObject(this,&UMRFPSLoginMenu::HandleFocusGained);
        BackgroundHandle=FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this,&UMRFPSLoginMenu::HandleFocusLost);
        ForegroundHandle=FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this,&UMRFPSLoginMenu::HandleFocusGained);
    }
}
void UMRFPSLoginMenu::NativeDestruct()
{
    if (!bClosing) CloseLogin();
    Super::NativeDestruct();
}
#if WITH_DEV_AUTOMATION_TESTS
void UMRFPSLoginMenu::NativeTick(const FGeometry& Geometry,float DeltaTime)
{
    Super::NativeTick(Geometry,DeltaTime);
    if (TestDuringPaint) TestDuringPaint();
}
#endif
bool UMRFPSLoginMenu::ResolvePointerWeapon(AMRFPSPawn* Pawn,FMRFPSWeaponPose& Weapon)
{
#if WITH_DEV_AUTOMATION_TESTS
    if (bUseTestWeapon) { Weapon=TestWeapon; return Weapon.Source!=MRFPSLoginPointer::ESource::None; }
#endif
    return Pawn && Pawn->GetLocalWeaponPose(Weapon);
}
void UMRFPSLoginMenu::TickXRPointerAfterPaint(UWidgetComponent* PaintedComponent,float DeltaTime)
{
#if !UE_SERVER
    auto* PC=Controller.Get();
    if (bVR && bOpen && !bClosing && Interaction && WorldWidget==PaintedComponent && IsValid(PC) &&
        !PC->IsActorBeingDestroyed() && PC->DeviceLogin==this && PC->GetWorld() && !PC->GetWorld()->bIsTearingDown) {
        const double Now=FPlatformTime::Seconds();
        if (!bPointerGridReady) UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_GRID ready=1"));
        bPointerGridReady=true; LastPointerPaint=Now;
        if (LastPointerTick>=0 && Now-LastPointerTick>=.75) {
            // Pose and focus continuity during a suspended frame are unknown.
            // Do not replay those edges against the current widget position.
            Interaction->CancelPointer(); PointerInput.CancelActive(); PointerInput.Disarm();
            PendingPointerEvents.Clear(); PendingPointerTargets.Reset(); LastPointerTick=Now; bGloveCursorInitialized=false; PreviousPointerHit.Reset();
            UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_GAP cancelled=1"));
        }
        UpdateXRPointer(DeltaTime);
    }
#endif
}
void UMRFPSLoginMenu::TickControlFile()
{
#if !UE_SERVER
    if (!bOpen || bClosing || bExchanging) return;
    auto* PC=Controller.Get();
    if (!IsValid(PC) || PC->IsActorBeingDestroyed() || PC->DeviceLogin!=this || !PC->IsLocalController()) return;
    UWorld* World=PC->GetWorld();
    if (!World || World!=GetWorld() || World->bIsTearingDown || PC->GetNetMode()!=NM_Standalone) return;
    const FWorldContext* Context=GEngine?GEngine->GetWorldContextFromWorld(World):nullptr;
    if (!Context || !Context->TravelURL.IsEmpty() || Context->PendingNetGame || World->IsInSeamlessTravel()) return;
    const double Now=FPlatformTime::Seconds();
    if (Now<NextControlCheck) return;
    NextControlCheck=Now+.5;
    ProcessControlFile();
#endif
}
void UMRFPSLoginMenu::UpdateXRPointer(float DeltaTime)
{
#if !UE_SERVER
    auto* PC=Controller.Get(); auto* Pawn=PC?PC->GetPawn<AMRFPSPawn>():nullptr;
    FMRFPSWeaponPose Weapon;
    const bool HasWeapon=ResolvePointerWeapon(Pawn,Weapon);
    auto Source=HasWeapon?Weapon.Source:MRFPSLoginPointer::ESource::None;
    bool UsesVRFocus=false,HasVRFocus=true;
    UHeadMountedDisplayFunctionLibrary::GetVRFocusState(UsesVRFocus,HasVRFocus);
    if (!PointerInput.Focused || (UsesVRFocus && !HasVRFocus)) Source=MRFPSLoginPointer::ESource::None;
    if (!bPointerPoseReported || Source!=PointerInput.Source) {
        if (Source!=PointerInput.Source) {
            Interaction->CancelPointer(); PointerInput.CancelActive(); PendingPointerEvents.Clear(); PendingPointerTargets.Reset(); LastPointerTick=FPlatformTime::Seconds();
            bGloveCursorInitialized=false; PreviousPointerHit.Reset(); bInputAnnounced=false;
        }
        PointerInput.SetSource(Source);
        bPointerPoseReported=true;
        if (PointerHint) PointerHint->SetText(FText::FromString(
            Source==MRFPSLoginPointer::ESource::Aim || Source==MRFPSLoginPointer::ESource::AimGrip?TEXT("右コントローラー・トリガーで選択"):
            Source==MRFPSLoginPointer::ESource::Grip?TEXT("右コントローラー・仮の向き"):
            Source==MRFPSLoginPointer::ESource::Hand?TEXT("右手追跡・トリガーで選択"):TEXT("コントローラーを確認")));
        UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_POSE source=%s weapon=%d assigned=%d"),
            MRFPSLoginPointer::SourceName(Source),HasWeapon,Pawn && Pawn->GetSlot()>=0);
    }
    if (Source!=MRFPSLoginPointer::ESource::None) {
        const FTransform Muzzle=MRFPSWeaponPose::Muzzle(Weapon.GripWorld);
        Pawn->SetLoginWeaponPose(&Weapon);
        Interaction->SetWorldLocationAndRotation(Muzzle.GetLocation(),Muzzle.GetRotation());
        Interaction->RefreshPointer();
    } else if (Pawn) Pawn->SetLoginWeaponPose(nullptr);
    const FHitResult RawHit=Interaction->GetLastHitResult();
    const bool RawWidget=Source!=MRFPSLoginPointer::ESource::None && RawHit.bBlockingHit && RawHit.GetComponent()==WorldWidget.Get();
    const auto CurrentHit=RawWidget && Interaction->IsOverHitTestVisibleWidget()?Interaction->HoveredTarget():TWeakPtr<SWidget>();
    const bool RawSlate=RawWidget && Interaction->IsOverHitTestVisibleWidget() && CurrentHit.IsValid();
    const auto PreviousTarget=PreviousPointerHit.Pin();
    const auto CurrentTarget=CurrentHit.Pin();
    const bool TargetChanged=PreviousTarget!=CurrentTarget;
    const bool CancelForHit=MRFPSLoginPointer::CancelForHitChange(PointerInput.Sent,PreviousTarget.Get(),CurrentTarget.Get());
    if (!bRawTraceReported || RawWidget!=bRawTraceWidget || RawSlate!=bRawTraceSlate) {
        bRawTraceReported=true; bRawTraceWidget=RawWidget; bRawTraceSlate=RawSlate;
        UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_RAW widget=%d slate=%d"),RawWidget,RawSlate);
    }
    PreviousPointerHit=CurrentHit;
    if (CancelForHit) {
        // Only a held press must be cancelled on target changes. Neutral input
        // remains armed while the ray moves onto the next key.
        Interaction->CancelPointer(); PointerInput.CancelActive(); PointerInput.ClearArms();
        // Preserve raw button up/down, even when its UI action must be dropped.
        for (std::size_t I=0;I<PendingPointerEvents.Count;++I)
            if (PendingPointerEvents.Events[I].Kind==MRFPSLoginPointer::EInput::Button)
                PointerInput.SetButton(PendingPointerEvents.Events[I].Value!=0);
        PendingPointerEvents.Clear(); PendingPointerTargets.Reset(); LastPointerTick=FPlatformTime::Seconds(); bGloveCursorInitialized=false;
        if (Source!=MRFPSLoginPointer::ESource::None) Interaction->RefreshPointer();
    }
    // The journal retains right-hand transitions independently of gameplay's
    // latest snapshot, including both edges of a tap between widget frames.
    FMRFPSGloveHandFrame Glove;
    UMRFPSGloveSubsystem* Gloves=nullptr;
    if (PC && PC->GetGameInstance()) Gloves=PC->GetGameInstance()->GetSubsystem<UMRFPSGloveSubsystem>();
    if (Gloves) {
        // Drain pending UDP samples on the game thread immediately before the
        // UI reads the journal. This avoids depending on engine tick order.
        Gloves->Tick(0.f);
        Gloves->GetLatestHand(EMRFPSGloveHand::Right,Glove);
    }
    if (PC) if (auto* GI=Cast<UMRFPSGameInstance>(PC->GetGameInstance()))
        GI->FireTransition.Observe(Glove.bFlexValid && Glove.FingerCurls.IsValidIndex(1),
            Glove.FingerCurls.IsValidIndex(1)?Glove.FingerCurls[1]:0,Glove.FlexReceivedAt,
            Glove.bControllerValid,Glove.bTriggerPressed,Glove.Trigger,Glove.ControllerReceivedAt);
    const bool ControllerFresh=Glove.bControllerValid && FMath::IsFinite(Glove.Trigger);
    const bool FlexFresh=Glove.bFlexValid && Glove.FingerCurls.IsValidIndex(1) && FMath::IsFinite(Glove.FingerCurls[1]);
    if (LastPointerTick<0) { PendingPointerEvents.Clear(); PendingPointerTargets.Reset(); LastPointerTick=FPlatformTime::Seconds(); }
    const auto Queued=PendingPointerEvents; PendingPointerEvents.Clear();
    auto QueuedTargets=MoveTemp(PendingPointerTargets);
    const bool ButtonMismatch=!MRFPSLoginPointer::ButtonTargetsMatch(Queued,CurrentTarget.Get());
    if (ButtonMismatch) {
        if (PointerInput.Sent && PointerInput.Selected==MRFPSLoginPointer::EInput::Button) {
            Interaction->CancelPointer(); PointerInput.CancelActive();
            if (Source!=MRFPSLoginPointer::ESource::None) Interaction->RefreshPointer();
        }
        for (std::size_t I=0;I<Queued.Count;++I)
            if (Queued.Events[I].Kind==MRFPSLoginPointer::EInput::Button) PointerInput.SetButton(Queued.Events[I].Value!=0);
        PointerInput.ClearArm(MRFPSLoginPointer::EInput::Button);
    }
    if (TargetChanged) {
        // OSC has no XR pose at packet receipt; never replay its old edges on
        // a newly aimed control. Resume only after fresh neutral samples.
        PointerInput.ClearArm(MRFPSLoginPointer::EInput::Axis);
        PointerInput.ClearArm(MRFPSLoginPointer::EInput::GloveController);
        PointerInput.ClearArm(MRFPSLoginPointer::EInput::GloveFlex);
        for (std::size_t I=0;I<Queued.Count;++I)
            if (Queued.Events[I].Kind==MRFPSLoginPointer::EInput::Axis) PointerInput.SetAxis(Queued.Events[I].Value);
    }
    std::array<MRFPSGloveTrigger::FEvent,MRFPSGloveTrigger::FJournal::Capacity> GloveEvents;
    std::size_t GloveCount=0; bool JournalOk=true;
    if (Gloves) {
        const auto& Journal=Gloves->GetRightTriggerJournal();
        if (!bGloveCursorInitialized) { GloveCursor=Journal.Latest(); bGloveCursorInitialized=true; }
        JournalOk=Journal.Read(GloveCursor,GloveEvents,GloveCount);
    }
    const double SampleAt=FPlatformTime::Seconds();
    bool LateController=false,LateFlex=false;
    for (std::size_t I=0;I<GloveCount;++I) if (GloveEvents[I].At<LastPointerTick) {
        if (GloveEvents[I].Stream==MRFPSGloveTrigger::EStream::Controller) LateController=true; else LateFlex=true;
    }
    TArray<MRFPSLoginPointer::FEventQueue::FEvent> Combined;
    if (!ControllerFresh || LateController) Combined.Add({MRFPSLoginPointer::EInput::GloveController,0,LastPointerTick,false,false});
    if (!FlexFresh || LateFlex) Combined.Add({MRFPSLoginPointer::EInput::GloveFlex,0,LastPointerTick,false,false});
    for (std::size_t I=0;I<Queued.Count;++I) {
        const auto& E=Queued.Events[I];
        if ((ButtonMismatch && E.Kind==MRFPSLoginPointer::EInput::Button) ||
            (TargetChanged && E.Kind==MRFPSLoginPointer::EInput::Axis)) continue;
        Combined.Add(E);
    }
    for (std::size_t I=0;I<GloveCount;++I) {
        const auto& E=GloveEvents[I];
        const bool IsControllerEvent=E.Stream==MRFPSGloveTrigger::EStream::Controller;
        if (TargetChanged || E.At<LastPointerTick || (IsControllerEvent && !ControllerFresh) || (!IsControllerEvent && !FlexFresh)) continue;
        Combined.Add({IsControllerEvent?MRFPSLoginPointer::EInput::GloveController:MRFPSLoginPointer::EInput::GloveFlex,
            E.Value,E.At,E.Valid,E.Button});
    }
    if (ControllerFresh) Combined.Add({MRFPSLoginPointer::EInput::GloveController,Glove.Trigger,SampleAt,true,Glove.bTriggerPressed});
    if (FlexFresh) Combined.Add({MRFPSLoginPointer::EInput::GloveFlex,Glove.FingerCurls[1],SampleAt,true,false});
    Combined.StableSort([](const auto& A,const auto& B) { return A.At<B.At; });
    MRFPSLoginPointer::FEventQueue Events;
    if (!JournalOk || Queued.Overflow) Events.Overflow=true;
    else for (const auto& E:Combined) Events.Push(E.Kind,E.Value,E.At,E.Valid,E.Flag,E.Target,E.TargetCaptured);
    const double End=FPlatformTime::Seconds();
    const auto Preview=MRFPSLoginPointer::PreviewBatch(PointerInput,Events,LastPointerTick,End);
    if (!Preview.Valid) {
        Interaction->CancelPointer(); PointerInput.FailClosed();
        UE_LOG(LogMRFPSLogin,Warning,TEXT("MRFPS_LOGIN_POINTER_INPUT_OVERFLOW_OR_TIME"));
    } else if (Preview.Ambiguous()) {
        // Multiple completed/pending clicks cannot be attributed to separate
        // targets from one sampled pose. A cancelled glove-to-button takeover
        // remains one click and is intentionally allowed.
        Interaction->CancelPointer(); PointerInput=Preview.Final; PointerInput.CancelActive(); PointerInput.ClearArms();
        UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_BATCH_DROPPED"));
    } else {
        MRFPSLoginPointer::Dispatch(PointerInput,Events,LastPointerTick,End,
            [this](MRFPSLoginPointer::EAction Action) {
                ApplyPointerAction(Action); return bOpen && Interaction && bPointerGridReady;
            },
            [](const MRFPSLoginPointer::FEventQueue::FEvent& Event) {
                UE_LOG(LogMRFPSLogin,VeryVerbose,TEXT("MRFPS_LOGIN_POINTER_INPUT kind=%d valid=%d"),int(Event.Kind),Event.Valid);
            });
    }
    QueuedTargets.Reset();
    LastPointerTick=End;
    if (!bOpen || !Interaction || !bPointerGridReady) return;
    if (!bInputAnnounced || AnnouncedInput!=PointerInput.Selected) {
        AnnouncedInput=PointerInput.Selected; bInputAnnounced=true;
        const TCHAR* PoseName=Source==MRFPSLoginPointer::ESource::Aim || Source==MRFPSLoginPointer::ESource::AimGrip?TEXT("右コントローラー"):
            Source==MRFPSLoginPointer::ESource::Grip?TEXT("右コントローラー・仮向き"):
            Source==MRFPSLoginPointer::ESource::Hand?TEXT("右手追跡"):
            Source==MRFPSLoginPointer::ESource::Glove?TEXT("外部トラッカー"):TEXT("追跡なし");
        const TCHAR* InputName=PointerInput.Mode==MRFPSLoginPointer::EMode::Auto?TEXT("トリガー / グローブ"):
            AnnouncedInput==MRFPSLoginPointer::EInput::GloveFlex?TEXT("グローブ指"):
            AnnouncedInput==MRFPSLoginPointer::EInput::GloveController?TEXT("グローブトリガー"):
            AnnouncedInput==MRFPSLoginPointer::EInput::Axis || AnnouncedInput==MRFPSLoginPointer::EInput::Button?TEXT("コントローラー"):
            TEXT("入力なし");
        if (PointerHint) PointerHint->SetText(FText::FromString(FString::Printf(TEXT("%s / %s"),PoseName,InputName)));
        UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_INPUT_SOURCE kind=%d"),int(AnnouncedInput));
    }
    const FHitResult& Hit=RawHit;
    const bool OnWidget=RawSlate;
    if (!bPointerHitReported || OnWidget!=bPointerHit) { bPointerHit=OnWidget; bPointerHitReported=true; UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_HIT value=%d"),OnWidget); }
    if (PointerRay) {
        PointerRay->SetVisibility(Source!=MRFPSLoginPointer::ESource::None);
        const float Length=OnWidget?FMath::Clamp(static_cast<float>(FVector::Distance(Interaction->GetComponentLocation(),Hit.ImpactPoint)),1.f,400.f):400.f;
        PointerRay->SetRelativeLocation(FVector(Length*.5f,0,0)); PointerRay->SetRelativeScale3D(FVector(.014,.014,Length/100.f));
    }
    if (PointerDot) { PointerDot->SetVisibility(OnWidget); if (OnWidget) PointerDot->SetWorldLocation(Hit.ImpactPoint+Hit.ImpactNormal*.2f); }
#endif
}
void UMRFPSLoginMenu::ApplyPointerAction(MRFPSLoginPointer::EAction Action)
{
    if (!Interaction) return;
    if (Action==MRFPSLoginPointer::EAction::None) return;
    if (Action==MRFPSLoginPointer::EAction::Press) {
        const FHitResult& Hit=Interaction->GetLastHitResult();
        const auto CurrentTarget=Interaction->HoveredTarget().Pin();
        const bool Blocking=Hit.bBlockingHit;
        const bool WidgetMatch=WorldWidget && Hit.GetComponent()==WorldWidget.Get();
        const bool SlateHit=Interaction->IsOverHitTestVisibleWidget();
        const bool TargetMatch=MRFPSLoginPointer::SameHit(PreviousPointerHit.Pin().Get(),CurrentTarget.Get());
        const bool Capture=Blocking && WidgetMatch && SlateHit && TargetMatch && Interaction->BeginPointer();
        if (!Capture) {
            PointerInput.CancelActive(); PointerInput.Disarm();
            UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_PRESS action=unavailable source=%s blocking=%d widget=%d slate=%d target=%d capture=%d"),
                MRFPSLoginPointer::SourceName(PointerInput.Source),Blocking,WidgetMatch,SlateHit,TargetMatch,Capture);
            return;
        }
    } else if (Action==MRFPSLoginPointer::EAction::Release) Interaction->EndPointer();
    else {
        Interaction->CancelPointer();
        // A physical trigger can immediately take over a cancelled glove press.
        // Cancel clears WIC hit/hover state, so trace the same muzzle pose again.
        if (bOpen && PointerInput.Source!=MRFPSLoginPointer::ESource::None) Interaction->RefreshPointer();
    }
    UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_PRESS action=%s source=%s"),
        Action==MRFPSLoginPointer::EAction::Press?TEXT("press"):Action==MRFPSLoginPointer::EAction::Release?TEXT("release"):TEXT("cancel"),
        MRFPSLoginPointer::SourceName(PointerInput.Source));
}
void UMRFPSLoginMenu::HandleFocusLost()
{
    if (!bOpen) return;
    if (Interaction) Interaction->CancelPointer();
    PointerInput.CancelActive(); PointerInput.SetFocused(false); PendingPointerEvents.Clear(); PendingPointerTargets.Reset(); LastPointerTick=-1; bGloveCursorInitialized=false; PreviousPointerHit.Reset();
    UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_FOCUS active=0"));
}
void UMRFPSLoginMenu::HandleFocusGained()
{
    if (!bOpen) return;
    if (Interaction) Interaction->CancelPointer();
    PointerInput.CancelActive(); PointerInput.SetFocused(true); PendingPointerEvents.Clear(); PendingPointerTargets.Reset(); LastPointerTick=-1; bGloveCursorInitialized=false; PreviousPointerHit.Reset();
    UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_LOGIN_POINTER_FOCUS active=1"));
}
void UMRFPSLoginMenu::ProcessControlFile()
{
#if !UE_SERVER
    FString File=ControlPath();
#if WITH_DEV_AUTOMATION_TESTS
    if (!TestControlPath.IsEmpty()) File=TestControlPath;
#endif
    if (File.IsEmpty()) return;
    if (!IFileManager::Get().FileExists(*File)) return;
    // Claim the current instruction atomically. A subsequent CLI submission
    // may replace the source path while this unique file is being read; it
    // must remain there for the next tick rather than being deleted here.
    const FString Processing=File+TEXT(".processing-")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    if (!IFileManager::Get().Move(*Processing,*File,false,false,false,true)) return;
    const int64 Size=IFileManager::Get().FileSize(*Processing);
    FString Body; const bool Read=Size>0 && Size<=1024 && FFileHelper::LoadFileToString(Body,*Processing);
    if (!IFileManager::Get().Delete(*Processing,false,true)) return;
    if (!Read) { SetStatus(TEXT("操作ファイルを読めません"),true); return; }
    TSharedPtr<FJsonObject> Json; const auto Reader=TJsonReaderFactory<>::Create(Body);
    if (!FJsonSerializer::Deserialize(Reader,Json) || !Json.IsValid()) { SetStatus(TEXT("操作ファイルを確認してください"),true); return; }
    FString Server,RequestedRole,Code;
    if (!Json->TryGetStringField(TEXT("server"),Server) || !Json->TryGetStringField(TEXT("role"),RequestedRole) ||
        (RequestedRole!=TEXT("player") && RequestedRole!=TEXT("inspector") && RequestedRole!=TEXT("admin"))) {
        SetStatus(TEXT("接続設定を確認してください"),true); return;
    }
    SetEndpoint(Server); SetStage(false); Next(); if (!bLoginPage) return; SelectRole(RequestedRole);
    UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_DEVICE_CONTROL role=%s has_code=%d"),*RequestedRole,Json->HasField(TEXT("code")));
    if (Json->TryGetStringField(TEXT("code"),Code)) { CodeBox->SetText(FText::FromString(Code)); Exchange(); }
    if (!Body.IsEmpty()) FMemory::Memzero(Body.GetCharArray().GetData(),Body.GetCharArray().Num()*sizeof(TCHAR));
    if (!Code.IsEmpty()) FMemory::Memzero(Code.GetCharArray().GetData(),Code.GetCharArray().Num()*sizeof(TCHAR));
#endif
}
void UMRFPSLoginMenu::CloseLogin()
{
    if (bClosing) return; bClosing=true; bOpen=false;
    if (auto* PC=Controller.Get()) if (auto* GI=Cast<UMRFPSGameInstance>(PC->GetGameInstance())) {
        FMRFPSGloveHandFrame Glove;
        if (auto* Gloves=GI->GetSubsystem<UMRFPSGloveSubsystem>()) {
            Gloves->Tick(0.f); Gloves->GetLatestHand(EMRFPSGloveHand::Right,Glove);
        }
        GI->FireTransition.Observe(Glove.bFlexValid && Glove.FingerCurls.IsValidIndex(1),
            Glove.FingerCurls.IsValidIndex(1)?Glove.FingerCurls[1]:0,Glove.FlexReceivedAt,
            Glove.bControllerValid,Glove.bTriggerPressed,Glove.Trigger,Glove.ControllerReceivedAt);
        GI->FireTransition.SealForTravel();
    }
    if (PendingRequest) { PendingRequest->OnProcessRequestComplete().Unbind(); PendingRequest->CancelRequest(); PendingRequest.Reset(); }
    FCoreDelegates::ApplicationWillDeactivateDelegate.Remove(FocusLostHandle);
    FCoreDelegates::ApplicationHasReactivatedDelegate.Remove(FocusGainedHandle);
    FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
    FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
    if (Interaction) Interaction->CancelPointer(); PointerInput.CancelActive(); PendingPointerEvents.Clear(); PendingPointerTargets.Reset();
    if (auto* PC=Controller.Get()) if (auto* Pawn=PC->GetPawn<AMRFPSPawn>(); Pawn && Pawn->GetSlot()<0 && Pawn->GunVisual)
        Pawn->GunVisual->SetVisibility(false);
    if (PointerDot) { PointerDot->DestroyComponent(); PointerDot=nullptr; }
    if (PointerRay) { PointerRay->DestroyComponent(); PointerRay=nullptr; }
    if (Interaction) { Interaction->DestroyComponent(); Interaction=nullptr; }
    if (WorldWidget) { auto* Component=WorldWidget.Get(); WorldWidget=nullptr; Component->SetWidget(nullptr); Component->DestroyComponent(); }
    if (auto* PC=Controller.Get()) { PC->SetIgnoreMoveInput(false); PC->SetIgnoreLookInput(false); PC->FlushPressedKeys(); PC->SetInputMode(FInputModeGameOnly()); PC->bShowMouseCursor=false; }
    RemoveFromParent();
}
void UMRFPSLoginMenu::SetPointerPressed(bool Pressed)
{
    if (!Interaction || !bOpen) return;
    if (!bPointerGridReady || LastPointerPaint<0 || FPlatformTime::Seconds()-LastPointerPaint>=.75) {
        PointerInput.SetButton(Pressed); PointerInput.ClearArm(MRFPSLoginPointer::EInput::Button); return;
    }
    // Sample the actual muzzle and widget at the physical edge. The next
    // frame must never replay an A tap onto B after the hand has moved.
    TSharedPtr<SWidget> Target;
    auto* PC=Controller.Get(); auto* Pawn=PC?PC->GetPawn<AMRFPSPawn>():nullptr;
    FMRFPSWeaponPose Weapon;
    bool UsesVRFocus=false,HasVRFocus=true;
    UHeadMountedDisplayFunctionLibrary::GetVRFocusState(UsesVRFocus,HasVRFocus);
    if (PointerInput.Focused && (!UsesVRFocus || HasVRFocus) && Pawn && ResolvePointerWeapon(Pawn,Weapon) &&
        Weapon.Source==PointerInput.Source && Weapon.Source!=MRFPSLoginPointer::ESource::None) {
        const FTransform Muzzle=MRFPSWeaponPose::Muzzle(Weapon.GripWorld);
        Interaction->SetWorldLocationAndRotation(Muzzle.GetLocation(),Muzzle.GetRotation());
        Interaction->RefreshPointer();
        const auto& Hit=Interaction->GetLastHitResult();
        if (Hit.bBlockingHit && Hit.GetComponent()==WorldWidget.Get() && Interaction->IsOverHitTestVisibleWidget())
            Target=Interaction->HoveredTarget().Pin();
    }
    const std::size_t Before=PendingPointerEvents.Count;
    PendingPointerEvents.Push(MRFPSLoginPointer::EInput::Button,Pressed?1.f:0.f,FPlatformTime::Seconds(),true,false,
        reinterpret_cast<std::uintptr_t>(Target.Get()),true);
    if (PendingPointerEvents.Overflow) PendingPointerTargets.Reset();
    else if (Target && PendingPointerEvents.Count>Before) PendingPointerTargets.Add(Target);
}
void UMRFPSLoginMenu::SetPointerTriggerAxis(float Value)
{
    if (!Interaction || !bOpen) return;
    if (!bPointerGridReady || LastPointerPaint<0 || FPlatformTime::Seconds()-LastPointerPaint>=.75) {
        PointerInput.SetAxis(Value); PointerInput.ClearArm(MRFPSLoginPointer::EInput::Axis); return;
    }
    PendingPointerEvents.Push(MRFPSLoginPointer::EInput::Axis,Value,FPlatformTime::Seconds());
}
void UMRFPSLoginMenu::SetEndpoint(const FString& Address) { if (AddressBox && !bExchanging) AddressBox->SetText(FText::FromString(Address.Left(255))); }
void UMRFPSLoginMenu::InsertKey(const FString& Key)
{
    if (bExchanging) return;
    auto* Field=bLoginPage?CodeBox.Get():AddressBox.Get(); if (!Field) return;
    FString Value=Field->GetText().ToString();
    if (Key==TEXT("BACK")) { if (!Value.IsEmpty()) Value.LeftChopInline(1); }
    else if ((bLoginPage && Value.Len()<6 && Key.IsNumeric()) || (!bLoginPage && Value.Len()<255)) Value+=Key;
    Field->SetText(FText::FromString(Value));
}
void UMRFPSLoginMenu::SetStatus(const FString& Message,bool Error)
{
    if (Status) { Status->SetText(FText::FromString(Message)); Status->SetColorAndOpacity(FSlateColor(Error?FLinearColor(1,.4f,.4f,1):Muted)); }
}
void UMRFPSLoginMenu::SetStage(bool Login)
{
    // The old page's Slate hit grid remains valid until the next completed
    // paint. Discard edges across the change instead of clicking an old key.
    bPointerGridReady=false; LastPointerPaint=-1; bGloveCursorInitialized=false;
    if (Interaction) Interaction->CancelPointer();
    PointerInput.CancelActive(); PointerInput.ClearArms();
    if (auto* PC=Controller.Get()) if (auto* GI=Cast<UMRFPSGameInstance>(PC->GetGameInstance()))
        PointerInput.SetButton(GI->FireTransition.PhysicalDown);
    PendingPointerEvents.Clear(); PendingPointerTargets.Reset(); PreviousPointerHit.Reset();
    bLoginPage=Login;
    if (AddressPage) AddressPage->SetVisibility(Login?ESlateVisibility::Collapsed:ESlateVisibility::Visible);
    if (LoginPage) LoginPage->SetVisibility(Login?ESlateVisibility::Visible:ESlateVisibility::Collapsed);
    if (Heading) Heading->SetText(FText::FromString(Login?TEXT("デバイスログイン"):TEXT("サーバー設定")));
    SetStatus(Login?TEXT("管理パネルの6桁コードを入力"):TEXT("接続先を確認してください"));
}
bool UMRFPSLoginMenu::ReadAddress(FString& Out) const
{
    Out=AddressBox?AddressBox->GetText().ToString().TrimStartAndEnd():FString();
    if (Out.IsEmpty() || Out.Len()>255) return false;
    for (TCHAR C:Out) if (!FChar::IsAlnum(C) && C!='.' && C!=':' && C!='-' && C!='[' && C!=']') return false;
    FString Host=Out, Port;
    if (Out.StartsWith(TEXT("["))) {
        int32 End; if (!Out.FindChar(']',End) || End<2) return false;
        Host=Out.Mid(1,End-1); const FString Tail=Out.Mid(End+1);
        if (!Tail.IsEmpty()) { if (!Tail.StartsWith(TEXT(":"))) return false; Port=Tail.Mid(1); if (Port.IsEmpty()) return false; }
    } else {
        int32 Colon; if (Out.FindChar(':',Colon)) { Host=Out.Left(Colon); Port=Out.Mid(Colon+1); if (Port.IsEmpty()) return false; }
    }
    if (Host.IsEmpty()) return false;
    if (!Port.IsEmpty()) { if (!Port.IsNumeric() || Port.Len()>5) return false; const int32 N=FCString::Atoi(*Port); if (N<1 || N>65535) return false; }
    return true;
}
void UMRFPSLoginMenu::Next()
{
    if (bExchanging) return;
    if (!ReadAddress(Destination)) { SetStatus(TEXT("接続先とポートを確認してください"),true); return; }
    if (GConfig) { GConfig->SetString(TEXT("MRFPS.Connection"),TEXT("Server"),*Destination,GGameUserSettingsIni); GConfig->Flush(false,GGameUserSettingsIni); }
    SetStage(true);
}
void UMRFPSLoginMenu::Back() { if (!bExchanging) SetStage(false); }
void UMRFPSLoginMenu::SelectRole(const FString& R)
{
    if (bExchanging) return; Role=R;
    if (RoleLabel) RoleLabel->SetText(FText::FromString(Role==TEXT("admin")?TEXT("管理"):Role==TEXT("inspector")?TEXT("観戦"):TEXT("プレイヤー")));
}
void UMRFPSLoginMenu::SelectPlayer() { SelectRole(TEXT("player")); }
void UMRFPSLoginMenu::SelectInspector() { SelectRole(TEXT("inspector")); }
void UMRFPSLoginMenu::SelectAdmin() { SelectRole(TEXT("admin")); }
void UMRFPSLoginMenu::Exchange()
{
    if (bExchanging || !bLoginPage || !CodeBox) return;
    FString Code=CodeBox->GetText().ToString().TrimStartAndEnd();
    if (Code.Len()!=6 || !Code.IsNumeric()) { SetStatus(TEXT("6桁の数字を入力してください"),true); return; }
    FString URL=AuthUrl;
#if !UE_BUILD_SHIPPING
    FString TestURL;
    if (FParse::Value(FCommandLine::Get(),TEXT("MRFPSDeviceAuthTestUrl="),TestURL)) {
        if (!MRFPSDeviceAuthUrl::IsSafeTestUrl(TestURL)) {
            UE_LOG(LogMRFPSLogin,Warning,TEXT("MRFPS_DEVICE_EXCHANGE_ERROR code=TEST_URL"));
            SetStatus(TEXT("テスト用認証先が無効です"),true); return;
        }
        URL=TestURL;
    }
#endif
    auto Json=MakeShared<FJsonObject>(); Json->SetStringField(TEXT("role"),Role); Json->SetStringField(TEXT("code"),Code);
    FString Body; const auto Writer=TJsonWriterFactory<>::Create(&Body); FJsonSerializer::Serialize(Json,Writer);
    CodeBox->SetText(FText::GetEmpty()); Code.Empty();
    auto Request=FHttpModule::Get().CreateRequest(); PendingRequest=Request;
    Request->SetURL(URL); Request->SetVerb(TEXT("POST")); Request->SetHeader(TEXT("Content-Type"),TEXT("application/json"));
    Request->SetTimeout(12.f); Request->SetContentAsString(Body); Body.Empty();
    const TWeakObjectPtr<UMRFPSLoginMenu> Weak(this);
    Request->OnProcessRequestComplete().BindLambda([Weak](FHttpRequestPtr Req,FHttpResponsePtr Response,bool OK) { if (Weak.IsValid()) Weak->HandleExchange(Req,Response,OK); });
    bExchanging=true; SetStatus(TEXT("認証中…"));
    UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_DEVICE_EXCHANGE_START role=%s"),*Role);
    if (!Request->ProcessRequest()) { PendingRequest.Reset(); bExchanging=false; UE_LOG(LogMRFPSLogin,Warning,TEXT("MRFPS_DEVICE_EXCHANGE_ERROR code=REQUEST_START")); SetStatus(TEXT("認証サーバーに接続できません"),true); }
}
void UMRFPSLoginMenu::HandleExchange(FHttpRequestPtr Request,FHttpResponsePtr Response,bool OK)
{
    if (!bOpen || !PendingRequest || Request!=PendingRequest) return;
    PendingRequest.Reset(); bExchanging=false;
    if (!OK || !Response.IsValid()) { UE_LOG(LogMRFPSLogin,Warning,TEXT("MRFPS_DEVICE_EXCHANGE_ERROR code=NETWORK")); SetStatus(TEXT("認証サーバーに接続できません"),true); return; }
    if (Response->GetResponseCode()!=200) {
        UE_LOG(LogMRFPSLogin,Warning,TEXT("MRFPS_DEVICE_EXCHANGE_ERROR code=HTTP_%d"),Response->GetResponseCode());
        if (Response->GetResponseCode()==429) {
            const FString RetryAfter=Response->GetHeader(TEXT("Retry-After"));
            const int Seconds=MRFPSRetryAfter::Parse(*RetryAfter,static_cast<std::size_t>(RetryAfter.Len()));
            SetStatus(Seconds?FString::Printf(TEXT("%d秒後、新しいコードで再試行してください"),Seconds):
                              FString(TEXT("しばらく待ってから再試行してください")),true);
        } else SetStatus(TEXT("コードが無効か期限切れです"),true);
        return;
    }
    if (Response->GetContent().Num()>8192) { SetStatus(TEXT("認証応答が無効です"),true); return; }
    TSharedPtr<FJsonObject> Json; const auto Reader=TJsonReaderFactory<>::Create(Response->GetContentAsString());
    FString Token,Key;
    if (!FJsonSerializer::Deserialize(Reader,Json) || !Json.IsValid() || !Json->TryGetStringField(TEXT("token"),Token) || !Json->TryGetStringField(TEXT("public_key"),Key)) {
        SetStatus(TEXT("認証応答が無効です"),true); return;
    }
    auto* PC=Controller.Get(); auto* Credentials=PC?PC->GetGameInstance()->GetSubsystem<UMRFPSAuthClientSubsystem>():nullptr;
    FString Error;
    const bool Accepted=Credentials && Credentials->AcceptDeviceExchange(Token,Key,Role,Error);
    if (!Token.IsEmpty()) FMemory::Memzero(Token.GetCharArray().GetData(),Token.GetCharArray().Num()*sizeof(TCHAR));
    if (!Accepted) {
        UE_LOG(LogMRFPSLogin,Warning,TEXT("MRFPS_DEVICE_EXCHANGE_ERROR code=%s"),Error.IsEmpty()?TEXT("AUTH_CONFIG"):*Error);
        if (Error==TEXT("AUTH_KEY_CHANGED")) { TrustButton->SetVisibility(ESlateVisibility::Visible); SetStatus(TEXT("公開鍵が変更されました。確認してから保存済みの鍵を解除してください"),true); }
        else SetStatus(TEXT("認証情報を検証できませんでした"),true);
        return;
    }
    UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_DEVICE_EXCHANGE_VERIFIED role=%s"),*Role);
    TrustButton->SetVisibility(ESlateVisibility::Collapsed); bExchanging=true; SetStatus(TEXT("ログイン成功。サーバーに接続します"));
    FTimerHandle Handle; GetWorld()->GetTimerManager().SetTimer(Handle,FTimerDelegate::CreateWeakLambda(this,[this]() { ConnectAfterLogin(); }),1.5f,false);
}
void UMRFPSLoginMenu::ResetTrust()
{
    auto* PC=Controller.Get(); auto* Credentials=PC?PC->GetGameInstance()->GetSubsystem<UMRFPSAuthClientSubsystem>():nullptr; FString Error;
    if (!Credentials || !Credentials->ClearIssuerTrust(Error)) { SetStatus(TEXT("保存済みの公開鍵を解除できません"),true); return; }
    TrustButton->SetVisibility(ESlateVisibility::Collapsed); SetStatus(TEXT("公開鍵を解除しました。新しいコードで再ログインしてください"));
}
void UMRFPSLoginMenu::ConnectAfterLogin()
{
    auto* PC=Controller.Get(); if (!PC || !bOpen) return;
    const FString Address=Destination, SelectedRole=Role;
    UE_LOG(LogMRFPSLogin,Display,TEXT("MRFPS_DEVICE_CONNECT role=%s"),*SelectedRole);
    CloseLogin(); PC->ConnectionError.Empty();
    auto* Instance=PC->GetGameInstance<UMRFPSGameInstance>();
    if (Instance) Instance->MarkNextConnectionAsDeviceLogin();
    if (SelectedRole==TEXT("admin")) PC->MRFPSConnectAdmin(Address);
    else PC->MRFPSConnect(Address,SelectedRole==TEXT("inspector"));
    if (!PC->ConnectionError.IsEmpty() && PC->ConnectionError!=TEXT("Connecting...") && PC->GetNetMode()==NM_Standalone) {
        if (Instance) Instance->CancelNextDeviceConnection();
        PC->OpenDeviceLogin();
        if (PC->DeviceLogin) PC->DeviceLogin->SetStatus(TEXT("サーバーに接続できません"),true);
    }
}
void UMRFPSLoginMenu::LoginCodeFromFile(const FString& File,const FString& RequestedRole)
{
    if (!bOpen || bExchanging || !AddressBox || !CodeBox) return;
    if (RequestedRole!=TEXT("player") && RequestedRole!=TEXT("inspector") && RequestedRole!=TEXT("admin")) { SetStatus(TEXT("役割を確認してください"),true); return; }
    FString Code;
    const int64 Size=IFileManager::Get().FileSize(*File);
    if (Size<=0 || Size>64 || !FFileHelper::LoadFileToString(Code,*File)) { SetStatus(TEXT("コードファイルを読めません"),true); return; }
#if PLATFORM_ANDROID
    // Only the app-private fixed staging file belongs to our provisioning
    // command. Never delete an arbitrary path supplied by a caller.
    FString Owned=FPaths::Combine(UMRFPSAuthClientSubsystem::PrivateDirectory(),TEXT("device-code.txt")),Supplied=File;
    FPaths::NormalizeFilename(Owned); FPaths::NormalizeFilename(Supplied);
    if (Owned==Supplied) IFileManager::Get().Delete(*Owned,false,true);
#endif
    Next(); if (!bLoginPage) return;
    SelectRole(RequestedRole); CodeBox->SetText(FText::FromString(Code.TrimStartAndEnd())); Exchange();
}
