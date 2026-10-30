#include "MRFPSMoBitsSetup.h"
#include "MRFPSMoBitsIdentify.h"
#include "MRFPSGame.h"
#include "MRFPSLoginMenu.h"
#include "MoBitsQuestStandaloneComponent.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSMoBitsSetup,Log,All);

namespace
{
const FLinearColor White(.91f,.95f,1.f,1.f), Muted(.65f,.76f,.86f,1.f), Blue(.035f,.265f,.79f,1.f);

UTextBlock* Label(UWidgetTree* Tree,UVerticalBox* Parent,const FString& Value,int32 Size=16)
{
    auto* Text=Tree->ConstructWidget<UTextBlock>();
    Text->SetFont(FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),Size));
    Text->SetColorAndOpacity(FSlateColor(White)); Text->SetAutoWrapText(true);
    Text->SetText(FText::FromString(Value)); Parent->AddChild(Text); return Text;
}
UHorizontalBox* Row(UWidgetTree* Tree,UVerticalBox* Parent)
{
    auto* Box=Tree->ConstructWidget<UHorizontalBox>(); Parent->AddChild(Box); return Box;
}
UButton* Button(UWidgetTree* Tree,UHorizontalBox* Parent,const FString& Caption,bool Danger=false)
{
    auto* B=Tree->ConstructWidget<UButton>();
    FButtonStyle Style; const FLinearColor Color=Danger?FLinearColor(.36f,.08f,.09f,1.f):Blue;
    Style.SetNormal(FSlateRoundedBoxBrush(Color,5));
    Style.SetHovered(FSlateRoundedBoxBrush(FMath::Lerp(Color,FLinearColor(.25f,.61f,1.f,1.f),.3f),5));
    Style.SetPressed(FSlateRoundedBoxBrush(FMath::Lerp(Color,FLinearColor::Black,.2f),5));
    Style.SetNormalPadding(FMargin(6,5)); Style.SetPressedPadding(FMargin(6,6,6,4)); B->SetStyle(Style);
    auto* Text=Tree->ConstructWidget<UTextBlock>();
    Text->SetFont(FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),16));
    Text->SetColorAndOpacity(FSlateColor(White)); Text->SetJustification(ETextJustify::Center);
    Text->SetAutoWrapText(true); Text->SetText(FText::FromString(Caption)); B->SetContent(Text);
    auto* Slot=Parent->AddChildToHorizontalBox(B); Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); Slot->SetPadding(FMargin(2));
    return B;
}
void TextIfChanged(UTextBlock* Target,const FString& Text)
{ if (Target && Target->GetText().ToString()!=Text) Target->SetText(FText::FromString(Text)); }
void Show(UVerticalBox* Page,bool Visible)
{
    if (!Page) return;
    const auto Wanted=Visible?ESlateVisibility::Visible:ESlateVisibility::Collapsed;
    if (Page->GetVisibility()!=Wanted) Page->SetVisibility(Wanted);
}
TArray<FMoBitsQuestDevice> Candidates(UMoBitsQuestStandaloneComponent* Runtime)
{
    TArray<FMoBitsQuestDevice> Result;
    if (Runtime) for (const auto& Device:Runtime->GetStatus().Devices)
        if (Device.Model==50 && !Device.Id.IsEmpty()) Result.Add(Device);
    Result.Sort([](const auto& A,const auto& B) { return A.Id<B.Id; });
    return Result;
}
FString ErrorText(const FMoBitsQuestStatus& Status)
{
    if (Status.Code==0) return FString();
    const FString& M=Status.Message;
    const TCHAR* Text=TEXT("処理できませんでした。状態を確認して再操作してください。");
    if (Status.Code==7) Text=TEXT("機器とのBluetooth接続に失敗しました。電源・距離と他アプリの接続を確認してください。");
    else if (Status.Code==188) Text=TEXT("Bluetooth登録の準備に失敗しました。他アプリの接続を終了し、機器を探し直してください。");
    else if (M.Contains(TEXT("Stopped: make a new explicit request"),ESearchCase::IgnoreCase)) Text=TEXT("停止後です。「開始・権限」を押し、もう一度操作してください。");
    else if (M.Contains(TEXT("Game is not advancing"),ESearchCase::IgnoreCase)) Text=TEXT("Questを装着してゲームに戻り、もう一度操作してください。");
    else if (M.Contains(TEXT("Select a fresh EVT5 pairing advertisement"),ESearchCase::IgnoreCase)) Text=TEXT("機器をペアリングモードにし、「機器を探す」から選び直してください。");
    else if (M.Contains(TEXT("Scan before selecting"),ESearchCase::IgnoreCase)) Text=TEXT("先に「機器を探す」を押し、接続する機器を選んでください。");
    else if (M.Contains(TEXT("Initialize the standalone runtime first"),ESearchCase::IgnoreCase) || M.Contains(TEXT("Start first"),ESearchCase::IgnoreCase)) Text=TEXT("先に「開始・権限」を押してから操作してください。");
    else if (M.Contains(TEXT("Close the session before pairing"),ESearchCase::IgnoreCase) || M.Contains(TEXT("Close the previous session first"),ESearchCase::IgnoreCase)) Text=TEXT("セッションを停止・終了してから機器を接続してください。");
    else if (M.Contains(TEXT("Disconnect other EMS devices"),ESearchCase::IgnoreCase)) Text=TEXT("選択した２台以外のMoBitsを切断してから開始してください。");
    else if (M.Contains(TEXT("Wait for fresh safety status"),ESearchCase::IgnoreCase)) Text=TEXT("接続直後です。機器の状態を受信するまで待ってから開始してください。");
    else if (M.Contains(TEXT("not queued"),ESearchCase::IgnoreCase) || M.Contains(TEXT("queue full"),ESearchCase::IgnoreCase)) Text=TEXT("前の接続操作の完了を待ってから、もう一度押してください。");
    else if (M.Contains(TEXT("permission"),ESearchCase::IgnoreCase) || M.Contains(TEXT("Nearby"))) Text=TEXT("付近のデバイスを許可し、もう一度「開始」を押してください。");
    else if (M.Contains(TEXT("battery"),ESearchCase::IgnoreCase) || M.Contains(TEXT("USB"))) Text=TEXT("機器を充電し、USBを外してから再開してください。");
    else if (M.Contains(TEXT("firmware"),ESearchCase::IgnoreCase) || M.Contains(TEXT("direct-BLE"))) Text=TEXT("機器のファームウェアとBluetooth対応を確認してください。");
    else if (M.Contains(TEXT("STOP is unconfirmed"),ESearchCase::IgnoreCase)) Text=TEXT("停止の確認待ちです。機器の停止を確認し、終了を再操作してください。");
    else if (M.Contains(TEXT("threshold"),ESearchCase::IgnoreCase)) Text=TEXT("確認した感覚の範囲では波形を準備できません。校正を確認してください。");
    else if (M.Contains(TEXT("rate"),ESearchCase::IgnoreCase) || M.Contains(TEXT("budget"))) Text=TEXT("間隔が短いため出力を見送りました。少し待ってください。");
    else if (M.Contains(TEXT("pairing"),ESearchCase::IgnoreCase)) Text=TEXT("機器をペアリングモードにして探し直してください。");
    else if (M.Contains(TEXT("timeout"),ESearchCase::IgnoreCase)) Text=TEXT("機器から応答がなく停止しました。接続を確認してください。");
    return FString::Printf(TEXT("%s (%d)"),Text,Status.Code);
}
const TCHAR* WaveName(int32 Wave)
{
    switch (Wave) {
    case 1:return TEXT("銃の反動"); case 2:return TEXT("エネルギー弾");
    case 3:return TEXT("被弾"); case 4:return TEXT("連射"); default:return TEXT("-");
    }
}
}

void UMRFPSMoBitsSetup::Build(UMRFPSLoginMenu* InMenu,UWidgetTree* Tree,UVerticalBox* Parent)
{
    Menu=InMenu;
    StateText=Label(Tree,Parent,TEXT("サーバー参加後に設定してください。"));
    NoteText=Label(Tree,Parent,TEXT("右トリガーで選択 / 右Bで停止"),15);
    NoteText->SetColorAndOpacity(FSlateColor(Muted));
    DevicesPage=Tree->ConstructWidget<UVerticalBox>(); Parent->AddChild(DevicesPage);
    Label(Tree,DevicesPage,TEXT("両腕のEVT5を準備し、初回はペアリングモードにしてください。"),15);
    auto* StartRow=Row(Tree,DevicesPage);
    Button(Tree,StartRow,TEXT("開始・権限"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::Start);
    Button(Tree,StartRow,TEXT("機器を探す"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::Scan);
    DeviceText=Label(Tree,DevicesPage,TEXT("候補なし"));
    auto* DeviceRow=Row(Tree,DevicesPage);
    Button(Tree,DeviceRow,TEXT("前"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::PreviousDevice);
    Button(Tree,DeviceRow,TEXT("次"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::NextDevice);
    Button(Tree,DeviceRow,TEXT("接続・登録"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::Pair);
    Label(Tree,DevicesPage,TEXT("接続すると順番に振動します。表示された左腕・右腕へ装着してください。"),15);
    Button(Tree,Row(Tree,DevicesPage),TEXT("振動をやり直す"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::RepeatIdentify);
    Label(Tree,DevicesPage,TEXT("両上腕に電極1・2を装着し、機器のUSBを外してください。"),15);
    Button(Tree,Row(Tree,DevicesPage),TEXT("装着と左右を確認して開始"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::BeginSession);

    SessionPage=Tree->ConstructWidget<UVerticalBox>(); Parent->AddChild(SessionPage);
    Label(Tree,SessionPage,TEXT("本人と装着を確認し、片腕ずつ弱い刺激から校正します。"));
    Button(Tree,Row(Tree,SessionPage),TEXT("校正を開始"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::StartCalibration);

    CalibrationPage=Tree->ConstructWidget<UVerticalBox>(); Parent->AddChild(CalibrationPage);
    Label(Tree,CalibrationPage,TEXT("1秒以上あけて少しずつ上げ、感じ始めと快適な強さを記録します。"),15);
    CalibrationText=Label(Tree,CalibrationPage,TEXT("校正"));
    auto* SideRow=Row(Tree,CalibrationPage);
    Button(Tree,SideRow,TEXT("左腕"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::LeftArm);
    Button(Tree,SideRow,TEXT("右腕"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::RightArm);
    auto* PulseRow=Row(Tree,CalibrationPage);
    Button(Tree,PulseRow,TEXT("−"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::LowerCurrent);
    Button(Tree,PulseRow,TEXT("＋"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::RaiseCurrent);
    Button(Tree,PulseRow,TEXT("校正パルス"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::CalibrationPulse);
    auto* ComfortRow=Row(Tree,CalibrationPage);
    Button(Tree,ComfortRow,TEXT("今、感じ始めた"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::MarkThreshold);
    Button(Tree,ComfortRow,TEXT("今の値が快適・確定"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::ConfirmComfort);
    Button(Tree,Row(Tree,CalibrationPage),TEXT("両腕確定後、波形を準備"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::Prepare);

    PresetsPage=Tree->ConstructWidget<UVerticalBox>(); Parent->AddChild(PresetsPage);
    Label(Tree,PresetsPage,TEXT("左右それぞれ4種類を試し、本人が快適さを確認してください。"),15);
    PresetText=Label(Tree,PresetsPage,TEXT("波形"));
    auto* PresetSide=Row(Tree,PresetsPage);
    Button(Tree,PresetSide,TEXT("左腕"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::LeftArm);
    Button(Tree,PresetSide,TEXT("右腕"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::RightArm);
    Button(Tree,PresetSide,TEXT("前の波形"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::PreviousWave);
    Button(Tree,PresetSide,TEXT("次の波形"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::NextWave);
    auto* TrialRow=Row(Tree,PresetsPage);
    Button(Tree,TrialRow,TEXT("この波形を試す"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::TryWave);
    Button(Tree,TrialRow,TEXT("快適だった・確定"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::ConfirmWave);
    Button(Tree,Row(Tree,PresetsPage),TEXT("全8項目を確認して有効化"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::Arm);

    ArmedPage=Tree->ConstructWidget<UVerticalBox>(); Parent->AddChild(ArmedPage);
    Label(Tree,ArmedPage,TEXT("MoBits 有効。ゲームへ戻ると、射撃と被弾に反応します。"));
    StoppedPage=Tree->ConstructWidget<UVerticalBox>(); Parent->AddChild(StoppedPage);
    Label(Tree,StoppedPage,TEXT("出力は停止状態です。停止確認後にセッションを閉じ、必要なら再設定してください。"));
    Button(Tree,Row(Tree,StoppedPage),TEXT("停止を確認してセッション終了"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::CloseSession);

    auto* Bottom=Row(Tree,Parent);
    Button(Tree,Bottom,TEXT("停止"),true)->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::Stop);
    Button(Tree,Bottom,TEXT("サーバー設定"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::Back);
    Button(Tree,Bottom,TEXT("閉じる"))->OnClicked.AddDynamic(this,&UMRFPSMoBitsSetup::Finish);
}

UMoBitsQuestStandaloneComponent* UMRFPSMoBitsSetup::Runtime() const
{
    const auto* PC=Menu.IsValid()?Cast<AMRFPSPlayerController>(Menu->GetOwningPlayer()):nullptr;
    auto* Pawn=PC?PC->GetPawn<AMRFPSPawn>():nullptr;
    return Pawn && Pawn->IsLocallyControlled() && Pawn->GetSlot()>=0 ? Pawn->QuestHaptics.Get() : nullptr;
}
void UMRFPSMoBitsSetup::Note(const FString& Message) { NoteValue=Message; TextIfChanged(NoteText,Message); }
FString UMRFPSMoBitsSetup::CandidateId() const
{
    const auto List=Candidates(Runtime());
    return List.IsValidIndex(Candidate)?List[Candidate].Id:FString();
}
void UMRFPSMoBitsSetup::Refresh()
{
    auto* R=Runtime();
    if (!R) {
        TextIfChanged(StateText,TEXT("プレイヤーとしてサーバーに参加してから設定してください。"));
        Show(DevicesPage,false); Show(SessionPage,false); Show(CalibrationPage,false);
        Show(PresetsPage,false); Show(ArmedPage,false); Show(StoppedPage,false); return;
    }
    const auto Status=R->GetStatus();
    static int32 LastLoggedCode=0;
    static FString LastLoggedMessage;
    if (Status.Code!=LastLoggedCode || Status.Message!=LastLoggedMessage) {
        if (Status.Code!=0) UE_LOG(LogMRFPSMoBitsSetup,Warning,TEXT("MRFPS_MOBITS_ERROR code=%d phase=%s message=%s"),Status.Code,*Status.Phase,*Status.Message);
        LastLoggedCode=Status.Code; LastLoggedMessage=Status.Message;
    }
    if (bPulsePending && (Status.Phase!=TEXT("calibration") ||
        (Status.Arms.IsValidIndex(PulseSide) && Status.Arms[PulseSide].LastCalibrationUa!=PulsePreviousUa) ||
        (Status.Code!=0 && (Status.Code!=PulsePreviousCode || Status.Message!=PulsePreviousMessage)))) bPulsePending=false;
    const bool Stopped=Status.Phase==TEXT("stopped") && Status.bHasSession;
    const bool Session=Status.Phase==TEXT("sessionOpen");
    const bool Calibration=Status.Phase==TEXT("calibration");
    const bool Presets=Status.Phase==TEXT("prepared");
    const bool Armed=Status.Phase==TEXT("armed");
    Show(DevicesPage,!Stopped && !Session && !Calibration && !Presets && !Armed);
    Show(SessionPage,Session); Show(CalibrationPage,Calibration); Show(PresetsPage,Presets);
    Show(ArmedPage,Armed); Show(StoppedPage,Stopped);
    const FString Error=ErrorText(Status);
    TextIfChanged(StateText,Error.IsEmpty() ? (Armed?TEXT("準備完了"):Stopped?TEXT("停止中"):TEXT("MoBits セットアップ")) : Error);
    const auto List=Candidates(R);
    if (List.IsEmpty()) Candidate=0; else Candidate=FMath::Clamp(Candidate,0,List.Num()-1);
    // Keep assigned IDs stable across disconnects; never silently swap arms.
    if (!Status.bHasSession) {
        for (const auto& Device:List) {
            if (!Device.bConnected) continue;
            if (LeftId.IsEmpty() && Device.Id!=RightId) LeftId=Device.Id;
            else if (RightId.IsEmpty() && Device.Id!=LeftId) RightId=Device.Id;
        }
        const auto Connected=[&List](const FString& Id) {
            return !Id.IsEmpty() && List.ContainsByPredicate([&Id](const FMoBitsQuestDevice& D) { return D.Id==Id && D.bConnected; });
        };
        if (!bIdentifyPaused && !IdentifyRequest.IsValid() && FPlatformTime::Seconds()>=NextIdentifyAt) {
            if (!bLeftIdentified && !bLeftIdentifyFailed && Connected(LeftId)) StartIdentify(false);
            else if (bLeftIdentified && !bRightIdentified && !bRightIdentifyFailed && Connected(RightId)) StartIdentify(true);
        }
    }
    FString Current=TEXT("候補なし。開始後に機器を探してください。");
    if (List.IsValidIndex(Candidate)) {
        const auto& D=List[Candidate];
        Current=FString::Printf(TEXT("候補 %d/%d  %s  %s"),Candidate+1,List.Num(),*D.Id,
            D.bConnected?TEXT("接続済み"):D.bPairing?TEXT("登録可能"):D.bPaired?TEXT("再接続待ち"):TEXT("未接続"));
    }
    TextIfChanged(DeviceText,Current+FString::Printf(TEXT("\n左: %s　右: %s"),LeftId.IsEmpty()?TEXT("接続待ち"):bLeftIdentified?TEXT("振動送信済み"):TEXT("振動待ち"),RightId.IsEmpty()?TEXT("接続待ち"):bRightIdentified?TEXT("振動送信済み"):TEXT("振動待ち")));
    if (Status.Arms.IsValidIndex(Side)) {
        const auto& A=Status.Arms[Side];
        MaxDeliveredUa[Side]=FMath::Max(MaxDeliveredUa[Side],A.LastCalibrationUa);
        TextIfChanged(CalibrationText,FString::Printf(TEXT("%s腕　設定 %d µA　実際 %d µA\n感じ始め %d µA　快適上限 %d µA"),
            Side?TEXT("右"):TEXT("左"),TargetUa[Side],A.LastCalibrationUa,FeltUa[Side],A.CeilingUa));
        int32 Confirmed=0;
        for (const auto& Arm:Status.Arms) for (bool Value:Arm.Confirmed) if (Value) ++Confirmed;
        const bool IsConfirmed=A.Confirmed.IsValidIndex(Wave-1) && A.Confirmed[Wave-1];
        TextIfChanged(PresetText,FString::Printf(TEXT("%s腕：%s　%s　（確認 %d / 8）"),
            Side?TEXT("右"):TEXT("左"),WaveName(Wave),IsConfirmed?TEXT("確定済み"):TEXT("未確認"),Confirmed));
    }
}

void UMRFPSMoBitsSetup::Start() { if (auto* R=Runtime()) { ResumeIdentify(); R->Start(); Note(TEXT("許可を求められた場合、許可後にもう一度「開始」を押してください。")); } }
void UMRFPSMoBitsSetup::Scan() { if (auto* R=Runtime()) { R->ScanDevices(); Note(TEXT("ペアリングモードの機器を探しています。")); } }
void UMRFPSMoBitsSetup::MoveCandidate(int32 Direction) { const auto L=Candidates(Runtime()); if (!L.IsEmpty()) Candidate=(Candidate+Direction+L.Num())%L.Num(); Refresh(); }
void UMRFPSMoBitsSetup::PreviousDevice() { MoveCandidate(-1); }
void UMRFPSMoBitsSetup::NextDevice() { MoveCandidate(1); }
void UMRFPSMoBitsSetup::Pair()
{
    auto* R=Runtime(); if (!R) return; const auto L=Candidates(R);
    if (!L.IsValidIndex(Candidate)) { Note(TEXT("先に機器を探してください。")); return; }
    const auto& D=L[Candidate];
    if (D.bConnected) { Note(TEXT("接続済みです。左右は自動で振り分けます。")); return; }
    if (!D.bPairing) { Note(TEXT("未接続の場合は機器をペアリングモードにしてください。")); return; }
    if (R->PairDevice(D.Id)) Note(TEXT("接続中です。30秒ほどお待ちください。Questの確認が出たら許可してください。"));
}
void UMRFPSMoBitsSetup::CancelIdentify()
{
    bIdentifyPaused=true;
    if (IdentifyRequest.IsValid()) IdentifyRequest->Cancelled.store(true);
}
void UMRFPSMoBitsSetup::ResumeIdentify() { bIdentifyPaused=false; }
void UMRFPSMoBitsSetup::StartIdentify(bool Right)
{
    const FString Id=Right?RightId:LeftId;
    if (Id.IsEmpty() || IdentifyRequest.IsValid() || bIdentifyPaused) return;
    const auto Request=MakeShared<FMRFPSMoBitsIdentifyRequest,ESPMode::ThreadSafe>();
    IdentifyRequest=Request;
    Note(Right?TEXT("右腕：振動する機器を右腕に装着してください。"):
        TEXT("左腕：振動する機器を左腕に装着してください。"));
    TWeakObjectPtr<UMRFPSMoBitsSetup> WeakThis(this);
    MRFPSMoBitsIdentify::Start(Id,Request,[WeakThis,Request,Right](bool Sent,int32 Code) {
        auto* Self=WeakThis.Get();
        if (!Self || Self->IdentifyRequest.Get()!=&Request.Get()) return;
        Self->IdentifyRequest.Reset();
        if (Request->Cancelled.load()) return;
        if (Sent) {
            if (Right) Self->bRightIdentified=true; else Self->bLeftIdentified=true;
            Self->Note(Right?TEXT("振動した機器を右腕に装着してください。"):
                TEXT("振動した機器を左腕に装着してください。"));
            Self->NextIdentifyAt=FPlatformTime::Seconds()+2.0;
        } else {
            if (Right) Self->bRightIdentifyFailed=true; else Self->bLeftIdentifyFailed=true;
            Self->Note(FString::Printf(TEXT("識別振動を送れません (%d)。接続を確認し、振動をやり直してください。"),Code));
            UE_LOG(LogMRFPSMoBitsSetup,Warning,TEXT("MRFPS_MOBITS_IDENTIFY side=%s code=%d"),Right?TEXT("right"):TEXT("left"),Code);
        }
    });
}
void UMRFPSMoBitsSetup::RepeatIdentify() { RequestIdentify(); }
bool UMRFPSMoBitsSetup::RequestIdentify()
{
    auto* R=Runtime();
    if (!R || R->GetStatus().bHasSession || IdentifyRequest.IsValid()) {
        Note(TEXT("接続や振動の完了を待ってください。")); return false;
    }
    ResumeIdentify(); bLeftIdentified=bRightIdentified=bLeftIdentifyFailed=bRightIdentifyFailed=false;
    NextIdentifyAt=0; Note(TEXT("接続した機器を順番に振動させます。")); Refresh();
    return true;
}
void UMRFPSMoBitsSetup::BeginSession()
{
    auto* R=Runtime(); if (!R) return;
    const auto List=Candidates(R);
    const auto Connected=[&List](const FString& Id) { return !Id.IsEmpty() && List.ContainsByPredicate([&Id](const FMoBitsQuestDevice& D) { return D.Id==Id && D.bConnected; }); };
    if (!Connected(LeftId) || !Connected(RightId) || LeftId==RightId || !bLeftIdentified || !bRightIdentified || IdentifyRequest.IsValid()) {
        Note(TEXT("2台の接続と識別振動の完了を待ってください。")); return;
    }
    TargetUa[0]=TargetUa[1]=500; MaxDeliveredUa[0]=MaxDeliveredUa[1]=FeltUa[0]=FeltUa[1]=0; Side=0; bPulsePending=false;
    if (R->BeginSession(LeftId,RightId,true)) Note(TEXT("開始中です。機器の状態を確認しています。"));
}
void UMRFPSMoBitsSetup::StartCalibration() { if (auto* R=Runtime()) if (R->StartCalibration(true)) Note(TEXT("校正を開始しています。")); }
void UMRFPSMoBitsSetup::SelectSide(bool Right) { Side=Right?1:0; Refresh(); }
void UMRFPSMoBitsSetup::LeftArm() { SelectSide(false); }
void UMRFPSMoBitsSetup::RightArm() { SelectSide(true); }
void UMRFPSMoBitsSetup::LowerCurrent() { TargetUa[Side]=FMath::Max(100,TargetUa[Side]-500); Refresh(); }
void UMRFPSMoBitsSetup::RaiseCurrent()
{
    const int32 Limit=MaxDeliveredUa[Side]>0?FMath::Min(8000,MaxDeliveredUa[Side]+500):1000;
    TargetUa[Side]=FMath::Min(Limit,TargetUa[Side]+500); Refresh();
}
void UMRFPSMoBitsSetup::CalibrationPulse()
{
    auto* R=Runtime(); if (!R) return; const auto S=R->GetStatus();
    if (bPulsePending) { Note(TEXT("前のパルスの機器応答を待ってください。")); return; }
    if (!S.Arms.IsValidIndex(Side)) { Note(TEXT("校正の開始を待ってください。")); return; }
    const int32 Previous=S.Arms[Side].LastCalibrationUa;
    if (Previous>0 && FMath::Abs(TargetUa[Side]-Previous)<25) { Note(TEXT("別の値を選んでから送ってください。")); return; }
    if (R->CalibrationPulse(Side==1,TargetUa[Side],true)) {
        bPulsePending=true; PulseSide=Side; PulsePreviousUa=Previous;
        PulsePreviousCode=S.Code; PulsePreviousMessage=S.Message;
        Note(TEXT("機器応答を待ち、本人が実際の刺激を確認してください。"));
    }
}
void UMRFPSMoBitsSetup::MarkThreshold()
{
    auto* R=Runtime(); if (!R) return; const auto S=R->GetStatus();
    if (bPulsePending) { Note(TEXT("パルスの機器応答を待ってください。")); return; }
    if (!S.Arms.IsValidIndex(Side) || S.Arms[Side].LastCalibrationUa<=0) { Note(TEXT("先に校正パルスを送ってください。")); return; }
    FeltUa[Side]=S.Arms[Side].LastCalibrationUa; Note(TEXT("感じ始めた値を記録しました。")); Refresh();
}
void UMRFPSMoBitsSetup::ConfirmComfort()
{
    auto* R=Runtime(); if (!R) return; const auto S=R->GetStatus();
    if (bPulsePending) { Note(TEXT("パルスの機器応答を待ってください。")); return; }
    if (!S.Arms.IsValidIndex(Side) || FeltUa[Side]<=0 || S.Arms[Side].LastCalibrationUa<FeltUa[Side]) {
        Note(TEXT("感じ始めた値と、実際に届いた快適な値を確認してください。")); return;
    }
    if (R->ConfirmCalibration(Side==1,FeltUa[Side],S.Arms[Side].LastCalibrationUa,true)) Note(TEXT("この腕の快適な上限を確認しています。"));
}
void UMRFPSMoBitsSetup::Prepare() { if (auto* R=Runtime()) if (R->PrepareDefaultPresets()) Note(TEXT("両腕の波形を準備しています。")); }
void UMRFPSMoBitsSetup::MoveWave(int32 Direction) { Wave=((Wave-1+Direction+4)%4)+1; Refresh(); }
void UMRFPSMoBitsSetup::PreviousWave() { MoveWave(-1); }
void UMRFPSMoBitsSetup::NextWave() { MoveWave(1); }
void UMRFPSMoBitsSetup::TryWave() { if (auto* R=Runtime()) if (R->TryPreset(Side==1,static_cast<EMoBitsHapticWave>(Wave),true)) Note(TEXT("実際に届いた後、本人が快適さを確認してください。")); }
void UMRFPSMoBitsSetup::ConfirmWave() { if (auto* R=Runtime()) if (R->ConfirmPreset(Side==1,static_cast<EMoBitsHapticWave>(Wave),true)) Note(TEXT("この波形の確認を記録しています。")); }
void UMRFPSMoBitsSetup::Arm() { if (auto* R=Runtime()) if (R->ArmPresets(true)) Note(TEXT("全8項目の確認後、有効化します。")); }
void UMRFPSMoBitsSetup::Stop() { CancelIdentify(); if (auto* R=Runtime()) { bPulsePending=false; R->StopAll(); Note(TEXT("停止を要求しました。")); } }
void UMRFPSMoBitsSetup::CloseSession() { if (auto* R=Runtime()) if (R->CloseSession()) Note(TEXT("停止確認後にセッションを閉じます。")); }
void UMRFPSMoBitsSetup::Back() { if (Menu.IsValid()) Menu->Back(); }
void UMRFPSMoBitsSetup::Finish() { if (Menu.IsValid()) Menu->CloseLogin(); }
